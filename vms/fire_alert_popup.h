#pragma once

#include <QDateTime>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

/**
 * @brief 화재/비상버튼 경보 팝업 (프레임 없는 모드리스 창) — AlertPopup(혼잡)과 같은 디자인
 *
 * 두 종류의 원인을 하나의 창으로 보여준다. 우선순위는 화재 > 버튼 — 화재가
 * 진행 중이면 버튼이 눌려도 화면은 화재 내용을 유지한다(화재가 더 위급하고,
 * 대개 버튼도 같은 화재 상황에서 눌리므로 중복 표시할 이유가 없다).
 *
 *   화재 : 혼잡 critical과 달리 **해제된다고 스스로 닫히지 않는다** — "화재가
 *          있었다"는 사실 자체를 사람이 반드시 인지해야 해서, 상황이 끝난
 *          뒤에도 "확인"을 누르기 전까지는 계속 떠 있는다(m_fire_pending).
 *          문구만 화재가 진행 중인지/이미 해제됐는지에 따라 달라진다.
 *   버튼 : FireAlertFeed::button_pressed()는 상태가 아니라 사건이라, 자동으로
 *          안 사라지고 "확인"을 눌러야만 닫힌다 — 사람이 실제로 확인했다는
 *          사실 자체가 중요하다. 화재와 같은 "확인해야 닫힘" 패턴이다.
 *
 * "Device Control 보기" 버튼은 오늘 만든 센서·액추에이터 화면으로 이동한다 —
 * 화재 원인 센서값을 그 자리에서 바로 확인하고 수동으로 대응할 수 있어서
 * congestion의 "LIVE 보기"보다 이쪽이 더 실질적인 다음 행동이다.
 */
class FireAlertPopup : public QWidget
{
    Q_OBJECT

public:
    explicit FireAlertPopup(QWidget *parent = nullptr);

signals:
    /**
     * @brief 화재 알림에서 "DEVICE CONTROL 보기" 클릭
     * @param zone_id 사건이 난 zone — MainWindow가 그 zone 탭까지 열어준다
     *
     * 화재는 센서값·액추에이터 상태를 봐야 판단이 서므로 Device Control로 간다.
     */
    void goto_device_control_requested(int zone_id);

    /**
     * @brief 비상 버튼 알림에서 "CCTV 보기" 클릭
     * @param zone_id 버튼이 눌린 zone — MainWindow가 그 구역 카메라를 띄운다
     *
     * 버튼은 화재와 목적이 다르다. "사람이 직접 눌렀다"는 신호라 지금 알아야
     * 할 것은 센서 수치가 아니라 **현장에 누가 왜 있는가**다. 그래서 같은
     * 팝업이라도 버튼일 때는 Device Control이 아니라 CCTV로 보낸다.
     */
    void goto_live_requested(int zone_id);

private:
    void on_fire_changed();
    void on_button_pressed(int cumulative_count, int zone_id, const QDateTime &ts);
    void refresh();
    /** @brief 창틀(면·테두리)만 다시 굽는다 — 점멸·테마 전환 공용 */
    void apply_frame();
    void reposition();
    /** @brief 지금 표시 중인 원인(화재 또는 버튼)만 확인 처리하고, 다른 쪽이
     *  아직 남아있으면 그쪽으로 이어서 보여준다 */
    void dismiss_current();

    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QLabel *m_time = nullptr;
    QPushButton *m_btn_device = nullptr;
    QPushButton *m_btn_close = nullptr;

    QTimer *m_pulse = nullptr;
    bool m_pulse_on = true;

    /// 아직 확인 안 된 화재 알림이 있는가 — fire.active(지금도 진행 중인지)와
    /// 완전히 독립적이다. 확정 전이에서 true로 세팅되고, dismiss_current()로만
    /// false가 된다(해제 전이로는 절대 안 꺼짐 — 1번 버그의 원인이었던 부분).
    bool m_fire_pending = false;

    bool m_button_pending = false;   ///< 버튼 눌림이 아직 확인 안 됨
    int m_button_count = 0;
    int m_button_zone = 0;
    QDateTime m_button_ts;
};
