#pragma once

#include <QMainWindow>

class AlertPopup;
class DeviceControlPage;
class FireAlertPopup;
class LiveViewer;
class LoginPage;
class NavRail;
class QLabel;
class QStackedWidget;
class RpiAlertPopup;
class TopBar;

/**
 * @brief GuardX VMS 메인 창 — TopBar(상단) + [ NavRail | 페이지 스택 ]
 *
 * 페이지 순서 = NavRail 버튼 순서: Live / Crowd / Report / Device / Zones.
 * Device만 PlaceholderPage고 나머지는 실화면이다 (스펙 §2).
 *
 * 화면이 아닌 것 넷: 동선(TRACK)은 LiveViewer 우측 `TrackingPanel`이,
 * 혼잡 critical 경보는 `AlertPopup`이, 화재·비상버튼 경보는 `FireAlertPopup`이,
 * RPi A/B/C 연결 끊김은 `RpiAlertPopup`이 맡는다 — 넷 다 어느 화면을 보고
 * 있든 동작해야 해서 탭에 넣으면 정작 필요할 때 못 본다.
 *
 * **가장 바깥에 스택이 하나 더 있다** (2026-08-11): 0=로그인, 1=본 화면.
 * 로그인 화면을 별도 창으로 만들지 않는 이유는 §6a 참조. 본 화면은 로그인
 * 전에 **미리 만들어 둔다** — 경보 팝업이 늦게 태어나면 이미 열려 있던
 * retained 경보를 놓치고(main.cpp의 구독 순서가 지키려던 것), 로그인 직후
 * RTSP 재연결을 기다려야 한다.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    /**
     * @brief 포커스 없는 스핀박스·콤보박스의 휠을 삼킨다 (08-12)
     *
     * DEVICE·SETTINGS 를 스크롤로 감싸면서 생긴 위험을 막는다 — 페이지를
     * 굴리다 커서가 값 위젯 위를 지나면 그 값이 조용히 바뀐다. SETTINGS 의
     * 화재 판단 임계 22칸이 그 사거리에 있다.
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setup_ui();

    /**
     * @brief 페이지 전환 — **로그인 전에는 아무 데도 안 간다**
     *
     * 경보 팝업 넷은 로그인 화면 위에도 뜬다(경보를 놓치는 쪽이 더 나쁜
     * 오류다). 그 팝업의 "이동" 버튼이 곧 로그인 우회로라, 전환을 이 한
     * 곳으로 모아 문을 여기서 잠근다. 호출부마다 검사를 흩뿌리면 새 호출부가
     * 생길 때 빠진다 — 실제로 08-10에 팝업 오프셋이 그렇게 어긋났다.
     *
     * @return 실제로 전환했으면 true (뒤따르는 조작을 이어갈지 판단용)
     */
    bool go_to(int page_index);

    /** @brief 오프라인 유예(§4b) 배너를 상태에 맞춰 보이거나 감춘다 */
    void update_offline_banner();

    QStackedWidget *m_root = nullptr;   ///< 0 = 로그인, 1 = 본 화면
    LoginPage *m_login = nullptr;
    QLabel *m_offline_banner = nullptr;
    TopBar *m_top_bar = nullptr;
    NavRail *m_nav = nullptr;
    QStackedWidget *m_pages = nullptr;
    LiveViewer *m_live = nullptr;                 ///< 비상버튼 팝업이 특정 채널을 띄울 때 필요
    DeviceControlPage *m_device = nullptr;        ///< 화재 팝업이 특정 구역으로 보낼 때 필요
    AlertPopup *m_alert_popup = nullptr;
    FireAlertPopup *m_fire_alert_popup = nullptr;
    RpiAlertPopup *m_rpi_alert_popup = nullptr;
};
