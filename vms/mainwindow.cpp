#include "mainwindow.h"
#include "alert_popup.h"
#include "alert_popup_stack.h"
#include "analytics_page.h"
#include "audio_alert_center.h"
#include "auth.h"
// business_flow_page / report_page — 지금은 탭에서 빠져 있지만(Analytics가
// 대신한다) 되살리기 쉽게 남겨 둔다. analytics_page.h 가 두 헤더의 자료형
// (AnalyticsFlowRow · ReportPage::ChanData)을 쓰므로 어차피 따라 들어온다.
#include "business_flow_page.h"
#include "camera_page.h"
#include "crowd_page.h"
#include "device_control_page.h"
#include "fire_alert_popup.h"
#include "live_viewer.h"
#include "login_page.h"
#include "nav_rail.h"
#include "report_page.h"
#include "rpi_alert_popup.h"
#include "zone_settings_page.h"
#include "theme.h"
#include "top_bar.h"

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDebug>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setup_ui();
    setWindowTitle("GuardX VMS");

    // 첫 크기 = 설계 폭(1600×900)과 **화면 작업영역 중 작은 쪽** (08-12).
    // 1600 고정이던 동안, 작업영역이 그보다 작은 화면(1536 논리 실측 —
    // 배율을 올린 노트북이 다 이렇다)에서 창이 작업표시줄 밑까지 뚫고
    // 내려가 하단 컨트롤이 가려졌다. 배율 100%든 125%든, 논리 화면이
    // 얼마가 되든 "보이는 영역 안에서 시작"이 불변식이다.
    // ⚠ 레이아웃 최소 크기가 작업영역보다 크면 Qt 가 최소로 되돌린다 —
    //    그 화면은 resize 로는 못 구한다(각 페이지의 최소 폭 문제).
    const QRect avail = screen() ? screen()->availableGeometry()
                                 : QRect(0, 0, 1600, 900);
    resize(qMin(1600, avail.width()), qMin(900, avail.height()));
    move(avail.topLeft());   // 시작 위치도 작업영역 안으로 (다중 모니터 포함)

    // 페이지별 최소 크기 진단 — 창 최소가 화면을 넘는 신고가 오면 범인을
    // 여기서 찾는다 (스택 최소 = 페이지 최소들의 최대라, 한 페이지가 전체를
    // 볼모로 잡는다).
    // ⚠ 페이지가 늘면 여기도 늘려야 한다 — 08-12 병합에서 flow 가 5번으로
    //    들어오며 settings 가 6번이 됐고, 이 배열이 6개라 라벨이 한 칸씩
    //    밀릴 뻔했다(진단 로그가 거짓말을 하는 건 없느니만 못하다).
    static const char *NAMES[] = { "live", "crowd", "analytics",
                                   "device", "camera", "settings" };
    const int named = int(sizeof(NAMES) / sizeof(NAMES[0]));
    for (int i = 0; i < m_pages->count() && i < named; ++i)
        qInfo().noquote() << QString("[main]   페이지 최소 %1 = %2x%3")
                                 .arg(NAMES[i])
                                 .arg(m_pages->widget(i)->minimumSizeHint().width())
                                 .arg(m_pages->widget(i)->minimumSizeHint().height());
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel) {
        auto *w = qobject_cast<QWidget *>(watched);
        // 포커스가 있으면 사용자가 그 칸을 쓰겠다고 밝힌 것이다 — 평소대로 돈다.
        // 없으면 지나가는 중이므로 휠은 페이지 스크롤에 넘긴다.
        if (w && !w->hasFocus()) {
            event->ignore();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setup_ui()
{
    m_top_bar = new TopBar(this);
    m_nav = new NavRail(this);

    // 페이지 인덱스 = NavRail 버튼 순서 (nav_rail.cpp의 items[]와 반드시 일치).
    // 제목은 디자인의 data-screen-label.
    // TRACK 페이지는 없다 — 동선은 LiveViewer 우측 TrackingPanel이 담당한다.
    // 긴 페이지는 스크롤로 감싼다 (08-12). 스택의 최소 크기 = 페이지 최소들의
    // **최대**라, 스크롤 없는 긴 페이지 하나가 창 전체의 최소 크기를 볼모로
    // 잡는다 — 실측: SETTINGS 1070x1112 · DEVICE 1336x840 이 창 최소를
    // 1420x1160 으로 끌어올려, 작업영역이 그보다 작은 화면(125% 노트북 =
    // 논리 1536x816)에서 창이 작업표시줄을 뚫고 레이아웃이 눌려 글자가
    // 깨졌다(4번 신고의 구조적 원인). 페이지 내부는 안 고친다 — 감싸는 것은
    // 소유자가 다른 파일을 건드리지 않는 유일한 자리다.
    // ⚠ #RScroll(투명)을 재사용하면 안 된다 — 투명 배경이 페이지 뒤의 어두운
    //    크롬을 그대로 비춰, 라이트 테마에서 페이지만 검게 남는다(08-12 실측.
    //    REPORT 가 RScroll 을 쓰면서 자기 팔레트로 배경을 덮는 이유가 이것).
    //    #PageScroll 은 전역 QSS 가 @bg0 로 칠한다.
    const auto wrap_scroll = [this](QWidget *page) {
        auto *sc = new QScrollArea(this);
        sc->setObjectName("PageScroll");
        sc->setWidgetResizable(true);
        sc->setFrameShape(QFrame::NoFrame);
        sc->setWidget(page);
        // ⚠ 스크롤을 붙이면 **휠이 흉기가 된다** (08-12 실측). 페이지를 굴리다
        //    커서가 스핀박스 위를 지나면 그 값이 조용히 바뀐다 — SETTINGS 에는
        //    화재 판단 임계 22칸이 있고, 실제로 시험 중에 위험 임계가 90→83 으로
        //    바뀌어 있었다. [적용] 을 안 눌러 서버로 가진 않았지만, 다음 사람이
        //    그 폼을 그대로 적용하면 화재 기준이 바뀐다.
        //    → 포커스가 없는 스핀박스·콤보박스는 휠을 무시한다(클릭해 포커스를
        //      준 뒤에는 평소대로 돈다). 스크롤을 감싼 이 자리가 책임 소재다.
        for (QWidget *w : page->findChildren<QWidget *>()) {
            if (!qobject_cast<QAbstractSpinBox *>(w) &&
                !qobject_cast<QComboBox *>(w))
                continue;
            w->setFocusPolicy(Qt::StrongFocus);   // 호버만으로 포커스가 가지 않게
            w->installEventFilter(this);
        }
        return sc;
    };

    m_pages = new QStackedWidget(this);
    m_live = new LiveViewer(this);
    m_pages->addWidget(m_live);                                       // 0 live
    m_pages->addWidget(new CrowdPage(this));                          // 1 heat
    // Predictions(ReportPage) + Flow(BusinessFlowPage) 를 합친 화면. 08-20에
    // 시안이 채택되어 **원본 두 탭을 대신한다**.
    //
    // 두 원본은 지우지 않았다 — 파일도 CMake 항목도 그대로라 계속 빌드된다
    // (그래야 되살릴 때 썩어 있지 않다). 다시 넣으려면 여기 두 줄을 되살리고
    //   m_pages->addWidget(new ReportPage(this));        // Predictions
    //   m_pages->addWidget(new BusinessFlowPage(this));  // Flow
    // nav_rail.cpp 의 items[] 에 같은 순서로 항목을 넣은 뒤,
    // nav_rail.h 의 DEVICE_INDEX·CAMERA_INDEX 와 아래 NAMES[] 를 맞추면 된다.
    m_pages->addWidget(new AnalyticsPage(this));                      // 2 analytics
    m_device = new DeviceControlPage(this);
    m_pages->addWidget(wrap_scroll(m_device));                        // 3 device
    m_pages->addWidget(new CameraPage(this));                         // 4 camera
    m_pages->addWidget(wrap_scroll(new ZoneSettingsPage(this)));      // 5 settings

    connect(m_nav, &NavRail::screen_selected,
            m_pages, &QStackedWidget::setCurrentIndex);

    // top_bar CAM pill — 채널 스트림 생존 집계 (형제 위젯이라 여기서 배선)
    connect(m_live, &LiveViewer::stream_health_changed,
            m_top_bar, &TopBar::set_cam_health);

    // APPS pill 클릭 → CAMERA 탭 점프 ("이상 감지 → 원클릭 드릴다운" 동선)
    connect(m_top_bar, &TopBar::camera_tab_requested, this,
            [this] { go_to(NavRail::CAMERA_INDEX); });

    // 혼잡 critical 경보 — 화면이 아니라 떠 있는 팝업으로 알린다.
    // 모드리스라 조작을 막지 않는다.
    m_alert_popup = new AlertPopup(this);
    connect(m_alert_popup, &AlertPopup::goto_live_requested, this,
            [this] { go_to(0); });

    // 오디오(비명/총성) 경보 — 혼잡 팝업과 독립.
    //
    // 팝업 하나가 아니라 카드 여러 장이다. 종류(비명·총성)마다, 그리고
    // 10분 집계 구간마다 따로 뜨고 `확인`을 눌러야 사라진다 — 자리 배치와
    // 장수 제한은 AudioAlertCenter 가 맡는다(this 가 부모).
    AudioAlertCenter::instance()->attach(this);
    connect(AudioAlertCenter::instance(),
            &AudioAlertCenter::goto_live_requested, this,
            [this](int) { go_to(0); });

    // 화재·비상버튼 경보 — 같은 이유로 팝업. AlertPopup과 동시에 뜰 수 있는데
    // (혼잡·화재는 서로 무관한 판정), FireAlertPopup의 reposition()이 더
    // 아래쪽에 자리를 잡아 겹치지 않는다.
    m_fire_alert_popup = new FireAlertPopup(this);
    connect(m_fire_alert_popup, &FireAlertPopup::goto_device_control_requested, this,
            [this](int zone_id) {
        if (!go_to(NavRail::DEVICE_INDEX))
            return;
        // 사건이 난 구역까지 골라준다 — 화면만 띄우고 다른 구역을 보여주면
        // 경보를 보고 넘어온 사람이 엉뚱한 센서값을 읽게 된다.
        m_device->show_zone(zone_id);
    });

    // 비상 버튼은 CCTV로 보낸다 — 화재와 달리 "사람이 눌렀다"는 신호라
    // 지금 봐야 할 것이 센서 수치가 아니라 현장 영상이다.
    // 채널 전체화면 확대는 없다(08-19 — LIVE는 항상 2×2 그리드). 어느 구역인지는
    // 해당 타일의 경보 테두리·칩이 이미 가리킨다.
    connect(m_fire_alert_popup, &FireAlertPopup::goto_live_requested, this,
            [this](int) { go_to(0); });

    // RPi A/B/C 연결 끊김 — DeviceControlPage가 이미 계산해 둔 3개 노드
    // 상태의 변화를 구독한다(새 MQTT 구독을 또 만들지 않는다).
    m_rpi_alert_popup = new RpiAlertPopup(m_device, this);
    connect(m_rpi_alert_popup, &RpiAlertPopup::goto_device_control_requested, this,
            [this] { go_to(NavRail::DEVICE_INDEX); });

    auto *shell = new QWidget(this);

    // 오프라인 유예 배너 (§4b) — 브로커에 못 붙어 캐시된 역할로 들어온 상태.
    // 화면 맨 위에 둔다: 지금 보고 있는 것이 **확인되지 않은 권한**이라는 사실은
    // 어느 탭에 있든 보여야 한다. 평소엔 숨어 있어 자리를 먹지 않는다.
    m_offline_banner = new QLabel(shell);
    m_offline_banner->setFont(Theme::mono_font(11));
    m_offline_banner->setFixedHeight(28);
    m_offline_banner->setAlignment(Qt::AlignCenter);
    m_offline_banner->setAttribute(Qt::WA_StyledBackground);
    Theme::restyle(m_offline_banner, [] {
        // 새 색을 만들지 않는다 — 경고는 amber, 면은 본문 한 단 위(elevated).
        return QString("background:%1; color:%2; border-bottom:1px solid %2;")
            .arg(Theme::elevated.name(), Theme::amber.name());
    });
    m_offline_banner->hide();

    // 워크스페이스 셸 (08-19): 탭은 별도 줄이 아니라 **타이틀 바 안**에 산다
    // (디자인의 한 줄 바). 좌측 레일이 사라져 영상 월이 그 폭만큼 넓어진다.
    m_top_bar->embed_nav(m_nav);

    auto *lay = new QVBoxLayout(shell);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_offline_banner);
    lay->addWidget(m_top_bar);
    lay->addWidget(m_pages, 1);

    // ---- 로그인 게이트 -----------------------------------------------------
    m_login = new LoginPage(this);
    m_root = new QStackedWidget(this);
    m_root->addWidget(m_login);   // 0
    m_root->addWidget(shell);     // 1
    setCentralWidget(m_root);

    connect(m_login, &LoginPage::authenticated, this, [this] {
        m_root->setCurrentIndex(1);
    });

    // 자발적 비밀번호 변경(§5b) — 로그인 화면의 변경 폼을 재사용한다.
    // 폼을 두 벌 만들면 정책·문구가 언젠가 갈린다.
    connect(m_top_bar, &TopBar::password_change_requested, this, [this] {
        m_root->setCurrentIndex(0);
        m_login->begin_change(false);
    });
    connect(m_login, &LoginPage::change_finished, this, [this] {
        if (Auth::instance()->logged_in())
            m_root->setCurrentIndex(1);
    });

    // 경보 팝업은 로그인 전에는 뜨지 않는다. 보이는 것만의 문제가 아니라,
    // 팝업의 [확인]이 **미확인 경보를 지우는 조작**이라 로그인 안 한 사람이
    // 누르면 진짜 운영자가 그 경보를 못 보게 된다. 보류된 팝업은 잠금이
    // 풀릴 때 그대로 다시 뜬다(AlertPopupStack::set_gated).
    AlertPopupStack::instance()->set_gated(!Auth::instance()->logged_in());

    // 로그아웃(단계 5)이나 세션 만료로 인증이 풀리면 문이 다시 닫힌다.
    // 지금 이 경로를 만들어 두는 이유는, 나중에 로그아웃 버튼을 붙이는
    // 사람이 화면 복귀까지 따로 배선하다 빠뜨리는 자리를 없애기 위해서다.
    connect(Auth::instance(), &Auth::state_changed, this, [this](Auth::State s) {
        AlertPopupStack::instance()->set_gated(s != Auth::State::LoggedIn);
        if (s == Auth::State::LoggedIn)
            m_root->setCurrentIndex(1);       // 자동 로그인도 이 경로로 들어온다
        // 서버가 `must_change_password` 로 거절해 상태가 되돌아온 경우 —
        // 셸을 보고 있던 중이므로 여기서 로그인 페이지로 옮겨야 변경 폼이 보인다.
        if (s == Auth::State::MustChangePassword)
            m_root->setCurrentIndex(0);
        if (s == Auth::State::LoggedOut && m_root->currentIndex() != 0) {
            m_root->setCurrentIndex(0);
            m_login->reset();
        }
        update_offline_banner();
    });
    connect(Auth::instance(), &Auth::verification_changed, this,
            [this](bool) { update_offline_banner(); });
    update_offline_banner();
}

void MainWindow::update_offline_banner()
{
    Auth *auth = Auth::instance();
    const bool grace = auth->logged_in() && !auth->verified();
    if (!grace) {
        m_offline_banner->hide();
        return;
    }

    // 기획서 §4b 의 문구를 그대로 쓴다 + 무엇이 막히는지 한 마디.
    const QDateTime last = auth->last_verified_at();
    m_offline_banner->setText(
        QString("Offline - last verified %1 · read only")
            .arg(last.isValid() ? last.toString("MM-dd HH:mm")
                                : QString("unknown")));
    m_offline_banner->show();
}

bool MainWindow::go_to(int page_index)
{
    if (!Auth::instance()->logged_in()) {
        // 로그인 전에 온 이동 요청(경보 팝업의 버튼)은 삼킨다. 조용히 버리지
        // 않고 남긴다 — "경보를 눌렀는데 아무 일도 없다"의 원인이 로그인이라는
        // 것을 로그에서 바로 알 수 있어야 한다.
        qInfo() << "[MainWindow] 로그인 전 화면 이동 요청 무시:" << page_index;
        return false;
    }
    m_nav->set_current(page_index);
    m_pages->setCurrentIndex(page_index);
    return true;
}
