#include "profile_panel.h"
#include "auth.h"
#include "camera_tuner.h"
#include "credentials.h"
#include "panel_chrome.h"
#include "sunapi_request.h"
#include "theme.h"

#include <QAuthenticator>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/** @brief 레지스트리 "profile4" 표기 → 4 (실패 시 기본값) */
int profile_number(const QString &name, int fallback)
{
    QString digits;
    for (QChar c : name)
        if (c.isDigit())
            digits += c;
    bool ok = false;
    const int n = digits.toInt(&ok);
    return ok ? n : fallback;
}

// role = Theme 팔레트 슬롯 포인터 (값 복사 금지 — 테마 전환이 안 따라온다)
QPushButton *outline_btn(const QString &text, const QColor *role, QWidget *parent,
                         int w = 140)
{
    auto *b = new QPushButton(text, parent);
    b->setFixedSize(Theme::px(w), Theme::px(26));  // 글자와 같은 배율로 (안 그러면 잘린다)
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

ProfilePanel::ProfilePanel(QWidget *parent)
    : QWidget(parent)
{
    m_net = new QNetworkAccessManager(this);
    Credentials::install_tls_pinning(m_net);
    connect(m_net, &QNetworkAccessManager::authenticationRequired,
            this, [](QNetworkReply *reply, QAuthenticator *auth) {
                if (auth->user() == Credentials::camera_user()) {
                    qWarning() << "[ProfilePanel] 인증 거부 — 계정 확인 필요";
                    reply->abort();
                    return;
                }
                auth->setUser(Credentials::camera_user());
                auth->setPassword(Credentials::camera_password());
            });

    // 프리셋·키프레임은 기존 CameraTuner 재사용 — 튜닝 대상은 실사용 프로파일
    // (레지스트리 grid_profile — LiveViewer 기동 튜닝과 같은 대상).
    // fullscreen_profile은 08-19 전체화면(FOCUS) 삭제와 함께 폐기됐다.
    QSettings reg("GuardX", "VMS");
    const int p_grid =
        profile_number(reg.value("grid_profile", "profile4").toString(), 4);
    m_tuner = new CameraTuner({0, 1, 2, 3}, {p_grid}, this);

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 8, 0, 0);
    col->setSpacing(14);
    col->addWidget(build_table_card());
    col->addWidget(build_editor_card());
    col->addStretch(1);
}

void ProfilePanel::showEvent(QShowEvent *ev)
{
    QWidget::showEvent(ev);
    fetch_profiles();  // 진입할 때마다 새로 — 밖(웹 UI 등)에서 바뀔 수 있다
}

// ---------------------------------------------------------------- UI 구축

QWidget *ProfilePanel::build_table_card()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto *refresh = outline_btn(QString("Refresh"),
                                &Theme::textMuted, w, 90);
    connect(refresh, &QPushButton::clicked, this, &ProfilePanel::fetch_profiles);
    col->addWidget(PanelChrome::header(
        QStringLiteral("Encoder Profiles"),
        QStringLiteral("media.cgi videoprofile"), w, refresh));

    auto *host = new QWidget(w);
    m_table = new QGridLayout(host);
    m_table->setContentsMargins(2, 10, 2, 6);
    m_table->setHorizontalSpacing(22);
    m_table->setVerticalSpacing(5);
    col->addWidget(host);
    return w;
}

QWidget *ProfilePanel::build_editor_card()
{
    auto *w = new QWidget(this);
    auto *col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(8);

    m_result = new QLabel(w);
    m_result->setFont(Theme::mono_font(10));
    m_result->hide();
    col->addWidget(PanelChrome::header(
        QStringLiteral("Edit · Low-Latency Tools"),
        QStringLiteral("applying may briefly renegotiate that stream"), w,
        m_result));

    auto *row = new QHBoxLayout;
    row->setSpacing(10);

    auto mono_combo = [this](QComboBox *&out, int w_px) {
        out = new QComboBox(this);
        out->setFixedSize(Theme::px(w_px), Theme::px(24));
        out->setFont(Theme::mono_font(10));
        Theme::restyle(out, [=] {
            return QString("QComboBox { background:%1; border:1px solid %2;"
                    " border-radius:2px; color:%3; padding-left:8px; }"
                    "QComboBox::drop-down { border:none; width:16px; }"
                    "QComboBox QAbstractItemView { background:%1; color:%3;"
                    " border:1px solid %2; selection-background-color:%4; }")
                .arg(Theme::elevated.name(), Theme::border2.name(),
                     Theme::textMid.name(), Theme::elevated2.name());
        });
    };
    auto mono_spin = [this](QSpinBox *&out, int min, int max, int step,
                            const QString &suffix) {
        out = new QSpinBox(this);
        out->setRange(min, max);
        out->setSingleStep(step);
        out->setSuffix(suffix);
        out->setFixedSize(Theme::px(110), Theme::px(24));
        out->setFont(Theme::mono_font(10));
        out->setButtonSymbols(QAbstractSpinBox::PlusMinus);
        Theme::restyle(out, [=] {
            return QString("QSpinBox { background:%1; border:1px solid %2;"
                    " border-radius:2px; color:%3; padding-left:6px; }")
                .arg(Theme::elevated.name(), Theme::border2.name(),
                     Theme::textMid.name());
        });
    };

    auto add_labeled = [&](const QString &label, QWidget *field) {
        auto *box = new QVBoxLayout;
        box->setSpacing(3);
        auto *l = new QLabel(label, w);
        l->setFont(Theme::ui_font(9, 600, 0.10));
        Theme::restyle(l, [=] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        box->addWidget(l);
        box->addWidget(field);
        row->addLayout(box);
    };

    mono_combo(m_sel_ch, 70);
    for (int ch = 0; ch < 4; ++ch)
        m_sel_ch->addItem(QString("CH%1").arg(ch + 1), ch);
    add_labeled("Channel", m_sel_ch);

    mono_combo(m_sel_profile, 100);
    add_labeled("Profile", m_sel_profile);

    mono_spin(m_fps, 1, 60, 1, "fps");
    add_labeled("FPS", m_fps);
    mono_spin(m_bitrate, 128, 20480, 128, "kbps");
    add_labeled("Bitrate", m_bitrate);
    mono_combo(m_cbr, 70);
    m_cbr->addItems({"CBR", "VBR"});
    add_labeled("Control", m_cbr);
    mono_spin(m_gov, 1, 300, 1, "");
    add_labeled("GOV", m_gov);

    m_btn_apply = outline_btn(QString("Apply"), &Theme::amber, w, 80);
    connect(m_btn_apply, &QPushButton::clicked, this, &ProfilePanel::apply_edit);
    auto *apply_box = new QVBoxLayout;
    apply_box->setSpacing(3);
    apply_box->addSpacing(15);
    apply_box->addWidget(m_btn_apply);
    row->addLayout(apply_box);

    row->addStretch(1);

    auto *keyframe = outline_btn(QString("Force keyframe"),
                                 &Theme::accent, w, 110);
    keyframe->setToolTip(
        "Asks the selected stream for an immediate I-frame - shortens the black gap when switching");
    Auth::bind(keyframe, Auth::Action::CameraTuning);
    connect(keyframe, &QPushButton::clicked, this, [this] {
        if (!Auth::can(Auth::Action::CameraTuning))
            return;   // 백스톱
        m_tuner->request_sync_point(m_sel_ch->currentData().toInt(),
                                    m_sel_profile->currentData().toInt());
        show_result("keyframe request sent", true);
    });
    auto *kf_box = new QVBoxLayout;
    kf_box->setSpacing(3);
    kf_box->addSpacing(15);
    kf_box->addWidget(keyframe);
    row->addLayout(kf_box);

    auto *preset = outline_btn(QString("Reapply low-latency preset"),
                               &Theme::green, w, 150);
    preset->setToolTip(
        "Turns off DynamicGOV/SmartCodec/DynamicFPS/WiseStream - same as the "
        "startup tuning, idempotent");
    Auth::bind(preset, Auth::Action::CameraTuning);
    connect(preset, &QPushButton::clicked, this, [this] {
        if (!Auth::can(Auth::Action::CameraTuning))
            return;   // 백스톱
        m_tuner->start();
        show_result("low-latency preset applied - see the console log", true);
    });
    auto *ps_box = new QVBoxLayout;
    ps_box->setSpacing(3);
    ps_box->addSpacing(15);
    ps_box->addWidget(preset);
    row->addLayout(ps_box);

    col->addLayout(row);

    connect(m_sel_ch, &QComboBox::currentIndexChanged, this,
            [this](int) { load_selection(); });
    connect(m_sel_profile, &QComboBox::currentIndexChanged, this,
            [this](int) { load_selection(); });
    return w;
}

// ---------------------------------------------------------------- 데이터

void ProfilePanel::fetch_profiles()
{
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/media.cgi");
    url.setQuery("msubmenu=videoprofile&action=view");
    // 텍스트 응답 — camera_tuner와 같은 형식 (Accept: json을 붙이지 않는다)
    QNetworkRequest req = sunapi_request(url, 6000);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            show_result(QString("profile fetch failed (%1)")
                            .arg(reply->errorString()), false);
            return;
        }
        m_state.clear();
        const QStringList lines = QString::fromUtf8(reply->readAll()).split('\n');
        for (const QString &line : lines) {
            const int eq = line.indexOf('=');
            if (eq > 0)
                m_state.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
        }
        rebuild_table();
    });
}

void ProfilePanel::rebuild_table()
{
    while (QLayoutItem *it = m_table->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    m_rows.clear();

    // 존재하는 (채널, 프로파일) 수집 — 이름 키가 프로파일마다 하나는 있다
    for (int ch = 0; ch < 4; ++ch) {
        for (int p = 1; p <= 20; ++p) {
            const QString base = QString("Channel.%1.Profile.%2.").arg(ch).arg(p);
            if (m_state.contains(base + "EncodingType")
                || m_state.contains(base + "Name"))
                m_rows.append({ch, p});
        }
    }

    auto cell = [this](const QString &text, int r, int c, bool head = false,
                       const QColor &color = QColor()) {
        auto *l = new QLabel(text, this);
        l->setFont(head ? Theme::ui_font(9, 600, 0.10) : Theme::mono_font(10));
        Theme::restyle(l, [=] {
            return QString("color:%1;")
                             .arg(color.isValid()
                                      ? color.name()
                                      : (head ? Theme::textDim.name()
                                              : Theme::textMid.name()));
        });
        m_table->addWidget(l, r, c, Qt::AlignLeft | Qt::AlignVCenter);
    };

    static const char *HEADS[] = {"CH", "Profile", "Codec", "Resolution",
                                  "FPS", "Bitrate", "Ctrl", "GOV"};
    for (int c = 0; c < 8; ++c)
        cell(QLatin1String(HEADS[c]), 0, c, true);

    int r = 1;
    for (const auto &[ch, p] : m_rows) {
        const QString base = QString("Channel.%1.Profile.%2.").arg(ch).arg(p);
        QString codec = m_state.value(base + "EncodingType");
        if (codec.isEmpty())
            codec = m_state.contains(base + "H264.GOVLength") ? "H264"
                    : m_state.contains(base + "H265.GOVLength") ? "H265" : "?";
        const bool encodable = codec == "H264" || codec == "H265";
        cell(QString("CH%1").arg(ch + 1), r, 0);
        cell(QString("%1 %2").arg(p).arg(m_state.value(base + "Name")), r, 1);
        cell(codec, r, 2);
        cell(m_state.value(base + "Resolution", "—"), r, 3);
        cell(m_state.value(base + "FrameRate", "—"), r, 4);
        cell(m_state.value(base + "Bitrate", "—"), r, 5);
        // BitrateControlType은 GOVLength처럼 코덱 접두 키다 (08-06 실측)
        const QString ctrl =
            m_state.value(base + codec + ".BitrateControlType",
                          m_state.value(base + "BitrateControlType", "—"));
        cell(ctrl, r, 6, false, ctrl == "CBR" ? Theme::green : QColor());
        cell(encodable ? m_state.value(base + codec + ".GOVLength", "—")
                       : QStringLiteral("—"), r, 7);
        ++r;
    }
    m_table->setColumnStretch(8, 1);

    // 편집 콤보 갱신 (선택 유지 시도)
    const int keep = m_sel_profile->currentData().toInt();
    m_loading = true;
    m_sel_profile->clear();
    QSet<int> seen;
    for (const auto &[ch, p] : m_rows) {
        if (seen.contains(p))
            continue;
        seen.insert(p);
        m_sel_profile->addItem(
            QString("p%1 %2").arg(p).arg(m_state.value(
                QString("Channel.0.Profile.%1.Name").arg(p))), p);
    }
    const int idx = m_sel_profile->findData(keep);
    if (idx >= 0)
        m_sel_profile->setCurrentIndex(idx);
    m_loading = false;
    load_selection();
}

void ProfilePanel::load_selection()
{
    if (m_loading || m_sel_profile->count() == 0)
        return;
    const int ch = m_sel_ch->currentData().toInt();
    const int p = m_sel_profile->currentData().toInt();
    const QString base = QString("Channel.%1.Profile.%2.").arg(ch).arg(p);

    QString codec = m_state.value(base + "EncodingType");
    if (codec.isEmpty())
        codec = m_state.contains(base + "H264.GOVLength") ? "H264" : "H265";

    m_fps->setValue(m_state.value(base + "FrameRate", "30").toInt());
    m_bitrate->setValue(m_state.value(base + "Bitrate", "1024").toInt());
    m_gov->setValue(m_state.value(base + codec + ".GOVLength", "30").toInt());
    m_cbr->setCurrentText(
        m_state.value(base + codec + ".BitrateControlType",
                      m_state.value(base + "BitrateControlType", "CBR"))
                .toUpper() == "VBR"
            ? "VBR" : "CBR");
    const bool editable = codec == "H264" || codec == "H265";
    m_btn_apply->setEnabled(editable);  // MJPEG 등은 GOV가 없어 편집 제외
    m_btn_apply->setToolTip(
        editable ? QString()
                 : QString("%1 profiles are not editable here").arg(codec));
}

void ProfilePanel::apply_edit()
{
    if (!Auth::can(Auth::Action::CameraTuning)) {
        show_result(Auth::deny_reason(Auth::Action::CameraTuning), false);
        return;
    }
    const int ch = m_sel_ch->currentData().toInt();
    const int p = m_sel_profile->currentData().toInt();
    const QString base = QString("Channel.%1.Profile.%2.").arg(ch).arg(p);
    QString codec = m_state.value(base + "EncodingType");
    if (codec.isEmpty())
        codec = m_state.contains(base + "H264.GOVLength") ? "H264" : "H265";

    // 확인 — 라이브 스트림 재협상 경고 (기획서 §3-C)
    QDialog dlg(this);
    dlg.setWindowTitle("Apply profile");
    dlg.setStyleSheet(QString("QDialog { background:%1; border:2px solid %2; }")
                          .arg(Theme::bg0.name(), Theme::amber.name()));
    auto *col = new QVBoxLayout(&dlg);
    col->setContentsMargins(24, 20, 24, 16);
    col->setSpacing(14);
    auto *body = new QLabel(
        QString("Apply to CH%1 · profile%2 (%3):\n"
                          "%4fps · %5kbps · %6 · GOV %7\n\n"
                          "Streams watching this profile may briefly renegotiate.")
            .arg(ch + 1).arg(p).arg(codec).arg(m_fps->value())
            .arg(m_bitrate->value()).arg(m_cbr->currentText())
            .arg(m_gov->value()),
        &dlg);
    body->setFont(Theme::ui_font(11));
    Theme::restyle(body, [=] {
        return QString("color:%1;").arg(Theme::textHi.name());
    });
    col->addWidget(body);
    auto *btns = new QDialogButtonBox(&dlg);
    auto *cancel = btns->addButton(QString("Cancel"),
                                   QDialogButtonBox::RejectRole);
    btns->addButton("Apply", QDialogButtonBox::AcceptRole);
    cancel->setDefault(true);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    col->addWidget(btns);
    if (dlg.exec() != QDialog::Accepted)
        return;

    // videoprofile 수정은 set이 아니라 update (set은 601 NG — camera_tuner 실측)
    QUrl url = Credentials::camera_base_url();
    url.setPath("/stw-cgi/media.cgi");
    url.setQuery(QString("msubmenu=videoprofile&action=update"
                         "&Channel=%1&Profile=%2&FrameRate=%3&Bitrate=%4"
                         "&%6.BitrateControlType=%5&%6.GOVLength=%7")
                     .arg(ch).arg(p).arg(m_fps->value()).arg(m_bitrate->value())
                     .arg(m_cbr->currentText(), codec).arg(m_gov->value()));
    QNetworkRequest req = sunapi_request(url, 6000);

    m_btn_apply->setEnabled(false);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_btn_apply->setEnabled(true);
        const QString body = QString::fromUtf8(reply->readAll()).trimmed();
        // SUNAPI는 실패도 HTTP 200 + "NG\n..." 본문으로 온다
        if (reply->error() != QNetworkReply::NoError)
            show_result(QString("apply failed (%1)")
                            .arg(reply->errorString()), false);
        else if (!body.startsWith("OK"))
            show_result(QString("camera refused: %1")
                            .arg(body.left(80)), false);
        else
            show_result("applied - refreshing the table", true);
        fetch_profiles();  // 성공이든 거부든 진실은 다시 읽는다
    });
}

void ProfilePanel::show_result(const QString &text, bool ok)
{
    m_result->setText(text);
    Theme::restyle(m_result, [=] {
        return QString("color:%1;")
                                .arg((ok ? Theme::green : Theme::alarm).name());
    });
    m_result->show();
    QTimer::singleShot(4000, m_result, &QLabel::hide);
}
