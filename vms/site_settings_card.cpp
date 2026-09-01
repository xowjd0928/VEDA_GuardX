#include "site_settings_card.h"

#include "auth.h"
#include "calibration_store.h"
#include "credentials.h"
#include "panel_chrome.h"
#include "site_config.h"
#include "reauth_dialog.h"
#include "theme.h"

#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

// 새 UI 문구는 영어로 쓴다(12번 전면 영문화 합의 — 기존 한글은 그 작업이
// 일괄 정리한다).

SiteSettingsCard::SiteSettingsCard(QWidget *parent) : QWidget(parent)
{
    // 08-19: Site label 과 Floor calibration 은 **별개 카드 둘**이다 — 한
    // 카드에 구분선 하나로는 서로 다른 설정임이 안 읽힌다(사용자 피드백).
    // 이 위젯 자체는 투명 컨테이너로 남는다.
    build_ui();
    refresh_status();
}

void SiteSettingsCard::build_ui()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(20);   // 우측 열(side_col)의 카드 간격과 동일

    // ⚠ 카드 위젯에는 WA_StyledBackground 가 있어야 QSS #Panel 배경이
    //   칠해진다 (08-19 사용자 신고)
    auto *site_card = new QWidget(this);
    site_card->setObjectName("Panel");
    site_card->setAttribute(Qt::WA_StyledBackground);
    outer->addWidget(site_card);
    auto *site_shell = new QVBoxLayout(site_card);
    site_shell->setContentsMargins(0, 0, 0, 0);
    site_shell->setSpacing(0);

    // 머리글은 PanelChrome::header — 카드 머리글 폰트·높이가 앱 전체와
    // 한 벌이 되게 한다 (08-19 "폰트 통일" 피드백)
    site_shell->addWidget(PanelChrome::header(
        QStringLiteral("Site label"), QString(), site_card));

    auto *site_body = new QWidget(site_card);
    site_shell->addWidget(site_body);
    auto *col = new QVBoxLayout(site_body);
    col->setContentsMargins(20, 10, 20, 14);
    col->setSpacing(8);

    // 08-19: 좁은 우측 열(보드 배치)용 줄바꿈 — 힌트는 접히는 줄로,
    // 입력·[적용]은 제 줄로. 한 줄에 다 세우면 열 폭을 넘어 잘린다
    // ("crunched" 사고의 원인이 바로 이 카드였다).
    auto *site_hint = new QLabel(
        "shown on the login card, top bar and reports - all clients", this);
    site_hint->setFont(Theme::mono_font(10));
    site_hint->setWordWrap(true);
    Theme::restyle(site_hint, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    col->addWidget(site_hint);

    auto *site_edit_row = new QHBoxLayout();
    site_edit_row->setSpacing(10);

    m_site_edit = new QLineEdit(this);
    m_site_edit->setFont(Theme::ui_font(11));
    m_site_edit->setMaxLength(80);
    m_site_edit->setMinimumWidth(Theme::px(240));
    m_site_edit->setText(SiteConfig::instance()->site_name());
    site_edit_row->addWidget(m_site_edit, 1);

    m_site_apply = new QPushButton("Apply", this);
    m_site_apply->setFont(Theme::ui_font(11, 600));
    m_site_apply->setCursor(Qt::PointingHandCursor);
    // 서버 전역 저장 = 관리자 전용. 진짜 방어선은 서버의 토큰 재검증이고
    // 이 잠금은 표면이다(코드베이스 규약). ⚠ Action 은 SETTINGS 쓰기 게이트
    // (ZoneSettings)를 빌려 쓴다 — 전용 Action 추가는 auth.h 소유자(A) 몫.
    Auth::bind(m_site_apply, Auth::Action::ZoneSettings);
    connect(m_site_apply, &QPushButton::clicked, this,
            &SiteSettingsCard::save_site_name);
    site_edit_row->addWidget(m_site_apply);
    col->addLayout(site_edit_row);

    m_site_status = new QLabel(this);
    m_site_status->setFont(Theme::mono_font(10));
    col->addWidget(m_site_status);

    // 서버(또는 다른 관리자)가 바꾸면 입력칸도 따라간다 — 단 편집 중이면
    // 덮지 않는다(입력을 지우는 앱은 신뢰를 잃는다).
    connect(SiteConfig::instance(), &SiteConfig::site_name_changed, this, [this] {
        if (!m_site_edit->hasFocus())
            m_site_edit->setText(SiteConfig::instance()->site_name());
        set_status(m_site_status, "site label updated", false);
    });

    // ---- 캘리브레이션 — 별개 카드 (08-19) ----------------------------------
    auto *cal_card = new QWidget(this);
    cal_card->setObjectName("Panel");
    cal_card->setAttribute(Qt::WA_StyledBackground);
    outer->addWidget(cal_card);
    auto *cal_shell = new QVBoxLayout(cal_card);
    cal_shell->setContentsMargins(0, 0, 0, 0);
    cal_shell->setSpacing(0);

    // 카메라 웹 UI 링크 — 머리글 우측(trailing). QSS 가 리치텍스트 앵커
    // 색까지는 못 건드려 직접 굽는다
    auto *link = new QLabel(cal_card);
    link->setFont(Theme::mono_font(10));
    link->setTextFormat(Qt::RichText);
    link->setOpenExternalLinks(true);
    link->setTextInteractionFlags(Qt::TextBrowserInteraction);
    link->setCursor(Qt::PointingHandCursor);
    link->setToolTip("opens the calibration web UI in your browser -\n"
                     "draw lines, compute H, then [export calibration.json]");
    auto paint_link = [link] {
        const QUrl url = Credentials::calibration_ui_url();
        link->setText(QString("<a href=\"%1\" style=\"color:%2\">open web UI</a>")
                          .arg(url.toString(), Theme::accent.name()));
    };
    paint_link();
    Theme::on_theme_changed(link, paint_link);

    cal_shell->addWidget(PanelChrome::header(
        QStringLiteral("Floor calibration"), QString(), cal_card, link));

    auto *cal_body = new QWidget(cal_card);
    cal_shell->addWidget(cal_body);
    col = new QVBoxLayout(cal_body);
    col->setContentsMargins(20, 10, 20, 14);
    col->setSpacing(8);

    auto *hint = new QLabel(
        "H + obstacles for the Crowd floor map - made in the web UI", cal_body);
    hint->setFont(Theme::mono_font(10));
    hint->setWordWrap(true);
    Theme::restyle(hint, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    col->addWidget(hint);

    auto *calib_btn_row = new QHBoxLayout();
    calib_btn_row->setSpacing(10);

    m_load_btn = new QPushButton("Load file...", this);
    m_load_btn->setFont(Theme::ui_font(11, 600));
    m_load_btn->setCursor(Qt::PointingHandCursor);
    connect(m_load_btn, &QPushButton::clicked, this, &SiteSettingsCard::pick_file);
    calib_btn_row->addWidget(m_load_btn);

    m_apply_btn = new QPushButton("Apply", this);
    m_apply_btn->setFont(Theme::ui_font(11, 600));
    m_apply_btn->setCursor(Qt::PointingHandCursor);
    m_apply_btn->setEnabled(false);   // staged 파일이 생겨야 켜진다
    connect(m_apply_btn, &QPushButton::clicked, this,
            &SiteSettingsCard::apply_staged);
    calib_btn_row->addWidget(m_apply_btn);
    calib_btn_row->addStretch(1);
    col->addLayout(calib_btn_row);

    m_calib_status = new QLabel(this);
    m_calib_status->setFont(Theme::mono_font(10));
    col->addWidget(m_calib_status);

    m_force = new QCheckBox(
        "draw even if the coordinate frame mismatches "
        "(on by default - positions are approximate)", this);
    m_force->setFont(Theme::mono_font(10));
    m_force->setChecked(CalibrationStore::instance()->force_draw());
    connect(m_force, &QCheckBox::toggled, this, [](bool on) {
        CalibrationStore::instance()->set_force_draw(on);
    });
    col->addWidget(m_force);

    connect(CalibrationStore::instance(), &CalibrationStore::changed, this,
            [this] { refresh_status(); });
}

void SiteSettingsCard::pick_file()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Load calibration.json", QString(), "JSON (*.json)");
    if (path.isEmpty())
        return;

    // 여기서는 **적용하지 않는다** — 파싱 검증까지만. 실수한 파일이 평면도를
    // 바로 덮는 것을 [Apply] 한 번이 막는다(10번 요구).
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        set_status(m_calib_status,
                   QString("cannot open file: %1").arg(f.errorString()), true);
        return;
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        set_status(m_calib_status,
                   QString("not valid JSON: %1").arg(perr.errorString()), true);
        return;
    }

    m_staged = doc.object();
    m_staged_path = path;
    m_apply_btn->setEnabled(true);
    set_status(m_calib_status,
               QString("file loaded - press [Apply] to use it (%1)").arg(path),
               false);
}

void SiteSettingsCard::apply_staged()
{
    if (m_staged.isEmpty())
        return;
    // ⚠ 관리자면 이 한 번이 **모든 VMS 의 평면도**를 바꾼다 — 설정 쓰기 중에서
    //   파급이 가장 넓은 자리라 재확인을 여기서도 받는다.
    if (!ReauthDialog::ensure_fresh(this))
        return;

    // 실제 적용(전 검증 포함). 실패하면 이전 상태가 유지된다 — store 규약.
    if (!CalibrationStore::instance()->load_json(m_staged, m_staged_path)) {
        set_status(m_calib_status, CalibrationStore::instance()->error(), true);
        return;
    }

    Auth *auth = Auth::instance();
    if (auth->role() == Auth::Role::Admin && auth->verified()) {
        // 관리자 = 전역. 성공 확인은 retained 재방송(계약의 set_zone 패턴)이
        // 하지만, 그 사이 사용자가 "됐나?" 하고 기다리지 않게 응답으로도 말한다.
        set_status(m_calib_status, "applied here - saving for all clients...",
                   false);
        SiteConfig::instance()->publish_calibration(
            CalibrationStore::instance()->raw_json(),
            [this](bool ok, const QString &reason) {
                if (ok)
                    set_status(m_calib_status,
                               "applied and confirmed - all clients updated",
                               false);
                else
                    set_status(m_calib_status,
                               QString("applied here, but saving for all "
                                       "clients failed: %1").arg(reason),
                               true);
            });
    } else {
        // 운영자(또는 오프라인 유예 관리자) = 이 계정만. 서버 무접촉.
        SiteConfig::instance()->save_local_calibration(
            CalibrationStore::instance()->raw_json());
        set_status(m_calib_status,
                   "applied and confirmed - this account only", false);
    }

    m_apply_btn->setEnabled(false);   // 같은 파일 재적용 방지 — 다시 고르면 켜진다
}

void SiteSettingsCard::save_site_name()
{
    const QString name = m_site_edit->text().trimmed();
    if (name.isEmpty()) {
        set_status(m_site_status, "site label cannot be empty", true);
        return;
    }
    if (!ReauthDialog::ensure_fresh(this))
        return;
    set_status(m_site_status, "saving...", false);
    SiteConfig::instance()->save_site_name(
        name, [this](bool ok, const QString &reason) {
            if (ok)
                set_status(m_site_status, "saved - all clients updated", false);
            else
                set_status(m_site_status,
                           QString("save failed: %1").arg(reason), true);
        });
}

void SiteSettingsCard::refresh_status()
{
    auto *store = CalibrationStore::instance();
    if (!store->loaded()) {
        set_status(m_calib_status,
                   "no calibration loaded - floor map uses the sketch layout",
                   false);
        return;
    }

    int usable = 0, geo = 0;
    // 채널은 0-based(카메라 축)다 — 1..4 로 돌면 CH1 의 캘리브레이션을 못 세고
    // 없는 채널 4 를 세게 된다 (2026-08-22 축 통일 전의 잔재)
    for (int ch = 0; ch < 4; ++ch) {
        const auto c = store->channel(ch);
        if (c.usable) ++usable;
        if (c.geo_usable) ++geo;
    }
    QString origin = store->last_path();
    if (SiteConfig::instance()->has_local_override())
        origin += "  [this account only]";
    set_status(m_calib_status,
               QString("active: %1 - live-usable %2/4 - geometry %3/4 - "
                       "obstacles %4")
                   .arg(origin).arg(usable).arg(geo)
                   .arg(store->obstacles().size()),
               false);
}

void SiteSettingsCard::set_status(QLabel *label, const QString &text, bool error)
{
    label->setText(text);
    label->setStyleSheet(QString("color:%1;")
                             .arg((error ? Theme::alarm : Theme::textDim).name()));
}
