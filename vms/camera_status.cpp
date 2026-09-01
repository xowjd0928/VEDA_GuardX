#include "camera_status.h"
#include "alert_feed.h"
#include "credentials.h"
#include "sunapi_request.h"

#include <QAuthenticator>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUrl>

// 카메라 자원·앱 상태 폴러 (opensdk.cgi — SUNAPI 2.6.6, 실측 2026-08-05)
//
// 부하 감각: appstatus 1/s + apps 0.2/s ≈ 1.2 req/s 순증 — read-only GET이라
// RTSP SETUP 버스트(세션 굶주림의 원인)와는 무관하지만, 카메라 제어 채널이
// 예민하다는 08-05 교훈대로 겹침 방지·타임아웃·킬스위치를 전부 갖춘다.

namespace {

/** @brief 킬스위치 — camera_status_poll=0 이면 폴러가 아예 돌지 않는다 */
bool poll_enabled()
{
    static const bool v = QSettings("GuardX", "VMS")
                              .value("camera_status_poll", 1).toInt() != 0;
    return v;
}

constexpr int POLL_MS = 1000;        ///< appstatus 주기 (사용자 확정 1초)
constexpr int APPS_EVERY_TICKS = 5;  ///< apps view = 5초 (상태 변화 느림)
constexpr int TIMEOUT_MS = 2500;     ///< 지나면 실패로 치고 다음 틱 (§4d)
constexpr int OFFLINE_AFTER = 3;     ///< 연속 실패 3회+ = Offline
constexpr qint64 REBOOT_GIVEUP_MS = 120000;  ///< 재부팅 복구 대기 상한

/** @brief 카메라가 "34" / 34 / 34.0 어느 쪽으로 줘도 int로 */
int json_int(const QJsonValue &v, int def = -1)
{
    if (v.isDouble())
        return int(v.toDouble() + 0.5);
    bool ok = false;
    const double d = v.toVariant().toString().toDouble(&ok);
    return ok ? int(d + 0.5) : def;
}

double json_num(const QJsonValue &v, double def = -1.0)
{
    if (v.isDouble())
        return v.toDouble();
    bool ok = false;
    const double d = v.toVariant().toString().toDouble(&ok);
    return ok ? d : def;
}

/** @brief true/"True"/"true" 전부 수용 (SUNAPI는 문자열 불리언을 자주 쓴다) */
bool json_bool(const QJsonValue &v)
{
    if (v.isBool())
        return v.toBool();
    return v.toVariant().toString().compare(QLatin1String("true"),
                                            Qt::CaseInsensitive) == 0;
}

/** @brief ControlForbidden — 배열이든 문자열이든 원문을 한 줄로 보존 */
QString json_forbidden(const QJsonValue &v)
{
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue &e : v.toArray())
            parts << e.toVariant().toString();
        return parts.join(',');
    }
    return v.toVariant().toString();
}

/** @brief ISO 8601 기간 "P0Y0M0DT8H3M11S" → 초. 못 읽으면 -1 */
qint64 parse_duration_s(const QString &iso)
{
    static const QRegularExpression re(
        "^P(?:(\\d+)Y)?(?:(\\d+)M)?(?:(\\d+)D)?"
        "(?:T(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?)?$");
    const auto m = re.match(iso.trimmed());
    if (!m.hasMatch())
        return -1;
    const auto n = [&m](int i) { return m.captured(i).toLongLong(); };
    // 년/월은 카메라 가동시간에선 사실상 0 — 근사(365/30일)로 충분
    return ((n(1) * 365 + n(2) * 30 + n(3)) * 24 + n(4)) * 3600
           + n(5) * 60 + n(6);
}

/** @brief 응답 루트에서 "Apps" 배열 — 키가 다르면 첫 배열로 폴백 */
QJsonArray find_apps_array(const QJsonObject &root)
{
    if (root.value("Apps").isArray())
        return root.value("Apps").toArray();
    for (auto it = root.begin(); it != root.end(); ++it)
        if (it.value().isArray())
            return it.value().toArray();
    return {};
}

const char *state_name(CameraStatus::LinkState s)
{
    switch (s) {
    case CameraStatus::LinkState::Online:    return "Online";
    case CameraStatus::LinkState::Stale:     return "Stale";
    case CameraStatus::LinkState::Offline:   return "Offline";
    case CameraStatus::LinkState::Rebooting: return "Rebooting";
    }
    return "?";
}

} // namespace

CameraStatus *CameraStatus::instance()
{
    static CameraStatus status;
    return &status;
}

CameraStatus::CameraStatus(QObject *parent)
    : QObject(parent)
{
    if (!poll_enabled()) {
        qInfo() << "[CamStatus] 폴링 비활성 (레지스트리 camera_status_poll=0)"
                   " — top_bar 자원 표시는 미확인(—)으로 남는다";
        return;
    }

    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);

    // SUNAPI Digest 인증 — CameraTuner/DetectionFeed와 같은 패턴.
    // 같은 요청에 두 번 불렸다면 자격이 틀린 것 — 무한 재시도 방지.
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[CamStatus] 인증 거부 — 계정/비밀번호 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &CameraStatus::tick);
    m_timer->start(POLL_MS);

    request_appstatus();
    request_apps();
}

void CameraStatus::tick()
{
    ++m_tick;
    request_appstatus();
    if (m_tick % APPS_EVERY_TICKS == 0) {
        request_apps();
        report_stats();
    }
}

void CameraStatus::enter_reboot_mode()
{
    m_reboot_started_ms = QDateTime::currentMSecsSinceEpoch();
    m_fail_streak = 0;
    set_state(LinkState::Rebooting);
}

// ---------------------------------------------------------------- appstatus

void CameraStatus::request_appstatus()
{
    if (!m_net || m_pending_status)
        return;  // 킬스위치로 꺼져 있거나 이전 요청 미완 — 건너뛴다

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/opensdk.cgi");
    url.setQuery("msubmenu=appstatus&action=view");

    QNetworkRequest req = sunapi_request(url, TIMEOUT_MS);
    req.setRawHeader("Accept", "application/json");

    m_pending_status = m_net->get(req);
    connect(m_pending_status, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_pending_status;
        m_pending_status = nullptr;
        handle_appstatus(reply);
        reply->deleteLater();
    });
}

void CameraStatus::handle_appstatus(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        on_poll_failure(reply->errorString());
        return;
    }

    const QByteArray body = reply->readAll();
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    if (!root.contains("TotalCPUUsage")) {
        // 응답은 왔는데 계약이 다르다 — 링크 실패와 구분해 원문을 남긴다
        static bool logged = false;
        if (!logged) {
            logged = true;
            qWarning() << "[CamStatus] appstatus 응답 형식이 예상과 다름:"
                       << body.left(300);
        }
        on_poll_failure("appstatus 파싱 실패");
        return;
    }

    m_res.cpu = json_int(root.value("TotalCPUUsage"));
    m_res.mem = json_int(root.value("TotalMemoryUsage"));
    m_res.npu = json_int(root.value("TotalNPUUsage"));
    m_res.ram_free_mb = json_num(root.value("FreeRamSpace"));
    m_res.ram_total_mb = json_num(root.value("TotalRamSpace"));
    m_res.storage_free_mb = json_num(root.value("FreeStorageSpace"));
    m_res.storage_total_mb = json_num(root.value("TotalStorageSpace"));

    // 앱별 자원을 목록(m_apps)에 병합 — 목록이 아직 안 왔으면 다음에 실린다
    const QJsonArray apps = find_apps_array(root);
    for (const QJsonValue &v : apps) {
        const QJsonObject o = v.toObject();
        const QString id = o.value("AppID").toString();
        for (CameraAppInfo &app : m_apps) {
            if (app.id != id)
                continue;
            app.cpu = json_int(o.value("CPUUsage"));
            app.mem = json_int(o.value("MemoryUsage"));
            app.npu = json_int(o.value("NPUUsage"));
            app.threads = json_int(o.value("ThreadsCount"));
            app.ram_mb = json_num(o.value("UsedRamSpace"));
            app.uptime_s = parse_duration_s(o.value("Duration").toString());
            break;
        }
    }

    on_poll_success();
    publish_resource_alert();
    emit resources_changed(m_res);
    if (!m_apps.isEmpty())
        emit apps_changed(m_apps);
}

// ---------------------------------------------------------------- apps view

void CameraStatus::request_apps()
{
    if (!m_net || m_pending_apps)
        return;

    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/opensdk.cgi");
    url.setQuery("msubmenu=apps&action=view");

    QNetworkRequest req = sunapi_request(url, TIMEOUT_MS);
    req.setRawHeader("Accept", "application/json");

    m_pending_apps = m_net->get(req);
    connect(m_pending_apps, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_pending_apps;
        m_pending_apps = nullptr;
        handle_apps(reply);
        reply->deleteLater();
    });
}

void CameraStatus::handle_apps(QNetworkReply *reply)
{
    // 링크 상태 분류는 1초 심장박동(appstatus)의 몫 — 여기선 로그만
    if (reply->error() != QNetworkReply::NoError) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            qWarning() << "[CamStatus] apps 목록 조회 실패:" << reply->errorString();
        }
        return;
    }

    const QByteArray body = reply->readAll();
    const QJsonArray arr = find_apps_array(QJsonDocument::fromJson(body).object());
    if (arr.isEmpty()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            qWarning() << "[CamStatus] apps 응답 형식이 예상과 다름:" << body.left(300);
        }
        return;
    }

    // 새 목록을 만들고 기존 자원값을 이어받는다 (appstatus가 1초 뒤 덮어씀)
    QVector<CameraAppInfo> next;
    next.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        CameraAppInfo app;
        app.id = o.value("AppID").toString();
        app.version = o.value("Version").toVariant().toString();
        app.status = o.value("Status").toString();
        app.priority = o.value("Priority").toString();
        app.installed_date = o.value("InstalledDate").toVariant().toString();
        app.auto_start = json_bool(o.value("AutoStart"));
        app.is_default = json_bool(o.value("IsDefault"));
        app.control_forbidden = json_forbidden(o.value("ControlForbidden"));
        for (const CameraAppInfo &old : std::as_const(m_apps)) {
            if (old.id != app.id)
                continue;
            app.cpu = old.cpu;
            app.mem = old.mem;
            app.npu = old.npu;
            app.threads = old.threads;
            app.ram_mb = old.ram_mb;
            app.uptime_s = old.uptime_s;
            break;
        }
        next.append(app);
    }
    m_apps = next;
    publish_app_alerts();
    emit apps_changed(m_apps);
}

// ------------------------------------------------------------- 상태 기계

void CameraStatus::on_poll_success()
{
    m_fail_streak = 0;
    m_last_ok_ms = QDateTime::currentMSecsSinceEpoch();
    ++m_stat_ok;
    if (m_state == LinkState::Rebooting)
        qInfo() << "[CamStatus] 재부팅 복구 확인 — 폴 재개";
    set_state(LinkState::Online);
}

void CameraStatus::on_poll_failure(const QString &why)
{
    ++m_fail_streak;
    ++m_stat_fail;

    if (m_state == LinkState::Rebooting) {
        // 재부팅 중 실패는 정상 — 단 2분 넘게 안 돌아오면 진짜 문제다
        if (QDateTime::currentMSecsSinceEpoch() - m_reboot_started_ms
            > REBOOT_GIVEUP_MS) {
            qWarning() << "[CamStatus] 재부팅 복구 대기 초과(2분) — 오프라인 전환";
            set_state(LinkState::Offline);
        }
        return;
    }

    const LinkState next = m_fail_streak >= OFFLINE_AFTER ? LinkState::Offline
                                                          : LinkState::Stale;
    if (next != m_state)  // 전이 때 1회만 로그 — 실패 로그 폭주 금지 (§4d)
        qWarning().noquote() << QString("[CamStatus] 폴 실패 %1회 → %2 (%3)")
                                    .arg(m_fail_streak)
                                    .arg(state_name(next), why);
    set_state(next);
}

void CameraStatus::set_state(LinkState s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit link_state_changed(s);

    // §4c-2: 오프라인 = Critical (전 스트림·박스 위험). 3회+ 연속 실패라는
    // 판정 자체가 히스테리시스라 깜빡이는 링크의 경보 난사를 막는다.
    // Rebooting은 의도된 부재라 경보 없음, Stale은 아직 판단 보류.
    if (s == LinkState::Offline)
        AlertFeed::instance()->raise_device_alert(
            AlertFeed::DEV_LINK, AlertFeed::Critical,
            "Camera offline - status polls failing in a row");
    else if (s == LinkState::Online)
        AlertFeed::instance()->raise_device_alert(AlertFeed::DEV_LINK,
                                                  AlertFeed::None, {});
}

void CameraStatus::publish_app_alerts()
{
    // 파이프라인을 실제로 떠받치는 앱 — 죽으면 Critical (기획서 §4c-2 표)
    static const QSet<QString> VITAL = {
        QStringLiteral("test"), QStringLiteral("juan_application"),
        QStringLiteral("WiseAI")};

    for (const CameraAppInfo &app : std::as_const(m_apps)) {
        const QString prev = m_prev_status.value(app.id);
        if (app.status == QLatin1String("Running")) {
            m_dead_apps.remove(app.id);
            m_expected_stops.remove(app.id);  // 다시 떴다 — 기대 소거
        } else if (prev == QLatin1String("Running")
                   && app.status == QLatin1String("Stopped")
                   && !m_expected_stops.contains(app.id)) {
            m_dead_apps.insert(app.id);  // 사용자가 안 껐는데 죽었다
        }
        m_prev_status.insert(app.id, app.status);
    }

    AlertFeed::Severity sev = AlertFeed::None;
    QStringList msgs;
    for (const QString &id : std::as_const(m_dead_apps)) {
        const bool vital = VITAL.contains(id);
        sev = qMax(sev, vital ? AlertFeed::Critical : AlertFeed::Warn);
        msgs << QString("App %1 stopped unexpectedly%2")
                    .arg(id, vital ? QString(" - hole in the analytics pipeline")
                                   : QString());
    }
    for (const CameraAppInfo &app : std::as_const(m_apps))
        if (!app.auto_start && !app.is_default) {
            sev = qMax(sev, AlertFeed::Warn);
            msgs << QString("App %1 has AutoStart off - it will not come back "
                            "after a power cut").arg(app.id);
        }

    AlertFeed::instance()->raise_device_alert(AlertFeed::DEV_APP, sev,
                                              msgs.join(QStringLiteral(" · ")));
}

void CameraStatus::publish_resource_alert()
{
    const bool hot = m_res.cpu > 90 || m_res.mem > 90 || m_res.npu > 90;
    m_hot_seconds = hot ? m_hot_seconds + 1 : 0;

    if (!m_res_alert && m_hot_seconds >= 10) {
        // 순간 스파이크(NPU는 일상)는 10초 지속 게이트가 거른다
        m_res_alert = true;
        AlertFeed::instance()->raise_device_alert(
            AlertFeed::DEV_RES, AlertFeed::Warn,
            QString("Device resources over threshold - CPU %1 · MEM %2 · NPU %3 "
                    "(10 s+, precursor to a latency blowup)")
                .arg(m_res.cpu).arg(m_res.mem).arg(m_res.npu));
    } else if (m_res_alert && !hot) {
        m_res_alert = false;
        AlertFeed::instance()->raise_device_alert(AlertFeed::DEV_RES,
                                                  AlertFeed::None, {});
    }
}

void CameraStatus::report_stats()
{
    int running = 0;
    for (const CameraAppInfo &app : std::as_const(m_apps))
        if (app.status == QLatin1String("Running"))
            ++running;

    qInfo().noquote()
        << QString("[CamStatus] cpu %1 mem %2 npu %3 | apps %4 (Running %5) "
                   "| poll ok %6 fail %7 | %8")
               .arg(m_res.cpu).arg(m_res.mem).arg(m_res.npu)
               .arg(m_apps.size()).arg(running)
               .arg(m_stat_ok).arg(m_stat_fail)
               .arg(state_name(m_state));
    m_stat_ok = m_stat_fail = 0;
}
