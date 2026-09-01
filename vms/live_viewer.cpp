#include "live_viewer.h"
#include "wall_layout.h"
#include "zone_config.h"
#include "camera_tuner.h"
#include "channel_view.h"
#include "credentials.h"
#include "sunapi_request.h"
#include "edge_map_generator.h"
#include "panel_chrome.h"
#include "prediction_feed.h"
#include "site_config.h"
#include "theme.h"
#include "track_display_link.h"
#include "tracking_panel.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QFontMetricsF>
#include <QDebug>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QAuthenticator>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

// ---- 카메라 설정 -------------------------------------------------------------
// 자격정보는 소스에 두지 않는다 — Credentials가 %APPDATA%/GuardX/credentials.ini
// 또는 환경변수에서 읽는다 (공유폴더·git으로 새어나가지 않도록)
static const quint16 CAM_PORT = 554;

// 그리드 프로파일. QSettings로 덮어쓸 수 있다 — 1080p 성능 벤치마크나 운영
// 정책에 맞춰 바꾸기 위함. 전체화면(FOCUS) 모드가 사라지면서(08-19) 고해상도
// fullscreen_profile은 더 이상 쓰지 않는다 — 화면은 항상 2×2 그리드다.
static QString sub_profile()
{
    return QSettings("GuardX", "VMS").value("grid_profile", "profile4").toString();
}

static const int NUM_CHANNELS = 4; // 센서 0..3

/** @brief "profile4" -> 4 (SUNAPI videoprofile 번호) */
static int profile_number(const QString &name)
{
    int n = 0;
    for (const QChar c : name)
        if (c.isDigit())
            n = n * 10 + c.digitValue();
    return n;
}

/**
 * @brief 화면 채널 -> DB detections.channel 매핑
 *
 * 현재는 동일하지만, 수신부가 다른 번호로 저장하면 여기만 고치면 된다.
 */
static int db_channel_of(int ch)
{
    return ch;
}

/** @brief db_channel_of의 역 — DB detections.channel -> 화면 채널 */
static int screen_channel_of(int db_channel)
{
    return db_channel;
}

// 우측 TRACKING 패널 폭 — 08-19 워크스페이스 보드는 300px
// (Option-A-Workspace.dc.html). 400 은 그 이전 세대(Main.dc.html) 값이라
// 영상 벽을 100px 잡아먹고 있었다.
static const int TRACK_PANEL_W = 300;

// 표시 지연 (+/- 키) — 이제 **영상을 늦추는** 양이다 (08-12).
// 맞춰야 할 값이 ~100ms 대(메타 5Hz 격자의 평균 나이)이고 잔여 톱니가 ±100ms
// 라, 100ms 계단으로는 가운데를 못 찾는다. 20ms 로 낮춘다.
// 상한도 3초 → 1초: 이 목적에 그 이상은 의미가 없고, 그만큼이 그대로
// 영상 지연이라 실수로 크게 주면 저지연 구성이 통째로 무의미해진다.
static const int DELAY_STEP_MS = 20;
static const int DELAY_MIN_MS  = 0;
static const int DELAY_MAX_MS  = 1000;

/** @brief 현재 KST 시각 문자열 (타임라인 표기용 — 표시 계층에서 KST 변환) */
static QString kst_now()
{
    static QTimeZone kst = [] {
        QTimeZone tz("Asia/Seoul");
        return tz.isValid() ? tz : QTimeZone(9 * 3600);
    }();
    return QDateTime::currentDateTimeUtc().toTimeZone(kst).toString("HH:mm:ss");
}

// ---------------------------------------------------------------- 크롬 위젯들

/** @brief 토글 스위치 (28x16) — 디자인의 track+knob 스타일 (Face Blur에 사용) */
class ToggleSwitch : public QAbstractButton
{
public:
    explicit ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
    {
        setCheckable(true);
        setChecked(true);
        setFixedSize(28, 16);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Theme::border2, 1));
        p.setBrush(isChecked() ? QColor(0x1E, 0x46, 0x34) : Theme::elevated);
        p.drawRoundedRect(QRectF(0.5, 0.5, 27, 15), 8, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::textHi);
        p.drawEllipse(QRectF(isChecked() ? 15 : 3, 3, 10, 10));
    }
};

/** @brief 점유율 바 하나 — 트랙 + 채움 + 예측 틱 */
class OccBar : public QWidget
{
public:
    explicit OccBar(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void set_values(double pct, double pred_pct, const QColor &color)
    {
        m_pct = qBound(0.0, pct, 1.0);
        m_pred_pct = qBound(0.0, pred_pct, 0.98);
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::elevated);
        p.drawRoundedRect(rect(), 4, 4);
        if (m_pct > 0) {
            p.setBrush(m_color);
            p.drawRoundedRect(QRectF(0, 0, width() * m_pct, height()), 4, 4);
        }
        // 예측 틱 (+5분)
        QColor tick = Theme::textHi;
        tick.setAlphaF(0.7);
        p.setBrush(tick);
        p.drawRect(QRectF(width() * m_pred_pct - 1, -0, 2, height()));
    }

private:
    double m_pct = 0, m_pred_pct = 0;
    QColor m_color = Theme::accent;
};

/**
 * @brief 존 점유율 · +5분 예측 패널 — 채널별 바 + now → pred 텍스트
 *
 * 점유율은 DetectionFeed 감지 수 실데이터. 예측 틱은 **실모델**
 * (PredictionFeed — 카메라 /prediction의 hw_damped p50). 워밍업 채널·피드
 * 다운 시에만 유입 속도 외삽으로 폴백 (2026-07-29 배선 — 스펙 §3.3 완성).
 */
class OccupancyPanel : public QWidget
{
public:
    explicit OccupancyPanel(QWidget *parent) : QWidget(parent)
    {
        setObjectName("Panel");
        setAttribute(Qt::WA_StyledBackground);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(1, 1, 1, 1);
        lay->setSpacing(0);
        lay->addWidget(PanelChrome::header(
            QString::fromUtf8("Zone Occupancy · Forecast +5 min"),
            QString(), this));

        auto *body = new QWidget(this);
        auto *body_lay = new QVBoxLayout(body);
        body_lay->setContentsMargins(14, 8, 14, 8);
        body_lay->setSpacing(9);
        body_lay->addStretch(1);

        for (int ch = 0; ch < 4; ++ch) {
            auto *row = new QWidget(body);
            auto *row_lay = new QHBoxLayout(row);
            row_lay->setContentsMargins(0, 0, 0, 0);
            row_lay->setSpacing(10);

            auto *ch_lbl = new QLabel(QString("CH%1").arg(ch + 1), row);
            ch_lbl->setFont(Theme::mono_font(10));
            ch_lbl->setFixedWidth(34);
            Theme::restyle(ch_lbl, [=] {
                return QString("color:%1;").arg((Theme::textMuted).name());
            });
            row_lay->addWidget(ch_lbl);

            m_bars[ch] = new OccBar(row);
            row_lay->addWidget(m_bars[ch], 1);

            m_texts[ch] = new QLabel(QString::fromUtf8("0 → 0"), row);
            m_texts[ch]->setFont(Theme::mono_font(10.5));
            m_texts[ch]->setFixedWidth(78);
            m_texts[ch]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            Theme::restyle(m_texts[ch], [=] {
                return QString("color:%1;").arg((Theme::textMid).name());
            });
            row_lay->addWidget(m_texts[ch]);

            body_lay->addWidget(row);
        }
        body_lay->addStretch(1);
        lay->addWidget(body, 1);
    }

    void set_row(int ch, int occ, int cap, int pred)
    {
        if (ch < 0 || ch >= 4 || cap <= 0)
            return;
        const double pct = double(occ) / cap;
        m_bars[ch]->set_values(pct, double(pred) / cap,
                               Theme::occ_bar_color(pct));
        m_texts[ch]->setText(QString::fromUtf8("%1 → %2").arg(occ).arg(pred));
    }

private:
    OccBar *m_bars[4] = {};
    QLabel *m_texts[4] = {};
};

// ------------------------------------------------------------------ LiveViewer

LiveViewer::LiveViewer(QWidget *parent)
    : QWidget(parent)
{
    // 전체 골격 (08-19 워크스페이스): [좌 패널 | 월+눈금자 | 동선+점유율] 위에
    // 하단 이벤트 티커. 페이지 여백·제목 행은 없다 — 월이 가장자리까지 간다.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(build_stage(), 1);

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        auto *view = new ChannelView(ch, db_channel_of(ch), this);
        connect(view, &ChannelView::object_selected,
                this, &LiveViewer::on_object_selected);
        connect(view, &ChannelView::stream_event,
                this, &LiveViewer::on_stream_event);
        connect(view, &ChannelView::session_started,
                this, &LiveViewer::on_session_started);
        connect(view, &ChannelView::tile_drag_moved,
                this, &LiveViewer::on_tile_drag_moved);
        connect(view, &ChannelView::tile_drag_finished,
                this, &LiveViewer::on_tile_drag_finished);
        m_views.append(view);
    }

    // 카메라 인코더를 저지연 설정으로 강제 (기동 시 1회, 멱등)
    // GOV=1×fps, CBR, DynamicGOV/SmartCodec/DynamicFPS/WiseStream 끔
    QVector<int> tune_channels;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        tune_channels.append(ch);
    m_tuner = new CameraTuner(
        tune_channels, { profile_number(sub_profile()) }, this);
    m_tuner->start();

    // 배치는 고정 2×2 그리드다. 전체화면(FOCUS) 모드는 08-19 디자인 개편으로
    // 삭제했다 — 프로파일 전환도 함께 사라져 모든 채널이 늘 sub_profile로 돈다
    // (「그리드 4MP 금지」위반 여지도 같이 없어졌다).
    m_wall = new WallLayout(m_grid, m_views, this);
    m_wall->show_grid();

    // 배치가 바뀌면 **엣지맵도 다시 합성한다**. 엣지맵은 "지금 이 벽을 굳힌
    // 그림"이라(finish_edge_map), 스왑 뒤 그대로 두면 배경은 옛 배치인데 그
    // 위의 점·라벨만 새 배치를 따라가 서로 다른 채널을 가리킨다. 스냅샷은
    // 채널별로 들고 있으므로 재촬영 없이 다시 그리기만 하면 된다.
    // ⚠ 큐를 한 번 돌린 뒤에 한다 — 스왑 직후에는 타일 geometry 가 아직 옛
    //   자리라(레이아웃이 나중에 돈다) 같은 그림이 다시 나온다.
    connect(WallOrder::notifier(), &WallOrder::Notifier::changed, this, [this] {
        if (m_snap_frames.isEmpty() || !m_edge_btn || !m_edge_btn->isChecked())
            return;
        QTimer::singleShot(0, this, [this] { finish_edge_map(); });
    });
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        play_channel(ch, sub_profile());

    QSettings settings("GuardX", "VMS");
    // 기본 0 — 이 값은 이제 **영상 지연**이다 (08-12). 옛 기본값 500 은
    // "박스를 500ms 미룬다"(실제로는 무동작)는 뜻이었으므로 그대로 두면
    // 새 머신이 아무 조작 없이 500ms 영상 지연을 얻는다.
    m_delay_ms = settings.value("playback_delay_ms", 0).toInt();
    for (ChannelView *view : m_views)
        view->set_playback_delay(m_delay_ms);

    // ⚠ 좌측 패널(토글)은 m_views 가 만들어지기 **전에** 생성된다(build_stage 가
    //   생성자 첫 줄). 그래서 저장값을 여기서 한 번 더 밀어 넣어야 재시작 후
    //   "켜 둔 상태"가 실제 타일에 반영된다 — 토글 콜백만 믿으면 스위치는
    //   켜져 있는데 박스는 안 보이는 상태가 된다.
    const bool show_all = settings.value("show_all_boxes", false).toBool();
    for (ChannelView *view : m_views)
        view->set_show_all_boxes(show_all);

    setFocusPolicy(Qt::StrongFocus);

    // 점유율/유입 속도 1초 주기 갱신 (타일 배지 + 하단 패널)
    m_uptime.start();
    auto *stats = new QTimer(this);
    connect(stats, &QTimer::timeout, this, &LiveViewer::update_occupancy_stats);
    stats->start(1000);

    // 실모델 예측 배선 (카메라 /prediction 직결 — 박스와 동일 원칙).
    // warmup 채널은 스냅샷을 남기지 않아 아래 외삽 폴백이 대신한다.
    // 정원의 진실원천은 zone_thresholds(ZoneConfig)다 — Theme 값은 그게 아직
    // 안 왔을 때의 디자인 기본값일 뿐이고, ZoneConfig::capacity() 안에 이미
    // 그 폴백이 들어 있다. 타일 OCC 배지(channel_view.cpp:229)는 원래부터
    // ZoneConfig 를 쓰고 있어서, 여기만 Theme 를 쓰면 **같은 정원을 두 곳이
    // 다르게 말한다.**
    auto apply_capacities = [] {
        for (int ch = 0; ch < NUM_CHANNELS; ++ch)
            PredictionFeed::instance()->set_capacity(ch, ZoneConfig::capacity(ch));
    };
    apply_capacities();
    // retained 라 보통 접속 직후에 오지만, 생성자 시점엔 아직 없을 수 있고
    // 운영자가 정원을 바꾸면 재시작 없이 새로 온다 — 그때 다시 반영한다.
    connect(ZoneConfig::notifier(), &ZoneConfig::Notifier::changed, this,
            apply_capacities);
    connect(PredictionFeed::instance(), &PredictionFeed::prediction_arrived,
            this, [this](int ch, const PredictionFeed::Info &info) {
                if (ch < 0 || ch >= NUM_CHANNELS || info.warmup)
                    return;
                const double p50 = info.p50_at(5);
                if (p50 < 0)
                    return;
                m_pred[ch] = { p50, m_uptime.elapsed() };
            });

    note_event("Info", "SYS",
        QString("Qt VMS started · RTSP ×4 · %1 grid").arg(sub_profile()));
}

QWidget *LiveViewer::build_left_panel()
{
    // 좌측 컨텍스트 패널 (08-19 워크스페이스) — 도구 + 현장 트리.
    // 기존 헤더 행의 Edge Map·Face Blur 가 여기로 옮겨 왔다 (배선 동일).
    auto *panel = new QWidget(this);
    // 보드 좌측 열 264px — CROWD 좌측 열과 같은 값·같은 규칙(글자 배율)
    panel->setFixedWidth(Theme::px(230));
    panel->setAttribute(Qt::WA_StyledBackground);
    Theme::restyle(panel, [] {
        return QString("background:%1; border-right:1px solid %2;")
            .arg(Theme::chromeElevated.name(), Theme::border.name());
    });

    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(14, 12, 14, 10);
    lay->setSpacing(9);

    const auto section = [&](const char *text) {
        auto *lbl = new QLabel(QString::fromUtf8(text), panel);
        lbl->setFont(Theme::ui_font(9.5, 600, 0.14));
        Theme::restyle(lbl, [] {
            return QString("color:%1;").arg(Theme::textDim.name());
        });
        return lbl;
    };
    const auto divider = [&] {
        auto *line = new QWidget(panel);
        line->setAttribute(Qt::WA_StyledBackground);
        line->setFixedHeight(1);
        Theme::restyle(line, [] {
            return QString("background:%1;").arg(Theme::rowDivider.name());
        });
        return line;
    };

    lay->addWidget(section("Tools"));

    auto *edge_btn = new QPushButton("Edge Map", panel);
    edge_btn->setObjectName("EdgeMapButton");
    edge_btn->setFixedHeight(24);
    edge_btn->setFont(Theme::ui_font(10.5, 600, 0.10));
    edge_btn->setCursor(Qt::PointingHandCursor);
    // 토글이다 — 예전에는 한 번 깔면 **끄는 방법이 없었다**(앱을 다시 켜야 했다).
    edge_btn->setCheckable(true);
    edge_btn->setToolTip("Freeze the current view as an edge map behind the "
                         "floor map. Press again to remove it.");
    // restyle 로 묶는다 — 예전엔 생성 시점 색이 구워져 테마 전환을 안 따라왔다.
    // ⚠ :checked 를 반드시 함께 정의한다. QSS 를 건 버튼은 눌린 상태가
    //   저절로 표시되지 않아, 켜 둔 토글이 꺼진 것과 똑같아 보인다
    //   (#SegBtn 이 같은 이유로 :disabled 를 빠뜨렸었다).
    Theme::restyle(edge_btn, [] {
        return QString(
            "QPushButton#EdgeMapButton {"
            "  color:%1; background:%2; border:1px solid %3;"
            "  border-radius:4px; padding:0 12px;"
            "}"
            "QPushButton#EdgeMapButton:hover { background:%4; border-color:%5; }"
            "QPushButton#EdgeMapButton:pressed { background:%5; color:%6; }"
            "QPushButton#EdgeMapButton:checked {"
            "  background:%5; border-color:%5; color:%6;"
            "}")
            .arg(Theme::textHi.name(),
                 Theme::elevated2.name(),
                 Theme::border2.name(),
                 Theme::panel.name(),
                 Theme::accent.name(),
                 Theme::bg0.name());
    });
    m_edge_btn = edge_btn;
    connect(edge_btn, &QPushButton::toggled, this, [this](bool on) {
        if (!on) {
            if (m_track_panel)
                m_track_panel->set_edge_map(QImage());   // 빈 이미지 = 지운다
            return;
        }
        // 카메라 응답을 기다리는 동안 버튼은 켜진 채로 둔다 — 다 실패하면
        // finish_edge_map() 이 되돌린다(그때 이유도 타임라인에 남는다).
        capture_edge_map();
    });
    lay->addWidget(edge_btn);

    // Face Blur 토글 — Face/Head(v15 category 2·3) 영역 모자이크.
    // AI Overlay 토글(모든 사람에 박스)은 08-19에 삭제 — 박스는 이제 우클릭으로
    // 추적을 시작한 사람에게만, 그 사람의 구분색으로 붙는다.
    auto *blur_row = new QWidget(panel);
    auto *blur_lay = new QHBoxLayout(blur_row);
    blur_lay->setContentsMargins(0, 0, 0, 0);
    blur_lay->setSpacing(7);

    auto *blur_toggle = new ToggleSwitch(blur_row);
    blur_toggle->setToolTip(
        "Blur faces on every video tile for privacy. Switch off to see "
        "faces unmasked.");
    connect(blur_toggle, &QAbstractButton::toggled, this, [this](bool on) {
        for (ChannelView *view : m_views)
            view->set_face_blur(on);
    });
    blur_lay->addWidget(blur_toggle);

    auto *blur_lbl = new QLabel("Face Blur", blur_row);
    blur_lbl->setFont(Theme::ui_font(10.5, 400, 0.06));
    Theme::restyle(blur_lbl, [] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    blur_lay->addWidget(blur_lbl);
    blur_lay->addStretch(1);
    lay->addWidget(blur_row);

    // ---- All Boxes 토글 (08-24 사용자 요청) ----------------------------------
    // 08-19 에 "박스는 우클릭한 사람만"으로 바꿨는데, 전부 보고 싶을 때가
    // 있다는 요구. 기본은 08-19 규칙 그대로 꺼짐이고, 켜 두면 **재시작해도
    // 유지**된다("항시 켜두기"가 요구의 핵심이라 세션 한정이면 의미가 없다).
    auto *all_row = new QWidget(panel);
    auto *all_lay = new QHBoxLayout(all_row);
    all_lay->setContentsMargins(0, 0, 0, 0);
    all_lay->setSpacing(7);

    const bool show_all =
        QSettings("GuardX", "VMS").value("show_all_boxes", false).toBool();

    auto *all_toggle = new ToggleSwitch(all_row);
    all_toggle->setChecked(show_all);
    all_toggle->setToolTip(
        "Draw a box on every detected person, not just the ones you "
        "track.\n"
        "Tracked people keep their thicker coloured box and P-tag.");
    connect(all_toggle, &QAbstractButton::toggled, this, [this](bool on) {
        for (ChannelView *view : m_views)
            view->set_show_all_boxes(on);
        QSettings("GuardX", "VMS").setValue("show_all_boxes", on);
    });
    all_lay->addWidget(all_toggle);

    auto *all_lbl = new QLabel("All Boxes", all_row);
    all_lbl->setFont(Theme::ui_font(10.5, 400, 0.06));
    Theme::restyle(all_lbl, [] {
        return QString("color:%1;").arg(Theme::textMuted.name());
    });
    all_lay->addWidget(all_lbl);
    all_lay->addStretch(1);
    lay->addWidget(all_row);

    lay->addWidget(divider());

    // ---- 현장 트리 — 카메라 채널 (이름은 DB zones, 점은 스트림 생존) ----
    m_site_lbl = new QLabel(panel);
    m_site_lbl->setFont(Theme::ui_font(11, 600));
    Theme::restyle(m_site_lbl, [] {
        return QString("color:%1;").arg(Theme::textMid.name());
    });
    const auto paint_site = [this] {
        m_site_lbl->setText(QString::fromUtf8("GuardX Site · %1")
                                .arg(SiteConfig::instance()->site_name()));
    };
    paint_site();
    connect(SiteConfig::instance(), &SiteConfig::site_name_changed,
            m_site_lbl, paint_site);
    lay->addWidget(m_site_lbl);

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        auto *row = new QWidget(panel);
        auto *row_lay = new QHBoxLayout(row);
        row_lay->setContentsMargins(10, 0, 0, 0);
        row_lay->setSpacing(7);

        m_tree_dot[ch] = new QLabel(row);
        m_tree_dot[ch]->setFixedSize(7, 7);
        row_lay->addWidget(m_tree_dot[ch]);

        m_tree_name[ch] = new QLabel(row);
        m_tree_name[ch]->setFont(Theme::mono_font(10.5));
        Theme::restyle(m_tree_name[ch], [] {
            return QString("color:%1;").arg(Theme::accent.name());
        });
        row_lay->addWidget(m_tree_name[ch], 1);
        lay->addWidget(row);
    }
    // 이름은 DB(zones)에서 온다 — 바뀌면 트리도 따라간다 (타일과 같은 원천)
    connect(ZoneConfig::notifier(), &ZoneConfig::Notifier::changed, this,
            [this] { refresh_tree(); });
    refresh_tree();

    lay->addStretch(1);

    auto *brand = new QLabel(QString::fromUtf8("Big Eyes · GuardX"), panel);
    brand->setFont(Theme::mono_font(9, 400, 0.10));
    Theme::restyle(brand, [] {
        return QString("color:%1;").arg(Theme::textFaint.name());
    });
    lay->addWidget(brand);

    return panel;
}

void LiveViewer::refresh_tree()
{
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        if (!m_tree_dot[ch] || !m_tree_name[ch])
            continue;
        m_tree_name[ch]->setText(Theme::channel_name(ch));
        // 점 = 스트림 생존 (m_last_status 빈 문자열 = 재생 중).
        // "거짓 초록 금지": 아직 상태를 모르는 기동 직후는 dim 이다.
        const bool known = m_last_status.contains(ch);
        const bool up = known && m_last_status.value(ch).isEmpty();
        const QColor c = !known ? Theme::textFaint
                       : up     ? Theme::green
                                : Theme::alarm;
        m_tree_dot[ch]->setStyleSheet(
            QString("background:%1; border-radius:3px;").arg(c.name()));
    }
}

QWidget *LiveViewer::build_stage()
{
    auto *stage = new QWidget(this);
    auto *lay = new QHBoxLayout(stage);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    lay->addWidget(build_left_panel());

    // 중앙: 월(2px 간격, 가장자리까지) + 24h 눈금자
    auto *center = new QWidget(stage);
    auto *center_lay = new QVBoxLayout(center);
    center_lay->setContentsMargins(0, 0, 0, 0);
    center_lay->setSpacing(0);

    m_grid_host = new QWidget(center);
    m_grid = new QGridLayout(m_grid_host);
    m_grid->setSpacing(2);
    m_grid->setContentsMargins(2, 2, 2, 2);
    m_grid->setRowStretch(0, 1);
    m_grid->setRowStretch(1, 1);
    m_grid->setColumnStretch(0, 1);
    m_grid->setColumnStretch(1, 1);
    center_lay->addWidget(m_grid_host, 1);

    lay->addWidget(center, 1);

    // 우측 열: 동선 패널(가변) + 존 점유율(고정) — 하단 스트립에서 옮겨 왔다
    auto *right = new QWidget(stage);
    right->setFixedWidth(TRACK_PANEL_W);
    auto *right_lay = new QVBoxLayout(right);
    right_lay->setContentsMargins(0, 0, 0, 0);
    right_lay->setSpacing(0);

    m_track_panel = new TrackingPanel(right);
    connect(m_track_panel, &TrackingPanel::selection_changed,
            this, &LiveViewer::apply_selection);

    // 현장 LED 매트릭스 송출. 패널은 네트워크를 모르고 링크는 화면을 모르므로
    // 둘을 여기서 잇는다 — 화면 채널 -> DB 채널 변환이 이 파일에만 있는 것도
    // 같은 이유다(db_channel_of).
    connect(m_track_panel, &TrackingPanel::matrix_output_changed, this,
            [](const TrackId &target, bool on) {
        if (!on) {
            TrackDisplayLink::instance()->stop();
            return;
        }

        // global_id가 붙은 대상은 카메라를 넘나들 수 있어 지목 당시의 채널이
        // 곧 낡는다. RPi B가 global_id로 찾으므로 채널은 참고값이고, 이력의
        // 최신 채널을 실어 보내 로그를 읽을 수 있게 한다.
        TrackHistory *hist = TrackHistory::instance();
        const int screen_ch = target.is_global() ? hist->current_channel(target)
                                                 : target.channel;
        const int object_id = target.is_global() ? hist->current_object_id(target)
                                                 : target.object_id;

        TrackDisplayLink::instance()->start(
            target.global_id,
            screen_ch >= 0 ? db_channel_of(screen_ch) : -1,
            object_id,
            TrackHistory::label(target));
    });

    right_lay->addWidget(m_track_panel, 1);

    m_occ_panel = new OccupancyPanel(right);
    m_occ_panel->setFixedHeight(172);
    right_lay->addWidget(m_occ_panel);

    lay->addWidget(right);

    return stage;
}

QUrl LiveViewer::stream_url(int ch, const QString &profile) const
{
    QUrl u;
    u.setScheme("rtsp");
    u.setHost(Credentials::camera_host());
    u.setPort(CAM_PORT);
    u.setUserName(Credentials::camera_user());
    u.setPassword(Credentials::camera_password());
    u.setPath(QString("/%1/%2/media.smp").arg(ch).arg(profile));
    return u;
}

void LiveViewer::play_channel(int ch, const QString &profile)
{
    m_views[ch]->play_stream(stream_url(ch, profile));
    // 스트림은 프로파일 기반 H.264 (스펙 §6 — 디자인의 H.265 표기는 지향점).
    // 캡션은 정보 표기라 Title Case 규칙을 따른다 ("Profile4 · H.264").
    QString caption = profile;
    if (!caption.isEmpty())
        caption[0] = caption[0].toUpper();
    m_views[ch]->set_stream_caption(
        QString::fromUtf8("%1 · H.264").arg(caption));
}

void LiveViewer::on_stream_event(int ch, const QString &status)
{
    const QString last = m_last_status.value(ch);
    if (status == last)
        return;
    m_last_status[ch] = status;

    const QString src = QString("CH%1").arg(ch + 1);
    if (status.isEmpty()) {
        note_event("Info", src, "stream connected · RTSP up");
    } else {
        note_event("Warn", src, status);
    }
    refresh_tree();   // 트리의 스트림 생존 점도 같은 사건으로 갱신된다

    int up = 0;
    for (int c = 0; c < NUM_CHANNELS; ++c)
        if (m_last_status.value(c).isEmpty())
            ++up;
    emit stream_health_changed(up, NUM_CHANNELS);
}

void LiveViewer::on_session_started(int ch)
{
    if (!m_tuner || ch < 0 || ch >= NUM_CHANNELS)
        return;

    // 채널당 최소 간격. 재접속이 백오프로 반복될 때 매번 쏘면 카메라 제어
    // 채널을 두들기게 되고, 그 압박이 다시 세션 굶주림을 부른다 —
    // 유령 세션 30~60s(08-04) · 메타 churn이 영상을 흔든 사고(08-05).
    // 백오프 최소값이 1초라 2초면 연속 재시도에서 한 번 걸러진다.
    static const qint64 MIN_INTERVAL_MS = 2000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_keyframe_ms.value(ch, 0) < MIN_INTERVAL_MS)
        return;
    m_keyframe_ms[ch] = now;

    // 배치는 고정 2×2 그리드라 프로파일은 늘 sub_profile 하나다
    const int profile = profile_number(sub_profile());
    m_tuner->request_sync_point(ch, profile);
    // 저빈도(채널당 2초 하한)라 로그로 남긴다 — 이 훅이 실제로 도는지는
    // 재접속 상황에서만 드러나는데, 그때 로그가 없으면 확인할 방법이 없다.
    qInfo().noquote() << QString("[LiveViewer] ch %1 세션 시작 — 키프레임 강제 "
                                 "(profile %2)").arg(ch).arg(profile);
}

void LiveViewer::capture_edge_map()
{
    if (!m_track_panel)
        return;
    if (m_snap_pending > 0)
        return;   // 이미 받는 중 — 겹쳐 쏘지 않는다

    snapshot_net();   // 지연 생성 (이벤트 상세 팝업과 공유)

    m_snap_frames.clear();
    m_snap_pending = 0;

    for (int ch = 0; ch < m_views.size(); ++ch) {
        QUrl url = Credentials::camera_base_url();
        url.setPath("/stw-cgi/video.cgi");
        url.setQuery(QString("msubmenu=snapshot&action=view&Channel=%1")
                         .arg(db_channel_of(ch)));

        // 정지영상은 인코더 설정 조회보다 오래 걸린다(키프레임 대기 + JPEG 인코딩).
        QNetworkReply *reply = m_snap_net->get(sunapi_request(url, 8000));
        ++m_snap_pending;
        connect(reply, &QNetworkReply::finished, this, [this, ch, reply] {
            on_snapshot(ch, reply);
            reply->deleteLater();
        });
    }

    if (m_snap_pending == 0) {
        finish_edge_map();
        return;
    }
    note_event("Info", "SYS",
                              "edge map: asking the camera for snapshots...");
}

void LiveViewer::on_snapshot(int channel, QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError) {
        QImage img;
        // 카메라는 JPEG 을 준다. 포맷을 못 읽으면 그 채널만 비운다 —
        // 한 채널 실패로 나머지 세 장을 버리지 않는다.
        if (img.loadFromData(reply->readAll()) && !img.isNull())
            m_snap_frames.insert(channel, img);
        else
            qWarning() << "[EdgeMap] ch" << channel << "스냅샷 디코드 실패";
    } else {
        qWarning() << "[EdgeMap] ch" << channel << "스냅샷 실패:"
                   << reply->errorString();
    }

    if (--m_snap_pending <= 0) {
        m_snap_pending = 0;
        finish_edge_map();
    }
}

void LiveViewer::finish_edge_map()
{
    if (!m_track_panel || !m_grid_host)
        return;

    if (m_snap_frames.isEmpty()) {
            note_event("Warn", "SYS",
                                  "edge map: the camera did not return a "
                                  "snapshot - nothing to draw");
        if (m_edge_btn) {
            const QSignalBlocker block(m_edge_btn);
            m_edge_btn->setChecked(false);
        }
        return;
    }

    // 화면에서 보던 배치 그대로 합성한다 — 엣지맵은 "지금 이 그리드를 굳힌
    // 그림"이라, 타일 위치가 다르면 평면도와 겹쳐 볼 때 방향 감각이 어긋난다.
    QImage composed(m_grid_host->size(), QImage::Format_RGB32);
    if (composed.isNull())
        return;
    composed.fill(Qt::black);
    {
        QPainter p(&composed);
        // ⚠ **부드럽게 줄여야 한다** (2026-08-24). 기본 변환은 최근접이라
        //   2592x1520 스냅샷을 ~515x250 타일로 5배 줄이면 **에일리어싱**이
        //   생긴다 — 부드러운 반사·무늬가 계단처럼 끊겨 들어와, 엣지 추출의
        //   선명도 판정(EdgeMapGenerator §why)이 그걸 전부 "날카로운 경계"로
        //   읽는다. 실제로 이 한 줄이 빠져서 무늬가 그대로 살아 있었다.
        for (int ch = 0; ch < m_views.size(); ++ch) {
            const QImage img = m_snap_frames.value(ch);
            if (img.isNull() || !m_views[ch])
                continue;
            const QPoint origin = m_views[ch]->mapTo(m_grid_host, QPoint(0, 0));
            // ⚠ **QImage::scaled(SmoothTransformation) 로 미리 줄인다.**
            //   painter 의 SmoothPixmapTransform 은 이중선형이라 2592x1520 →
            //   ~515x250 (5배 축소)에서 표본이 모자라 **에일리어싱**이 남는다.
            //   부드러운 반사·돌무늬가 계단으로 끊겨 들어오면 엣지 추출의
            //   선명도 판정(EdgeMapGenerator §why)이 그걸 전부 "날카로운
            //   경계"로 읽어, 걸러내려던 무늬가 그대로 살아난다 (08-24 실측).
            //   QImage::scaled 는 축소 시 면적 평균이라 그 문제가 없다.
            p.drawImage(origin, img.scaled(m_views[ch]->size(),
                                           Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation));
        }
    }

    // 엣지 색은 팔레트에서 가져온다 — 화면 위 다른 선(동선·박스)이 전부
    // 의미색이라, 배경이 되는 이 선만 중립이어야 그 위가 읽힌다.
    QColor edge_ink = Theme::textMuted;
    edge_ink.setAlpha(170);
    const QImage edge_map =
        EdgeMapGenerator::generate_edge_overlay(composed, 30, edge_ink);
    if (edge_map.isNull()) {
            note_event("Warn", "SYS", "edge map generation failed");
        if (m_edge_btn) {
            const QSignalBlocker block(m_edge_btn);
            m_edge_btn->setChecked(false);
        }
        return;
    }

    m_track_panel->set_edge_map(edge_map);
    note_event(
            "Info", "SYS",
            QString("edge map captured from %1 camera snapshot(s)")
                .arg(m_snap_frames.size()));
}

void LiveViewer::update_occupancy_stats()
{
    const qint64 now = m_uptime.elapsed();

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        const int occ = m_views[ch]->occupancy();
        // 타일 배지와 같은 원천을 쓴다 — 하단 패널만 정적 기본값을 쓰면
        // 같은 화면의 두 숫자가 어긋나고, 예측도 틀린 정원으로 계산된다.
        const int cap = ZoneConfig::capacity(ch);

        // 최근 60초 샘플로 유입 속도(명/분) 추정
        QVector<OccSample> &hist = m_occ_hist[ch];
        hist.append({now, occ});
        while (!hist.isEmpty() && now - hist.first().ms > 60000)
            hist.removeFirst();

        double flow = 0;
        const qint64 span = now - hist.first().ms;
        if (span >= 5000)
            flow = double(occ - hist.first().occ) / (span / 60000.0);
        m_views[ch]->set_flow_per_min(flow);

        // +5분 예측 — 실모델(hw_damped, 카메라 /prediction) 우선. 신선도 3분
        // (60초 폴링 2회 유예). 스냅샷이 없거나 낡으면(워밍업 채널·피드 다운)
        // 종전의 유입 속도 외삽으로 폴백 — 빈 값보다 근사가 낫다.
        int pred;
        const PredSnap &ps = m_pred[ch];
        if (ps.p50 >= 0 && ps.at_ms >= 0 && now - ps.at_ms < 3 * 60 * 1000)
            pred = qBound(0, qRound(ps.p50), cap);
        else
            pred = qBound(0, qRound(occ + 5 * flow), cap);
        m_occ_panel->set_row(ch, occ, cap, pred);
    }
}

void LiveViewer::on_object_selected(int db_channel, int object_id,
                                    const QRectF &cam_rect)
{
    Q_UNUSED(cam_rect);

    // v6 이전에는 여기서 detections를 조회해 "화면 박스 좌표 vs DB 최신 좌표"를
    // qDebug로 대조했다. 좌표 투영을 맞추던 시절의 검증 코드이고 화면 데이터의
    // 원천이 아니라 함께 제거했다 — VMS의 마지막 DB 직결이었다.
    //
    // 부수 효과로 버그가 하나 고쳐졌다: 구 코드는 DB에 해당 object_id가 없으면
    // 조기 return 해서 아래 하이라이트까지 건너뛰었다. 이제는 항상 강조된다.

    // 선택 상태는 패널이 들고 있다 (다중 선택·색 배정이 거기 규칙이다).
    // 패널이 selection_changed로 되돌려주면 apply_selection이 타일에 반영한다.
    m_track_panel->select(screen_channel_of(db_channel), object_id);
}

void LiveViewer::apply_selection(const QVector<TrackId> &targets)
{
    TrackHistory *hist = TrackHistory::instance();

    // 색은 패널에 물어본다 — 슬롯이 대상에 고정돼 있어 목록 인덱스로는
    // 계산할 수 없고, 두 곳에서 따로 계산하면 언젠가 어긋난다.
    QHash<int, QColor> by_channel[NUM_CHANNELS];
    for (const TrackId &id : targets) {
        const int ch = hist->current_channel(id);
        const int object_id = hist->current_object_id(id);
        if (ch < 0 || ch >= NUM_CHANNELS || object_id < 0)
            continue;
        by_channel[ch].insert(object_id, m_track_panel->color_of(id));
    }

    for (ChannelView *view : m_views)
        view->set_selected_objects(by_channel[view->channel()]);
}

/**
 * @brief 드래그 고스트 카드 — 미니 타일 (LIVE 점 + 채널명)
 *
 * 영상은 못 담는다: direct 경로는 네이티브 HWND라 grab()이 검은 화면을
 * 돌려준다 (08-13 실측 — capture_edge_map 주석). 대신 타일 좌상단 크롬과
 * 같은 문법의 카드를 그려 "어느 채널을 들고 있는지"만 보여 준다.
 */
static QPixmap make_drag_ghost(int ch, qreal dpr)
{
    const QSize sz(176, 99);   // 16:9 미니 타일
    QPixmap pm(sz * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Theme::OnVideo::accent, 1.5));
    p.setBrush(QColor(10, 13, 18, 235));
    p.drawRoundedRect(QRectF(1, 1, sz.width() - 2, sz.height() - 2), 4, 4);

    p.setPen(Qt::NoPen);
    p.setBrush(Theme::OnVideo::accent);
    p.drawEllipse(QRectF(12, 14, 7, 7));
    p.setBrush(Qt::NoBrush);
    p.setPen(Qt::white);
    p.setFont(Theme::mono_font(11, 600, 0.08));
    p.drawText(QPointF(27, 23), Theme::channel_name(ch));
    return pm;
}

int LiveViewer::tile_at(const QPoint &global_pos) const
{
    for (ChannelView *view : m_views)
        if (view->isVisible()
            && view->rect().contains(view->mapFromGlobal(global_pos)))
            return view->channel();
    return -1;
}

void LiveViewer::on_tile_drag_moved(int ch, const QPoint &global_pos)
{
    if (!m_drag_ghost) {
        // 고스트는 **톱레벨** 창이다 — 같은 창 안의 형제 위젯은 direct 경로의
        // 네이티브 영상 창 아래에 깔린다(airspace). ToolTip 플래그면 최상위에
        // 뜨고 포커스를 뺏지 않으며, 마우스 투과라 타일 판정도 방해하지 않는다.
        m_drag_ghost = new QLabel(nullptr,
                                  Qt::ToolTip | Qt::FramelessWindowHint);
        m_drag_ghost->setAttribute(Qt::WA_TranslucentBackground);
        m_drag_ghost->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_drag_ghost->setAttribute(Qt::WA_ShowWithoutActivating);
        m_drag_ghost->setAttribute(Qt::WA_DeleteOnClose);
        m_drag_ghost->setPixmap(make_drag_ghost(ch, devicePixelRatioF()));
        m_drag_ghost->setFixedSize(176, 99);
        QGuiApplication::setOverrideCursor(Qt::ClosedHandCursor);
    }
    m_drag_ghost->move(global_pos + QPoint(14, 10));
    m_drag_ghost->show();

    const int under = tile_at(global_pos);
    const int hint = (under >= 0 && under != ch) ? under : -1;
    if (hint != m_drop_ch) {
        if (m_drop_ch >= 0)
            m_views[m_drop_ch]->set_drop_hint(false);
        m_drop_ch = hint;
        if (m_drop_ch >= 0)
            m_views[m_drop_ch]->set_drop_hint(true);
    }
}

void LiveViewer::on_tile_drag_finished(int ch, const QPoint &global_pos)
{
    if (m_drag_ghost) {
        m_drag_ghost->close();   // WA_DeleteOnClose
        m_drag_ghost = nullptr;
        QGuiApplication::restoreOverrideCursor();
    }
    if (m_drop_ch >= 0)
        m_views[m_drop_ch]->set_drop_hint(false);
    m_drop_ch = -1;

    // 타일 밖(패널·창 밖)이나 자기 자신 위에서 놓으면 취소 — 무동작이 곧
    // 취소 경로라 별도 Esc 처리가 없어도 안전하다.
    const int target = tile_at(global_pos);
    if (target < 0 || target == ch)
        return;
    m_wall->swap_channels(ch, target);
    note_event("Info", "SYS",
               QString("tile swap: CH%1 <-> CH%2")
                   .arg(ch + 1).arg(target + 1));
}

QNetworkAccessManager *LiveViewer::snapshot_net()
{
    if (!m_snap_net) {
        m_snap_net = new QNetworkAccessManager(this);
        Credentials::install_tls_pinning(m_snap_net);
        connect(m_snap_net, &QNetworkAccessManager::authenticationRequired, this,
                [](QNetworkReply *, QAuthenticator *auth) {
            // 같은 자격으로 두 번 물으면 비밀번호가 틀린 것이다 — 무한 재시도를
            // 만들지 않는다(camera_tuner 와 같은 규칙).
            if (auth->user() == Credentials::camera_user())
                return;
            auth->setUser(Credentials::camera_user());
            auth->setPassword(Credentials::camera_password());
        });
    }
    return m_snap_net;
}

void LiveViewer::note_event(const QString &sev, const QString &src,
                            const QString &msg)
{
    // 하단 이벤트 티커·24h 눈금자는 08-19 오후 사용자 요청으로 삭제됐다 —
    // 이벤트는 콘솔 로그로만 남긴다 (스트림 up/down·엣지맵 결과).
    // 표시 계층을 되살릴 일이 생기면 여기가 단일 통로다.
    qInfo().noquote() << QString("[Event] %1 · %2 · %3").arg(sev, src, msg);
}

void LiveViewer::keyPressEvent(QKeyEvent *ev)
{
    switch (ev->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:   // Shift 없이 '=' 키도 +로 취급
        adjust_delay(+DELAY_STEP_MS);
        break;
    case Qt::Key_Minus:
        adjust_delay(-DELAY_STEP_MS);
        break;
    default:
        QWidget::keyPressEvent(ev);
    }
}

void LiveViewer::adjust_delay(int delta_ms)
{
    m_delay_ms = qBound(DELAY_MIN_MS, m_delay_ms + delta_ms, DELAY_MAX_MS);

    for (ChannelView *view : m_views)
        view->set_playback_delay(m_delay_ms);

    QSettings settings("GuardX", "VMS");
    settings.setValue("playback_delay_ms", m_delay_ms);

    qDebug() << "[LiveViewer] 재생 지연 =" << m_delay_ms << "ms";
}
