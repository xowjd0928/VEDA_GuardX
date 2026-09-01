#include "system_panel.h"
#include "auth.h"
#include "camera_control.h"
#include "camera_status.h"
#include "credentials.h"
#include "panel_chrome.h"
#include "sunapi_request.h"
#include "theme.h"

#include <QAuthenticator>
#include <QButtonGroup>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int DATE_POLL_MS = 30000;  ///< 시계는 거의 안 변한다 (§5)

// role은 Theme 팔레트 **슬롯을 가리키는 포인터**다 — 값을 복사하면 테마가
// 바뀌어도 옛 색이 남는다. 호출부는 반드시 &Theme::xxx 를 넘길 것.
QPushButton *make_outline_btn(const QString &text, const QColor *role,
                              QWidget *parent, int w = 150)
{
    auto *b = new QPushButton(text, parent);
    b->setFixedSize(Theme::px(w), Theme::px(26));  // 글자와 같은 배율
    b->setFont(Theme::ui_font(11, 600));
    b->setCursor(Qt::PointingHandCursor);
    Theme::restyle(b, [=] {
        return QString("QPushButton { background:transparent; border:1px solid %1;"
                " border-radius:2px; color:%1; }"
                "QPushButton:hover { background:%2; }"
                "QPushButton:disabled { border-color:%3; color:%3; }")
            .arg(role->name(), Theme::elevated2.name(), Theme::textFaint.name());
    });
    return b;
}

} // namespace

SystemPanel::SystemPanel(CameraControl *control, QWidget *parent)
    : QWidget(parent), m_control(control)
{
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[SystemPanel] 인증 거부 — 계정 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 8, 0, 0);
    col->setSpacing(14);
    col->addWidget(build_clock_card());
    col->addWidget(build_profile_card());
    col->addWidget(build_log_card());
    col->addWidget(build_danger_card());

    m_date_timer = new QTimer(this);
    m_date_timer->setInterval(DATE_POLL_MS);
    connect(m_date_timer, &QTimer::timeout, this, &SystemPanel::fetch_date);
}

void SystemPanel::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    fetch_date();
    m_date_timer->start();
}

void SystemPanel::hideEvent(QHideEvent *ev)
{
    QWidget::hideEvent(ev);
    m_date_timer->stop();
}

// ---------------------------------------------------------------- UI 구축

QWidget *SystemPanel::build_clock_card()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    m_btn_ntp = make_outline_btn("Resync NTP", &Theme::accent, w, 110);
    Auth::bind(m_btn_ntp, Auth::Action::CameraSystem);
    connect(m_btn_ntp, &QPushButton::clicked, this, [this] {
        if (!Auth::can(Auth::Action::CameraSystem))
            return;   // 백스톱 — 버튼 잠금은 표면이다
        // SyncType=NTP를 다시 set하면 재동기가 걸린다 (Manual이면 NTP 전환).
        // 08-04 교훈: 스트리밍(RTCP) 시계는 별개라 재부팅만이 재앵커 —
        // 이 버튼은 시스템 시계(NTP)까지만 만진다.
        QUrl url = Credentials::camera_base_url();
        url.setPath("/stw-cgi/system.cgi");
        url.setQuery("msubmenu=date&action=set&SyncType=NTP");
        QNetworkRequest req = sunapi_request(url, 4000);
        QNetworkReply *reply = m_net->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            qInfo() << "[SystemPanel] NTP 재동기 요청:"
                    << (reply->error() == QNetworkReply::NoError
                            ? "수락" : reply->errorString());
            QTimer::singleShot(1200, this, &SystemPanel::fetch_date);
        });
    });
    col->addWidget(PanelChrome::header(QStringLiteral("Clock · NTP"),
                                       QStringLiteral("system.cgi date · 30s"),
                                       w, m_btn_ntp));

    m_clock_body = new QLabel("fetching...", w);
    m_clock_body->setFont(Theme::mono_font(11));
    m_clock_body->setTextFormat(Qt::RichText);  // 첫 글자가 평문이라 자동감지 실패
    Theme::restyle(m_clock_body, [=] {
        return QString("color:%1; padding:8px 2px;")
                                    .arg(Theme::textMid.name());
    });
    col->addWidget(m_clock_body);
    return w;
}

QWidget *SystemPanel::build_profile_card()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto *btn = make_outline_btn("Refresh", &Theme::textMuted, w, 90);
    connect(btn, &QPushButton::clicked, this, &SystemPanel::fetch_profile_access);
    // 부제는 짧게 — 08-19 CAMERA 개편으로 이 패널이 우측 열(≒460px)로
    // 들어갔다. "profileaccessinfo · RTSP load" 는 그 폭에서 잘린다.
    col->addWidget(PanelChrome::header(
        QStringLiteral("Viewing Sessions"),
        QStringLiteral("RTSP load"), w, btn));

    m_profile_body = new QLabel("not fetched yet - press Refresh", w);
    m_profile_body->setFont(Theme::mono_font(10));
    Theme::restyle(m_profile_body, [=] {
        return QString("color:%1; padding:8px 2px;")
                                      .arg(Theme::textMid.name());
    });
    m_profile_body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_profile_body);
    return w;
}

QWidget *SystemPanel::build_log_card()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(8);

    // 로그 종류 선택 — 누르면 즉시 불러온다 (읽기전용이라 부담 없음)
    auto *seg = new QWidget(w);
    auto *seg_row = new QHBoxLayout(seg);
    seg_row->setContentsMargins(0, 0, 0, 0);
    seg_row->setSpacing(6);
    struct { const char *label; const char *submenu; } TYPES[] = {
        {"System log", "systemlog"},
        {"Access log", "accesslog"},
        {"Event log", "eventlog"},
    };
    for (const auto &t : TYPES) {
        auto *b = make_outline_btn(t.label, &Theme::textMuted, seg, 100);
        const QString submenu = QLatin1String(t.submenu);
        connect(b, &QPushButton::clicked, this,
                [this, submenu] { fetch_logs(submenu); });
        seg_row->addWidget(b);
    }
    seg_row->addStretch(1);

    col->addWidget(PanelChrome::header(QStringLiteral("Device Logs"),
                                       QStringLiteral("read only · last 40 lines"),
                                       w));
    col->addWidget(seg);

    m_log_view = new QPlainTextEdit(w);
    m_log_view->setReadOnly(true);
    m_log_view->setFixedHeight(170);
    m_log_view->setFont(Theme::mono_font(10));
    m_log_view->setFrameShape(QFrame::NoFrame);
    Theme::restyle(m_log_view, [=] {
        return QString("QPlainTextEdit { background:%1; color:%2; border:1px solid %3;"
                " border-radius:2px; padding:6px; }")
            .arg(Theme::elevated.name(), Theme::textMid.name(),
                 Theme::border.name());
    });
    m_log_view->setPlaceholderText("pick a log type");
    col->addWidget(m_log_view);
    return w;
}

QWidget *SystemPanel::build_danger_card()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 12);
    col->setSpacing(8);

    col->addWidget(PanelChrome::header(QStringLiteral("Backup · Danger Zone"),
                                       QStringLiteral("isolated actions - two-step confirm"),
                                       w));

    auto *row = new QHBoxLayout;
    row->setSpacing(10);

    m_btn_backup = make_outline_btn("Save config backup", &Theme::green, w, 150);
    m_btn_backup->setToolTip(
        "Downloads configbackup into your Downloads folder - insurance for a "
        "day like the 08-03 factory-reset accident");
    Auth::bind(m_btn_backup, Auth::Action::CameraSystem);
    connect(m_btn_backup, &QPushButton::clicked, this, &SystemPanel::save_backup);
    row->addWidget(m_btn_backup);

    auto *reboot = make_outline_btn("Reboot camera...", &Theme::alarm, w, 150);
    reboot->setToolTip(
        "Re-anchors the streaming clock and clears sessions - every view "
        "reconnects for about a minute");
    Auth::bind(reboot, Auth::Action::CameraSystem);
    connect(reboot, &QPushButton::clicked, this, &SystemPanel::confirm_reboot);
    row->addWidget(reboot);
    row->addStretch(1);
    col->addLayout(row);
    return w;
}

// ---------------------------------------------------------------- 데이터

void SystemPanel::fetch_date()
{
    if (m_pending_date)
        return;
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/system.cgi");
    url.setQuery("msubmenu=date&action=view");
    QNetworkRequest req = sunapi_request(url, 4000);
    req.setRawHeader("Accept", "application/json");

    m_pending_date = m_net->get(req);
    connect(m_pending_date, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_pending_date;
        m_pending_date = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_clock_body->setText(QString("fetch failed (%1)")
                                      .arg(reply->errorString()));
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        m_sync_type = obj.value("SyncType").toString();

        // 카메라 UTC vs PC UTC — 초 단위 오프셋 (스트리밍 RTCP 시계와는 별개)
        const QDateTime cam_utc = QDateTime::fromString(
            obj.value("UTCTime").toString(), "yyyy-MM-dd HH:mm:ss");
        QString offset = QStringLiteral("—");
        if (cam_utc.isValid()) {
            QDateTime cam = cam_utc;
            cam.setTimeZone(QTimeZone::UTC);
            const double diff =
                (cam.toMSecsSinceEpoch()
                 - QDateTime::currentMSecsSinceEpoch()) / 1000.0;
            offset = QString("%1%2s (vs this PC)")
                         .arg(diff >= 0 ? "+" : "").arg(diff, 0, 'f', 1);
        }

        const bool ntp = m_sync_type.compare(QLatin1String("NTP"),
                                             Qt::CaseInsensitive) == 0;
        m_clock_body->setText(
            QString("device %1 · UTC %2<br/>SyncType <span style=\"color:%3\">"
                    "%4</span> · offset %5")
                .arg(obj.value("LocalTime").toString(),
                     obj.value("UTCTime").toString(),
                     ntp ? Theme::green.name() : Theme::amber.name(),
                     m_sync_type.isEmpty() ? QStringLiteral("?") : m_sync_type,
                     offset));
        m_btn_ntp->setText(ntp ? QString("Resync NTP")
                               : QString("Switch to NTP"));
    });
}

void SystemPanel::fetch_profile_access()
{
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/system.cgi");
    url.setQuery("msubmenu=profileaccessinfo&action=view&ViewGroup=Profile");
    // 텍스트 응답을 원문 그대로 보여준다 (미실측 API — Accept: json을 붙이지 않는다)
    QNetworkRequest req = sunapi_request(url, 4000);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const int http = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            m_profile_body->setText(
                http == 608 || http == 404
                    ? QString("this device does not support profileaccessinfo")
                    : QString("fetch failed (%1)").arg(reply->errorString()));
            return;
        }
        // 응답은 전 채널×프로파일 덤프(수백 줄) — 세션이 붙은 것만 추려 보인다.
        // "Channel.N.Profile.M.Key=Value" 줄을 (채널,프로파일)로 묶는다.
        const QStringList lines = QString::fromUtf8(reply->readAll())
                                      .split('\n', Qt::SkipEmptyParts);
        QMap<QPair<int, int>, QHash<QString, QString>> rows;
        static const QRegularExpression re(
            "^Channel\\.(\\d+)\\.Profile\\.(\\d+)\\.(\\w+)=(.*)$");
        for (const QString &line : lines) {
            const auto m = re.match(line.trimmed());
            if (m.hasMatch())
                rows[{m.captured(1).toInt(), m.captured(2).toInt()}]
                    .insert(m.captured(3), m.captured(4));
        }
        if (rows.isEmpty()) {  // 예상 밖 형식 — 원문 앞부분이라도
            m_profile_body->setText(lines.mid(0, 20).join('\n'));
            return;
        }
        QStringList out;
        int total_sessions = 0;
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            const int users = it.value().value("ConcurrentUserCount").toInt();
            if (users <= 0)
                continue;
            total_sessions += users;
            out << QString("CH%1 · profile%2 — %3 sessions · %4/%5 kbps · %6fps")
                       .arg(it.key().first + 1).arg(it.key().second).arg(users)
                       .arg(it.value().value("CurrentBitrate", "?"),
                            it.value().value("TotalBitrate", "?"),
                            it.value().value("CurrentFPS", "?"));
        }
        out << QString("total %1 sessions · %2 profiles queried")
                   .arg(total_sessions).arg(rows.size());
        m_profile_body->setText(out.join('\n'));
    });
}

void SystemPanel::fetch_logs(const QString &submenu)
{
    m_log_view->setPlainText("loading...");
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/system.cgi");
    url.setQuery(QString("msubmenu=%1&action=view").arg(submenu));
    // 텍스트 응답 "[날짜] [종류] 내용" 줄을 그대로 — 로그는 길어서 8초
    QNetworkRequest req = sunapi_request(url, 8000);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, submenu] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            m_log_view->setPlainText(QString("%1 fetch failed (%2)")
                                         .arg(submenu, reply->errorString()));
            return;
        }
        QStringList lines = QString::fromUtf8(reply->readAll())
                                .split('\n', Qt::SkipEmptyParts);
        if (lines.size() > 41)  // Total= 헤더 + 40줄
            lines = lines.mid(0, 41);
        m_log_view->setPlainText(lines.join('\n'));
    });
}

void SystemPanel::save_backup()
{
    if (!Auth::can(Auth::Action::CameraSystem)) {
        qWarning() << "[SystemPanel] 권한 없는 설정 백업 차단";
        return;
    }
    m_btn_backup->setEnabled(false);
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/system.cgi");
    url.setQuery("msubmenu=configbackup&action=control");
    // 설정 파일이 느린 링크에서 수 초 걸릴 수 있다
    QNetworkRequest req = sunapi_request(url, 30000);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        // 받는 동안 꺼뒀던 버튼을 되돌린다 — 권한을 잃었으면 꺼둔 채로.
        m_btn_backup->setEnabled(Auth::can(Auth::Action::CameraSystem));
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "[SystemPanel] 백업 실패:" << reply->errorString();
            m_btn_backup->setText("Backup failed");
            QTimer::singleShot(3000, this, [this] {
                m_btn_backup->setText("Save config backup");
            });
            return;
        }
        const QByteArray data = reply->readAll();
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString path = QDir(dir).filePath(
            QString("guardx_camera_backup_%1.bin")
                .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            qWarning() << "[SystemPanel] 백업 저장 실패:" << path;
            m_btn_backup->setText("Save failed");
            return;
        }
        f.close();
        qInfo() << "[SystemPanel] 설정 백업 저장:" << path << data.size() << "bytes";
        m_btn_backup->setText(
            QString("Saved (%1 KB)").arg(data.size() / 1024));
        m_btn_backup->setToolTip(path);
        QTimer::singleShot(4000, this, [this] {
            m_btn_backup->setText("Save config backup");
        });
    });
}

void SystemPanel::confirm_reboot()
{
    if (!Auth::can(Auth::Action::CameraSystem)) {
        qWarning() << "[SystemPanel] 권한 없는 카메라 재부팅 차단";
        return;
    }
    // 1단: 파장 설명 (§4d — "약 1분간 전 화면이 재접속됩니다")
    QDialog dlg(this);
    dlg.setWindowTitle("Reboot camera");
    dlg.setStyleSheet(QString("QDialog { background:%1; border:2px solid %2; }")
                          .arg(Theme::bg0.name(), Theme::alarm.name()));
    auto *col = new QVBoxLayout(&dlg);
    col->setContentsMargins(24, 20, 24, 16);
    col->setSpacing(14);
    auto *body = new QLabel(
        QString("Reboot the camera?\n\n"
                          "· every channel view drops for about a minute, then reconnects\n"
                          "· the streaming clock is re-anchored (good for latency measurement)\n"
                          "· every app comes back according to its AutoStart setting"),
        &dlg);
    body->setFont(Theme::ui_font(11));
    Theme::restyle(body, [=] {
        return QString("color:%1;").arg(Theme::textHi.name());
    });
    col->addWidget(body);
    auto *btns = new QDialogButtonBox(&dlg);
    auto *cancel = btns->addButton(QString("Cancel"),
                                   QDialogButtonBox::RejectRole);
    btns->addButton("Continue...", QDialogButtonBox::AcceptRole);
    cancel->setDefault(true);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    col->addWidget(btns);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // 2단: 최종 확인 (이중 클릭 방지 — 기본 버튼도 취소)
    QDialog dlg2(this);
    dlg2.setWindowTitle("Final confirmation");
    dlg2.setStyleSheet(dlg.styleSheet());
    auto *col2 = new QVBoxLayout(&dlg2);
    col2->setContentsMargins(24, 20, 24, 16);
    col2->setSpacing(14);
    auto *body2 = new QLabel(
        QString("<b style=\"color:%1\">Rebooting now.</b> This cannot be "
                          "undone.").arg(Theme::alarm.name()), &dlg2);
    body2->setFont(Theme::ui_font(12));
    col2->addWidget(body2);
    auto *btns2 = new QDialogButtonBox(&dlg2);
    auto *cancel2 = btns2->addButton(QString("Cancel"),
                                     QDialogButtonBox::RejectRole);
    auto *go = btns2->addButton(QString("Reboot now"),
                                QDialogButtonBox::AcceptRole);
    cancel2->setDefault(true);
    Theme::restyle(go, [=] {
        return QString("QPushButton { border:1px solid %1; color:%1;"
                              " background:transparent; padding:5px 14px; }")
                          .arg(Theme::alarm.name());
    });
    QObject::connect(btns2, &QDialogButtonBox::accepted, &dlg2, &QDialog::accept);
    QObject::connect(btns2, &QDialogButtonBox::rejected, &dlg2, &QDialog::reject);
    col2->addWidget(btns2);
    if (dlg2.exec() != QDialog::Accepted)
        return;

    // 재부팅 모드 먼저 (폴 실패를 오프라인으로 오해하지 않게) — 그리고 발사
    CameraStatus::instance()->enter_reboot_mode();
    m_control->reboot_camera();
    qInfo() << "[SystemPanel] 재부팅 지시 — 복구 대기 모드 진입";
}
