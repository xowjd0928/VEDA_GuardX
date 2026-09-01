#include "site_config.h"

#include "auth.h"
#include "calibration_store.h"
#include "mqtt_link.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSettings>

namespace {
const char *TOPIC_STATE = "guardx/db/rpib/site_config";
const char *TOPIC_CMD = "guardx/db/rpib/cmd/set_site_config";

// QSettings 키. 전역 캐시는 계정 무관(현장 공통값의 사본이므로),
// 오버라이드만 계정별이다.
const char *KEY_NAME = "site/name";
const char *KEY_GLOBAL = "site/calibration_global";
const char *KEY_OVERRIDE_FMT = "site/calibration_local_%1";

QSettings settings()
{
    return QSettings("GuardX", "VMS");
}

QJsonObject read_json_setting(const QString &key)
{
    QSettings s("GuardX", "VMS");
    const QByteArray raw = s.value(key).toByteArray();
    if (raw.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void write_json_setting(const QString &key, const QJsonObject &obj)
{
    QSettings s("GuardX", "VMS");
    if (obj.isEmpty())
        s.remove(key);
    else
        s.setValue(key, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
} // namespace

const char *SiteConfig::DEFAULT_SITE_NAME = "Terminal West · Sector B";

SiteConfig *SiteConfig::instance()
{
    static SiteConfig inst;
    return &inst;
}

SiteConfig::SiteConfig(QObject *parent) : QObject(parent) {}

void SiteConfig::init()
{
    if (m_started)
        return;
    m_started = true;

    // 캐시 복원 — retained 가 오기 전(오프라인 포함)에도 마지막 값으로 그린다
    m_site_name = settings().value(KEY_NAME).toString();
    m_global_calib = read_json_setting(KEY_GLOBAL);

    MqttLink::instance()->subscribe(TOPIC_STATE, [this](const QByteArray &p) {
        on_payload(p);
    });

    // 캘리브레이션 선택은 **로그인한 계정**에 따라 갈린다(운영자 오버라이드).
    // 로그인 전에는 어차피 CROWD 화면이 없으니 전역값이면 충분하고,
    // 로그인/로그아웃 순간마다 다시 고른다.
    connect(Auth::instance(), &Auth::state_changed, this,
            [this] { apply_effective_calibration(); });
    apply_effective_calibration();
}

QString SiteConfig::site_name() const
{
    return m_site_name.isEmpty() ? QString::fromUtf8(DEFAULT_SITE_NAME)
                                 : m_site_name;
}

void SiteConfig::on_payload(const QByteArray &payload)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[SiteConfig] payload 파싱 실패 — 무시:" << perr.errorString();
        return;
    }
    const QJsonObject o = doc.object();

    // 내용이 같은 재발행(폴러 기동 시 DB→retained 재방송)은 조용히 지나간다
    const QString at = o.value("updated_at").toString();
    if (!at.isEmpty() && at == m_last_updated_at)
        return;
    m_last_updated_at = at;

    // ⚠ 없는 키 = "설정 안 됨"(계약) — 지우지 않고 그대로 둔다.
    //   서버가 site_name 만 알고 있으면 calibration 필드 자체가 안 온다.
    if (o.contains("site_name")) {
        const QString name = o.value("site_name").toString();
        if (name != m_site_name) {
            m_site_name = name;
            settings().setValue(KEY_NAME, name);
            emit site_name_changed();
        }
    }

    if (o.contains("calibration")) {
        // ⚠ **검증 전에는 캐시에 쓰지 않는다** (08-12 실측). 서버는 calibration 을
        //    해석하지 않고 통짜 보관하므로(계약) 여기 오는 값이 캘리브레이션이
        //    아닐 수 있다 — 실제로 다른 세션의 계약 시험 payload
        //    (`{"reply_to":…,"token":…}`)가 그대로 캐시에 굳어, 재시작마다
        //    파싱 실패 경고를 뱉었다. 통과한 것만 남긴다.
        const QJsonObject calib = o.value("calibration").toObject();
        QString err;
        if (calib.isEmpty()) {
            // 빈 객체 = "지움"(계약). 캐시를 비우고 화면은 그대로 둔다 —
            // 이미 그려진 평면도를 원격이 조용히 지우게 하지 않는다.
            m_global_calib = {};
            write_json_setting(KEY_GLOBAL, {});
        } else if (CalibrationStore::is_valid(calib, &err)) {
            m_global_calib = calib;
            write_json_setting(KEY_GLOBAL, calib);
            apply_effective_calibration();   // 오버라이드가 있으면 화면은 안 바뀐다
        } else {
            qWarning().noquote()
                << "[SiteConfig] 받은 calibration 이 유효하지 않다 — 캐시·화면 둘 다"
                   " 그대로 둔다:" << err;
        }
    }

    qInfo().noquote() << QString("[SiteConfig] 전역 설정 수신 (updated_at %1)")
                             .arg(at.isEmpty() ? "-" : at);
}

QString SiteConfig::override_key() const
{
    const QString user = Auth::instance()->username();
    return user.isEmpty() ? QString()
                          : QString::fromLatin1(KEY_OVERRIDE_FMT).arg(user);
}

bool SiteConfig::has_local_override() const
{
    const QString key = override_key();
    return !key.isEmpty() && !read_json_setting(key).isEmpty();
}

void SiteConfig::apply_effective_calibration()
{
    // 우선순위: 이 계정의 오버라이드 > 전역. 뭘 고르든 **빈 것이면 안 건드린다**
    // — 시작 직후 아무것도 없는 상태에서 기존 평면도(파일로 불러온 것)를
    // 지우는 일이 없어야 한다.
    const QString key = override_key();
    const QJsonObject local = key.isEmpty() ? QJsonObject{} : read_json_setting(key);
    const QJsonObject &pick = !local.isEmpty() ? local : m_global_calib;
    if (pick.isEmpty())
        return;
    // 캐시에 예전 형식·오염된 값이 남아 있을 수 있다(검증 전에 저장하던 시절의
    // 잔재). 매 기동 경고를 뱉는 대신 조용히 건너뛴다 — 화면은 기존 상태 유지.
    if (!CalibrationStore::is_valid(pick))
        return;

    const QString source = !local.isEmpty()
                               ? QString::fromUtf8("local override (%1)").arg(key)
                               : QString::fromUtf8("site_config");
    if (!CalibrationStore::instance()->load_json(pick, source))
        qWarning().noquote() << "[SiteConfig] 캘리브레이션 적용 실패:"
                             << CalibrationStore::instance()->error();
}

void SiteConfig::save_site_name(const QString &name, Done done)
{
    QJsonObject params;
    params["cmd"] = "set_site_config";
    params["site_name"] = name;
    Auth::attach_token(params);

    MqttLink::instance()->request(
        TOPIC_CMD, params,
        [done](const QJsonObject &) { if (done) done(true, QString()); },
        [done](const QString &err) { if (done) done(false, err); },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [done](const QJsonObject &reply) {
            Auth::note_write_reject(reply);
            if (done) done(false, reply.value("reason").toString());
        });
    // 성공 확인은 응답이 아니라 **retained 재방송**이 하는 것(계약의 set_zone
    // 패턴) — 화면 문구 갱신은 site_name_changed() 가 맡는다.
}

void SiteConfig::publish_calibration(const QJsonObject &calib, Done done)
{
    QJsonObject params;
    params["cmd"] = "set_site_config";
    params["calibration"] = calib;
    Auth::attach_token(params);

    MqttLink::instance()->request(
        TOPIC_CMD, params,
        [done](const QJsonObject &) { if (done) done(true, QString()); },
        [done](const QString &err) { if (done) done(false, err); },
        MqttLink::DEFAULT_TIMEOUT_MS,
        [done](const QJsonObject &reply) {
            Auth::note_write_reject(reply);
            if (done) done(false, reply.value("reason").toString());
        });
}

void SiteConfig::save_local_calibration(const QJsonObject &calib)
{
    const QString key = override_key();
    if (key.isEmpty()) {
        // 로그인 없이 부를 일이 없는 경로지만, 생기면 조용히 삼키지 않는다
        qWarning() << "[SiteConfig] 로그인 전에는 로컬 오버라이드를 저장할 수 없다";
        return;
    }
    write_json_setting(key, calib);
}

void SiteConfig::clear_local_override()
{
    const QString key = override_key();
    if (key.isEmpty())
        return;
    write_json_setting(key, {});
    apply_effective_calibration();   // 전역값으로 되돌아간다 (있으면)
}
