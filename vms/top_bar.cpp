#include "top_bar.h"
#include "auth.h"
#include "fire_zone_map.h"
#include "mqtt_link.h"
#include "theme.h"
#include "zone_sensor_store.h"

#include <QSettings>

#include <QAction>
#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

/**
 * @brief 상태 필 하나: 색 점 + 모노 라벨
 * @param dot 팔레트 **슬롯 포인터** — 값을 복사하면 테마 전환 때 점 색이 굳는다
 */
QLabel *make_pill(const QString &label, const QColor *dot, QWidget *parent)
{
    auto *pill = new QLabel(parent);
    pill->setObjectName("ChromePill");
    pill->setFixedHeight(26);  // 4a 칩: padding 5px 10px + 12px mono
    pill->setFont(Theme::mono_font(11));
    auto paint = [pill, label, dot] {
        pill->setText(
            QString::fromUtf8("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; %2")
                .arg(dot->name(), label));
    };
    paint();
    Theme::on_theme_changed(pill, paint);  // 실데이터 필은 아래 render_*가 덮어쓴다
    return pill;
}

QString fmt_time(const QDateTime &dt)
{
    return dt.toString("HH:mm:ss");
}

/**
 * @brief 자원 % → 색. occ_color 임계와 같은 언어(>70 alarm · >45 amber).
 *
 * 정상 구간만 다르다 — CPU/MEM은 green, NPU는 accent(AI 카메라의 상징,
 * 기획서 §5-D "NPU 특별 취급").
 */
QColor metric_color(double v, bool npu)
{
    // 크롬이 어두우면 밝은 상태색을 쓴다 — 흰 배경용으로 눌러 놓은 초록/황은
    // 검은 상단바 위에서 읽히지 않는다 (3a 하이브리드).
    if (Theme::chrome_is_dark()) {
        if (v > 70) return Theme::OnVideo::alarm;
        if (v > 45) return Theme::OnVideo::amber;
        return npu ? Theme::OnVideo::accent : Theme::OnVideo::green;
    }
    if (v > 70) return Theme::alarm;
    if (v > 45) return Theme::amber;
    return npu ? Theme::accent : Theme::green;
}

/**
 * @brief 크롬 위에서 읽히는 상태색
 *
 * 크롬이 어두운 테마(3a)에선 흰 배경용으로 눌러 놓은 초록/황/적이 검은 바
 * 위에서 뭉갠다. 그럴 땐 밝은 팔레트(OnVideo)를 쓴다.
 */
QColor chrome_ok()   { return Theme::chrome_is_dark() ? Theme::OnVideo::green : Theme::green; }
QColor chrome_warn() { return Theme::chrome_is_dark() ? Theme::OnVideo::amber : Theme::amber; }
QColor chrome_bad()  { return Theme::chrome_is_dark() ? Theme::OnVideo::alarm : Theme::alarm; }

} // namespace

TopBar::TopBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("TopBar");
    setAttribute(Qt::WA_StyledBackground);
    // 08-19 워크스페이스: 타이틀 바 한 줄 = [G 마크 | 화면 탭 | 상태 필 |
    // 사용자 | 시계]. 제품명 텍스트·Site 문구는 바에서 뺐다 — 디자인은 G
    // 마크만 남기고, 현장 이름은 Live 좌측 패널의 트리 헤더가 보여준다.
    setFixedHeight(40);

    // ⚠ **이 줄이 창의 최소 폭을 정하게 두면 안 된다.**
    //   상단바는 앱에서 가로로 가장 긴 줄이다(마크 + 탭 7개 + 상태 필 6개 +
    //   사용자 칩 + 시계). 레이아웃이 요구하는 최소 폭이 화면보다 커지면
    //   창이 그 밑으로 못 줄어들고, **그 폭이 아래 페이지들에도 그대로**
    //   내려가 화면 오른쪽이 통째로 잘린다 — LIVE 의 TRACKING 패널이 잘리는
    //   것이 그 증상이다(08-19 실측: 탭 이름 Report→Predictions 로 길어지고
    //   관리자 계정으로 로그인하자 시계와 우측 패널이 화면 밖으로 밀렸다).
    //   명시적 최소 폭을 주면 위젯은 레이아웃 요구보다 작아질 수 있고,
    //   모자란 만큼은 apply_width_budget() 이 상태 필을 숨겨 해결한다.
    setMinimumWidth(Theme::px(1040));

    auto *lay = new QHBoxLayout(this);
    m_lay = lay;
    lay->setContentsMargins(14, 0, 14, 0);
    lay->setSpacing(0);

    // 브랜드 마크 (오렌지 — #Logo QSS)
    auto *logo = new QLabel("G", this);
    logo->setObjectName("Logo");
    logo->setFixedSize(24, 24);
    logo->setAlignment(Qt::AlignCenter);
    logo->setFont(Theme::ui_font(14, 700));
    logo->setToolTip(QString::fromUtf8("GuardX VMS 1.0"));
    lay->addWidget(logo);
    lay->addSpacing(10);
    // ← embed_nav() 가 여기(인덱스 2)에 워크스페이스 탭 줄을 끼운다
    lay->addSpacing(14);

    // 상태 필. CAM=채널 스트림 집계 · RPI A=환경센서 신선도(B 경유) ·
    // RPI B/DB=MQTT 브로커 · POLL=레지스트리 실값. RPI C만 올라오는 신호가
    // 없어 미확인(dim) 유지 — 모르는 것을 초록으로 칠하지 않는다.
    // 폭이 모자라면 m_droppable 순서(POLL부터)로 숨김.
    m_pill_cam = make_pill("CAM —", &Theme::chromeTextDim, this);
    m_pill_rpi = make_pill(QString::fromUtf8("RPi A·B·C"), &Theme::chromeTextDim, this);
    m_pill_db = make_pill("DB —", &Theme::chromeTextDim, this);

    // 박스 폴링 주기 — detection_feed와 같은 규칙(0=연속, 30~1000 클램프).
    // 기동 시 1회 읽음(실행 중 안 바뀜). 주 공급은 ONVIF 메타 푸시(5Hz)고
    // HTTP 폴은 대조표 갱신용이라는 맥락은 툴팁으로.
    const int poll_ms = [] {
        const int v = QSettings("GuardX", "VMS")
                          .value("detection_poll_ms", 50).toInt();
        return v <= 0 ? 0 : qBound(30, v, 1000);
    }();
    auto *pill_poll = make_pill(poll_ms == 0 ? QStringLiteral("Poll cont")
                                             : QString("Poll %1ms").arg(poll_ms),
                                &Theme::chromeTextDim, this);
    pill_poll->setToolTip(
        "HTTP box polling interval (registry detection_poll_ms)\n"
        "boxes come mainly from ONVIF metadata push (5 Hz); HTTP backs it up");

    for (QWidget *p : std::initializer_list<QWidget *>{
             m_pill_cam, m_pill_rpi, m_pill_db, pill_poll}) {
        lay->addWidget(p);
        lay->addSpacing(8);
    }
    // 숨기는 순서 = 덜 중요한 것부터. 자원·APPS 필은 실데이터라 **맨 마지막**
    // 에 양보한다(생성은 아래에서 하므로 여기서는 정적 필만 담고, 뒤에서
    // 이어 붙인다).
    m_droppable = {pill_poll, m_pill_rpi, m_pill_db, m_pill_cam};

    // ---- 카메라 자원·앱 pill (CameraStatus 실데이터, 1초) ----
    m_res_pill = make_pill("", &Theme::chromeTextDim, this);
    lay->addWidget(m_res_pill);
    lay->addSpacing(8);

    m_apps_pill = make_pill("", &Theme::chromeTextDim, this);
    m_apps_pill->setCursor(Qt::PointingHandCursor);
    m_apps_pill->installEventFilter(this);  // 클릭 → CAMERA 탭 점프
    lay->addWidget(m_apps_pill);

    // 정적 필을 다 숨기고도 모자라면 이 둘까지 — 시계와 사용자 칩("지금 누구
    // 인가")은 어떤 경우에도 남긴다.
    m_droppable << m_apps_pill << m_res_pill;

    lay->addStretch(1);

    // SIMULATE FIRE EVENT 버튼은 VEDA-174에서 제거됐다 — RPi A/B가 실제
    // 화재 판단을 올려 보내는 지금, 가짜 경보 버튼은 상단바에 둘 이유가 없다.

    // 사용자 칩 (§6b) — "지금 누구인가". 클릭하면 로그아웃 메뉴.
    // ⚠ 역할을 **색으로 구분하지 않는다** — 상태색은 건강 표시 전용이다.
    //    역할은 글자로 적는다.
    m_user_chip = new QPushButton(this);
    m_user_chip->setObjectName("ChromeBtn");   // 크롬 위 버튼 (테마 §3)
    m_user_chip->setFont(Theme::ui_font(12, 500));
    m_user_chip->setFixedHeight(26);
    m_user_chip->setCursor(Qt::PointingHandCursor);
    connect(m_user_chip, &QPushButton::clicked, this, &TopBar::show_user_menu);
    lay->addWidget(m_user_chip);
    lay->addSpacing(16);

    Auth *auth = Auth::instance();
    connect(auth, &Auth::state_changed, this, [this] { render_user_chip(); });
    connect(auth, &Auth::verification_changed, this, [this] { render_user_chip(); });
    render_user_chip();

    // 시계 (KST 크게 + UTC 작게)
    auto *clock_box = new QWidget(this);
    auto *clock_lay = new QVBoxLayout(clock_box);
    clock_lay->setContentsMargins(0, 0, 0, 0);
    clock_lay->setSpacing(1);
    m_clock_kst = new QLabel(clock_box);
    m_clock_kst->setFont(Theme::mono_font(15));       // 4a: clock 15/400 mono
    m_clock_kst->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_clock_utc = new QLabel(clock_box);
    m_clock_utc->setFont(Theme::mono_font(10));
    m_clock_utc->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    clock_lay->addWidget(m_clock_kst);
    clock_lay->addWidget(m_clock_utc);
    lay->addWidget(clock_box);

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TopBar::update_clock);
    // RPi A는 "값이 언제 마지막으로 왔나"로 판정한다 — 끊긴 것은 시간이 지나야
    // 알 수 있으므로 수신 때만이 아니라 1초마다 다시 본다 (DEVICE 화면과 동일).
    connect(timer, &QTimer::timeout, this, &TopBar::render_db_pills);
    connect(ZoneSensorStore::instance(), &ZoneSensorStore::updated,
            this, [this](int) { render_db_pills(); });
    timer->start(1000);
    update_clock();

    // ---- CameraStatus 구독 ----
    // 1초 폴링 갱신이 깜빡임이 되면 안 된다 — 새 값은 즉시 찍지 않고
    // 400ms 동안 이전 표시값에서 굴려간다(숫자 롤링).
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(400);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    m_anim->setStartValue(0.0);
    m_anim->setEndValue(1.0);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        const double t = v.toDouble();
        for (int i = 0; i < 3; ++i)
            m_shown[i] = m_from[i] + (m_to[i] - m_from[i]) * t;
        render_resource_pill();
    });

    auto *cam = CameraStatus::instance();
    connect(cam, &CameraStatus::resources_changed, this, &TopBar::on_resources);
    connect(cam, &CameraStatus::apps_changed, this, &TopBar::on_apps);
    connect(cam, &CameraStatus::link_state_changed, this, &TopBar::on_link_state);
    m_link = cam->link_state();
    if (cam->resources().valid())
        on_resources(cam->resources());
    if (!cam->apps().isEmpty())
        on_apps(cam->apps());
    render_resource_pill();
    render_apps_pill();

    // ---- DB·RPI B pill: MQTT 브로커(RPi B에서 돈다) 연결 상태 ----
    connect(MqttLink::instance(), &MqttLink::online_changed, this, [this](bool on) {
        m_db_state = on ? 1 : 0;
        render_db_pills();
    });
    if (MqttLink::instance()->online())
        m_db_state = 1;  // 이미 붙어 있으면 초록으로 시작 (시그널을 놓친 경우)

    // RPi C 생존 신호 — device_control_page.cpp와 같은 토픽을 각자 구독한다.
    // 공유 상태 클래스 하나로 묶기엔 bool 하나짜리라 중복이 더 단순하다.
    MqttLink::instance()->subscribe(
        "guardx/status/rpic",
        [this](const QByteArray &p) { on_rpic_status(p); },
        1);

    render_db_pills();
    render_cam_pill();

    // 테마 전환 — 필 텍스트에 색을 구워 넣었으므로 다시 만든다
    Theme::on_theme_changed(this, [this] {
        render_resource_pill();
        render_apps_pill();
        render_cam_pill();
        render_db_pills();
        update_clock();
    });
}

void TopBar::embed_nav(QWidget *nav)
{
    // 로고(0)·간격(1) 뒤, 두 번째 간격 앞 = 인덱스 2. 탭은 바 높이(40)를 꽉
    // 채운다 — 활성 탭의 상단 2px 마커가 바 위 모서리에 붙어야 디자인대로
    // 읽힌다.
    m_lay->insertWidget(2, nav);
}

void TopBar::set_cam_health(int up, int total)
{
    m_cam_up = up;
    m_cam_total = total;
    render_cam_pill();
}

void TopBar::render_cam_pill()
{
    if (m_cam_up < 0) {  // LiveViewer가 아직 한 번도 보고 안 함
        m_pill_cam->setText(
            QString::fromUtf8("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; CAM —")
                .arg(Theme::chromeTextDim.name()));
        return;
    }
    const QColor c = m_cam_up == m_cam_total ? chrome_ok()
                     : m_cam_up == 0         ? chrome_bad()
                                             : chrome_warn();
    m_pill_cam->setText(
        QString::fromUtf8("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; CAM %2/%3")
            .arg(c.name())
            .arg(m_cam_up)
            .arg(m_cam_total));
    m_pill_cam->setToolTip("live channel streams / total");
}

void TopBar::render_db_pills()
{
    // 2색 규칙(device_control_page.cpp NodeState와 통일): 연결 확인만 초록,
    // 그 외(끊김·미확인 둘 다)는 빨강. 예전엔 끊김을 amber로 칠해서 "위험은
    // 아니고 주의만" 처럼 보였는데, 브로커가 끊기면 DB 조회 경로 자체가
    // 죽은 거라 그 정도로 가볍게 볼 상태가 아니다.
    const QColor b = m_db_state == 1 ? chrome_ok() : chrome_bad();
    m_pill_db->setText(
        QString::fromUtf8("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; DB %2")
            .arg(b.name(), m_db_state == 1 ? QStringLiteral("OK") : QStringLiteral("—")));
    m_pill_db->setToolTip("MQTT broker (RPi B) link - the DB query path");

    // ---- RPI pill: A·B·C를 각각 제 신호로 칠한다 ----
    //
    // A = 환경센서 스트림의 신선도. RPi A는 VMS와 직접 말하지 않지만(설계상
    //     VMS↔B만), 그 값이 B를 거쳐 계속 올라온다는 것 자체가 A 생존의 실신호다.
    //     ⚠ **더미 zone은 세지 않는다** — ZoneSensorStore가 가짜 값을 만들어
    //     넣기 때문에, 그것까지 세면 하드웨어가 죽어도 A가 영원히 초록이 된다.
    // C = guardx/status/rpic(LWT) — 접속 시 online, 끊기면 브로커가 대신
    //     offline을 뿌린다(guardx_protocol.h). m_rpic_online 초기값 false라
    //     한 번도 못 받았을 때도 자연히 빨강으로 시작한다.
    // 판정은 ZoneSensorStore 한 곳에만 있다 (더미 제외·재발행 배제·연속
    // 조건이 전부 거기 들어 있다 — 여기서 다시 계산하면 Device 탭과 갈라진다).
    const ZoneSensorStore::NodeAHealth ah =
        ZoneSensorStore::instance()->node_a_health();
    const qint64 age = ah.age_ms;
    // 브로커가 끊기면 A의 신선도 판정 자체가 의미 없다 (경로가 죽은 것이지
    // A가 죽었는지는 알 수 없다) — 그때도 그냥 빨강(2색 규칙이라 "미확인"을
    // 따로 표시할 색이 없다).
    const bool a_known = m_db_state == 1 && ah.seen;
    const QColor a = a_known && ah.streaming ? chrome_ok() : chrome_bad();
    const QColor c = m_rpic_online ? chrome_ok() : chrome_bad();

    m_pill_rpi->setText(
        QString::fromUtf8("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; "
                          "RPi <span style=\"color:%2\">A</span>·"
                          "<span style=\"color:%3\">B</span>·"
                          "<span style=\"color:%4\">C</span>")
            .arg(b.name(), a.name(), b.name(), c.name()));

    const QString a_note =
        !a_known   ? QString("unknown (nothing received yet)")
        : ah.streaming
            ? QString("healthy · streaming · last value %1 s ago").arg(age / 1000)
        : age > ZoneSensorStore::STALE_MS
            ? QString("stale · no new values for %1 s").arg(age / 1000)
            // 값은 신선한데 연속 조건을 아직 못 채웠다 = 이제 막 돌아왔거나
            // 띄엄띄엄 온다. "정상"이라고 하면 안 되는 구간이다.
            : QString("unsteady · %1 of %2 cycles in a row")
                  .arg(ah.streak).arg(ZoneSensorStore::STREAM_STREAK);
    m_pill_rpi->setToolTip(QString(
        "A = freshness of environment sensor data (via B) - %1\n"
        "B = live MQTT broker link\n"
        "C = actuator liveness signal (LWT) - %2").arg(
            a_note, m_rpic_online ? QString("connected")
                                  : QString("not connected")));
}

void TopBar::on_rpic_status(const QByteArray &payload)
{
    m_rpic_online = (payload == "online");
    render_db_pills();
}

bool TopBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_apps_pill && event->type() == QEvent::MouseButtonPress) {
        emit camera_tab_requested();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void TopBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    apply_width_budget();
}

void TopBar::budget_if_width_changed(QWidget *pill, int &cache)
{
    if (!pill)
        return;
    const int w = pill->sizeHint().width();
    if (w == cache)
        return;      // 폭 그대로 — 레이아웃을 건드릴 이유가 없다
    cache = w;
    apply_width_budget();
}

void TopBar::apply_width_budget()
{
    // 1600×900 기본 창엔 정적 pill + 실데이터 pill + 버튼 + 시계가 다 못
    // 들어간다. 실데이터(자원·APPS)가 이 확장의 목적이므로 정적 표시부터
    // 양보한다 — 넓은 화면(시연 모니터)에선 전부 보인다.
    QLayout *lay = layout();
    if (!lay)
        return;

    // ⚠ **전부 보이게 했다가 다시 숨기지 않는다.** 그렇게 하면 이 함수가
    //   불릴 때마다(자원 pill 은 값이 굴러가는 **매 프레임** 다시 그려진다)
    //   숨겨 둔 pill 이 한 프레임 나타났다 사라져 상단바가 통째로 떨린다
    //   (08-19 사용자 신고 "navigator tab is shaking really bad").
    //
    //   대신 산술로 판단한다: 지금 레이아웃이 요구하는 폭에 **숨겨 둔
    //   pill 들의 폭을 더해** "전부 보였을 때"를 계산하고, 거기서부터 앞
    //   순서대로(덜 중요한 것부터) 필요한 만큼만 뺀다. 결과가 현재 상태와
    //   같으면 위젯을 아예 건드리지 않는다 — 깜빡임의 원인이 사라진다.
    lay->invalidate();
    int needed = lay->sizeHint().width();
    for (QWidget *w : std::as_const(m_droppable))
        if (w->isHidden())
            needed += w->sizeHint().width();   // 간격(addSpacing)은 숨겨도 남는다

    const int budget = width();
    QVector<bool> want;
    want.reserve(m_droppable.size());
    for (QWidget *w : std::as_const(m_droppable)) {
        const bool hide = needed > budget;
        if (hide)
            needed -= w->sizeHint().width();
        want.append(!hide);
    }

    bool changed = false;
    for (int i = 0; i < m_droppable.size(); ++i) {
        const bool visible_now = !m_droppable[i]->isHidden();
        if (visible_now == want[i])
            continue;                       // 이미 원하는 상태 — 손대지 않는다
        m_droppable[i]->setVisible(want[i]);
        changed = true;
    }
    if (changed)
        lay->invalidate();
}

void TopBar::on_resources(const CameraResources &res)
{
    // 첫 표본은 0에서 차오르게(인트로), 이후엔 직전 표시값에서 굴린다
    for (int i = 0; i < 3; ++i)
        m_from[i] = m_shown[i];
    m_to[0] = res.cpu;
    m_to[1] = res.mem;
    m_to[2] = res.npu;
    m_have_res = true;

    // 툴팁: 상세 수치는 pill이 아니라 여기로 (52px 폭 예산)
    const double gb = 1024.0;
    m_res_pill->setToolTip(
        QString("RAM %1 / %2 GB available\nStorage %3 / %4 GB available\n"
                "details in the Camera tab")
            .arg(res.ram_free_mb / gb, 0, 'f', 1)
            .arg(res.ram_total_mb / gb, 0, 'f', 1)
            .arg(res.storage_free_mb / gb, 0, 'f', 2)
            .arg(res.storage_total_mb / gb, 0, 'f', 2));

    m_anim->stop();
    m_anim->start();
}

void TopBar::on_apps(const QVector<CameraAppInfo> &apps)
{
    const bool first = m_apps.isEmpty();
    m_apps = apps;
    render_apps_pill();
    if (first)
        apply_width_budget();  // "Apps —" → "APPS 4"로 폭이 처음 정해질 때
}

void TopBar::on_link_state(CameraStatus::LinkState state)
{
    m_link = state;
    render_resource_pill();
    render_apps_pill();
}

void TopBar::render_resource_pill()
{
    using LS = CameraStatus::LinkState;

    // 오프라인/재부팅/첫 표본 전 = 값을 아는 척하지 않는다 ("거짓 초록 금지")
    if (!m_have_res || m_link == LS::Offline || m_link == LS::Rebooting) {
        m_res_pill->setText(
            QString("<span style=\"color:%1\">CPU —·MEM —·NPU —</span>")
                .arg(Theme::chromeTextDim.name()));
        return;
    }

    // Stale(폴 1~2회 실패)이면 값은 유지하되 흐리게 — 낡았을 수 있다는 표시.
    // ⚠ 색까지 죽이지는 않는다: 임계 색상이 "지금 얼마나 높은가"를 나르므로
    //    낡았다고 회색으로 만들면 정작 봐야 할 신호가 사라진다. 알파만 낮춘다.
    // % 기호는 생략 — 52px 폭 예산(§7). 단위는 툴팁·CAMERA 탭이 말해준다.
    const bool dim = m_link == LS::Stale;
    QString parts[3];
    static const char *NAMES[3] = {"CPU", "MEM", "NPU"};
    for (int i = 0; i < 3; ++i) {
        QColor c = metric_color(m_to[i], i == 2);
        if (dim)
            c.setAlphaF(0.5);
        parts[i] = QString("<span style=\"color:%1\">%2</span>"
                           "&nbsp;<b style=\"color:%3\">%4</b>")
                       .arg(Theme::chromeTextDim.name(), NAMES[i],
                            QString("rgba(%1,%2,%3,%4)")
                                .arg(c.red()).arg(c.green()).arg(c.blue())
                                .arg(c.alphaF(), 0, 'f', 2))
                       .arg(qRound(m_shown[i]));
    }
    m_res_pill->setText(parts[0] + "·" + parts[1] + "·" + parts[2]);
    // ⚠ 폭 예산을 **여기서도** 다시 잡는다. 이 pill 은 "CPU —·MEM —·NPU —"
    //   에서 "CPU 31·MEM 58·NPU 60" 으로 **자라기** 때문이다. resizeEvent 에만
    //   걸어 두면 창 크기가 그대로인 채 내용만 늘어난 경우를 못 잡아,
    //   맨 오른쪽 시계가 창 밖으로 밀려 잘린다 (08-19 실측: 관리자 로그인 +
    //   카메라 온라인 상태에서 시계가 반쯤 잘렸다).
    //   단, 이 함수는 값이 굴러가는 매 프레임 불리므로 **폭이 변했을 때만**.
    budget_if_width_changed(m_res_pill, m_res_pill_w);
}

void TopBar::render_apps_pill()
{
    using LS = CameraStatus::LinkState;

    if (m_link == LS::Offline || m_link == LS::Rebooting || m_apps.isEmpty()) {
        const QString label = m_link == LS::Rebooting
                                  ? QString::fromUtf8("Reboot…")
                                  : QStringLiteral("Apps —");
        m_apps_pill->setText(
            QString::fromUtf8("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; "
                              "<span style=\"color:%1\">%2</span>")
                .arg(Theme::chromeTextDim.name(), label));
        m_apps_pill->setToolTip("camera app status - waiting for a connection");
        return;
    }

    // 최악 상태색 하나로 요약: 정지=alarm · 전이중/AutoStart꺼짐=amber · 평상시 green
    QColor worst = chrome_ok();
    QStringList tips;
    for (const CameraAppInfo &app : m_apps) {
        QString note;
        if (app.status != QLatin1String("Running")) {
            // Starting/Installing 같은 전이 상태는 경고, 완전 정지는 위험
            const bool transient = app.status.contains(QLatin1String("ing"));
            worst = transient ? (worst == chrome_bad() ? worst : chrome_warn())
                              : chrome_bad();
            note = app.status;
        } else if (!app.auto_start && !app.is_default) {
            if (worst != chrome_bad())
                worst = chrome_warn();
            note = QString("AutoStart off");
        }
        tips << QString("%1 %2 · %3%4")
                    .arg(app.id, app.status,
                         app.cpu >= 0 ? QString("CPU %1%").arg(app.cpu)
                                      : QString("resources —"),
                         note.isEmpty() ? QString() : QString(" ⚠ %1").arg(note));
    }

    m_apps_pill->setText(
        QString::fromUtf8("<span style=\"font-size:8px;color:%1\">●</span>&nbsp; "
                          "Apps %2")
            .arg(worst.name())
            .arg(m_apps.size()));
    m_apps_pill->setToolTip(tips.join('\n') + "\nclick: Camera tab");
    // "Apps —" -> "Apps 4" 로 폭이 변한다 (위 ⚠ 참조)
    budget_if_width_changed(m_apps_pill, m_apps_pill_w);
}

void TopBar::render_user_chip()
{
    Auth *auth = Auth::instance();
    if (!auth->logged_in()) {
        // 로그인 전에는 상단바가 보이지도 않지만(스택 0페이지), 로그아웃
        // 순간의 한 프레임에 옛 이름이 남지 않게 비운다.
        m_user_chip->hide();
        return;
    }

    // 오프라인 유예면 그 사실이 여기에도 보여야 한다 — 배너를 못 본 채
    // 버튼이 왜 꺼져 있는지 찾는 일이 없게(§4b).
    const QString role = auth->verified() ? Auth::role_text(auth->role())
                                          : QString("read only");

    // ⚠ **아이디를 반드시 보여준다.** 표시 이름만 쓰면 그 값이 역할과 같을 때
    //    (실서버 시드 계정의 display_name 이 "관리자"다) 칩이 "관리자 · 관리자"가
    //    되어 **지금 어느 계정으로 들어와 있는지 알 수 없다.**
    //    표시 이름은 아이디·역할과 다를 때만 덧붙인다.
    const QString id = auth->username();
    const QString name = auth->display_name();
    const bool name_adds_info =
        !name.isEmpty() && name != id && name != Auth::role_text(auth->role());

    m_user_chip->setText(name_adds_info
                             ? QString("● %1 (%2) · %3").arg(name, id, role)
                             : QString("● %1 · %2").arg(id, role));
    m_user_chip->setToolTip(
        QString("%1%2 · %3 - click for the menu")
            .arg(id, name_adds_info ? QString(" (%1)").arg(name) : QString(),
                 Auth::role_text(auth->role())));
    m_user_chip->show();
    apply_width_budget();   // 칩이 늘면 정적 pill 이 먼저 자리를 내준다
}

void TopBar::show_user_menu()
{
    QMenu menu(this);
    QAction *change = menu.addAction("Change password");
    // 오프라인 유예 중에는 서버에 못 닿으니 바꿀 수도 없다 — 눌리게 두면
    // "왜 안 되지"가 된다.
    change->setEnabled(Auth::instance()->verified());
    menu.addSeparator();
    QAction *logout = menu.addAction("Sign out");

    const QAction *picked = menu.exec(
        m_user_chip->mapToGlobal(QPoint(0, m_user_chip->height() + 4)));
    if (picked == logout)
        Auth::instance()->logout();
    else if (picked == change)
        emit password_change_requested();
}

void TopBar::update_clock()
{
    static QTimeZone kst = [] {
        QTimeZone tz("Asia/Seoul");
        return tz.isValid() ? tz : QTimeZone(9 * 3600);
    }();

    const QDateTime utc = QDateTime::currentDateTimeUtc();
    m_clock_kst->setText(QString("<span style=\"color:%3\">%1</span>"
                                 " <span style=\"font-size:%4px;color:%2\">KST</span>")
                             .arg(fmt_time(utc.toTimeZone(kst)),
                                  Theme::chromeTextDim.name(),
                                  Theme::chromeText.name())
                             .arg(Theme::px(10)));
    m_clock_utc->setText(QString("<span style=\"color:%1\">%2 UTC</span>")
                             .arg(Theme::chromeTextDim.name(), fmt_time(utc)));
}
