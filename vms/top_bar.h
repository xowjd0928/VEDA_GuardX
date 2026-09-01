#pragma once

#include <QWidget>

#include "camera_status.h"

class QHBoxLayout;
class QLabel;
class QPushButton;
class QVariantAnimation;

/**
 * @brief 워크스페이스 타이틀 바 (40px) — G 마크 · 화면 탭 · 상태 필 · 시계
 *
 * 08-19 디자인: 탭(NavRail)이 이 바 안에 산다 — MainWindow 가 embed_nav()
 * 로 끼운다. 자원 pill(CPU/MEM/NPU)과 APPS pill은 CameraStatus 싱글턴을
 * 구독해 1초 폴링으로 갱신되는 실데이터다 — 어느 탭에서든 상시 보인다.
 */
class TopBar : public QWidget
{
    Q_OBJECT

public:
    explicit TopBar(QWidget *parent = nullptr);

    /** @brief 워크스페이스 탭 줄을 로고 옆에 끼운다 (소유권은 레이아웃으로) */
    void embed_nav(QWidget *nav);

public slots:
    /** @brief 채널 스트림 생존 집계 (LiveViewer stream_health_changed) */
    void set_cam_health(int up, int total);

signals:
    /** @brief APPS pill 클릭 — CAMERA 탭으로 점프 */
    void camera_tab_requested();

    /** @brief 사용자 칩 메뉴의 [비밀번호 변경] (§5b) */
    void password_change_requested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void update_clock();

    void on_resources(const CameraResources &res);
    void on_apps(const QVector<CameraAppInfo> &apps);
    void on_link_state(CameraStatus::LinkState state);
    /** @brief 보간 중인 표시값(m_shown)으로 자원 pill 텍스트 재조립 */
    void render_resource_pill();
    void render_apps_pill();
    /** @brief 폭이 모자라면 정적 pill부터 숨긴다 (실데이터 우선) */
    void apply_width_budget();

    /**
     * @brief pill 하나의 폭이 **실제로 변했을 때만** 폭 예산을 다시 잡는다
     *
     * 자원 pill 은 값이 굴러가는 매 프레임 다시 그려진다. 그때마다 레이아웃을
     * invalidate 하면 창 전체가 매 프레임 다시 배치된다 — 눈에 띄는 떨림까지는
     * 아니어도 순수한 낭비다. 폭이 그대로면(대개 그렇다) 아무것도 안 한다.
     * @param cache 직전 폭을 담아 두는 곳 (호출부가 pill 마다 하나씩 들고 있다)
     */
    void budget_if_width_changed(QWidget *pill, int &cache);

    void render_cam_pill();
    /** @brief DB pill + RPI pill의 B 점 (둘 다 MQTT 브로커 = RPi B 생존) */
    void render_db_pills();

    /** @brief 사용자 칩 "● 이름 · 역할" (§6b). 오프라인이면 역할 자리에 읽기 전용 */
    void render_user_chip();
    void show_user_menu();

    QHBoxLayout *m_lay = nullptr;   ///< embed_nav 삽입 지점 계산용
    QPushButton *m_user_chip = nullptr;
    QLabel *m_clock_kst = nullptr;
    QLabel *m_clock_utc = nullptr;

    // ---- 카메라 자원·앱 pill (CameraStatus 구독) ----
    QLabel *m_res_pill = nullptr;
    QLabel *m_apps_pill = nullptr;
    QVariantAnimation *m_anim = nullptr;  ///< 값 전이 보간 (폴링 깜빡임 금지)
    double m_shown[3] = {0, 0, 0};        ///< 화면에 그려진 CPU/MEM/NPU
    double m_from[3] = {0, 0, 0};
    double m_to[3] = {0, 0, 0};
    bool m_have_res = false;              ///< 첫 표본 전엔 "—"
    QVector<CameraAppInfo> m_apps;
    CameraStatus::LinkState m_link = CameraStatus::LinkState::Offline;

    /** @brief 좁을 때 숨겨도 되는 정적 pill (앞에서부터 희생) */
    QVector<QWidget *> m_droppable;
    /// 자원·APPS pill 의 직전 sizeHint 폭 (budget_if_width_changed 용)
    int m_res_pill_w = -1;
    int m_apps_pill_w = -1;

    // ---- 승격된 pill (실데이터) ----
    QLabel *m_pill_cam = nullptr;
    QLabel *m_pill_rpi = nullptr;
    QLabel *m_pill_db = nullptr;
    int m_cam_up = -1;      ///< 살아있는 채널 수 (-1 = 아직 미보고)
    int m_cam_total = 4;
    int m_db_state = -1;    ///< -1 미확인 · 0 끊김 · 1 연결
    bool m_rpic_online = false;   ///< guardx/status/rpic (LWT) — 화면엔 2색뿐이라 bool로 충분

    /** @brief RPi C 생존 신호 수신 — guardx/status/rpic */
    void on_rpic_status(const QByteArray &payload);
};
