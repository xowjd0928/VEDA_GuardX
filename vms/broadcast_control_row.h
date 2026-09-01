#pragma once

#include <QColor>
#include <QWidget>

class BroadcastController;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;

/** Independent live-broadcast row for the existing actuator list. */
class BroadcastControlRow : public QWidget
{
    Q_OBJECT

public:
    explicit BroadcastControlRow(QWidget *parent = nullptr);

private:
    void sync_ui(bool active);
    void set_level(double rms_db);
    void set_volume_label(int percent);

    /** @brief ON 클릭 — 다른 VMS 가 방송 중이면 인수 확인창을 먼저 띄운다 */
    void on_clicked_on();

    /**
     * @brief 인수 확인창. 확인하면 true.
     *
     * 일반적인 방송 시작/종료에는 확인창을 띄우지 않는다 — 매번 물으면
     * 사람이 읽지 않고 누르게 되고, 그러면 정작 남의 방송을 끊는 순간에도
     * 그냥 누른다.
     */
bool confirm_takeover(const QString &owner);
    void set_status_color(const QColor *slot);

    BroadcastController *m_broadcast = nullptr;
    QLabel *m_status = nullptr;
    /// 상태 글자색은 **팔레트 슬롯 포인터**로 기억한다 — QColor 값을 복사해
    /// 두면 테마 전환 뒤에도 옛 색으로 다시 칠해진다. nullptr = 아직 색을
    /// 지정한 적 없음(기본 글자색 유지).
    const QColor *m_status_color = nullptr;
    QPushButton *m_on = nullptr;
    QPushButton *m_off = nullptr;
    QProgressBar *m_level = nullptr;      ///< 송출 오디오 레벨(마이크 확인용)
    /// 방송 출력 음량. 사이렌과 무관하다 — 사이렌은 RPi C 로컬 재생이라
    /// 이 값을 지나지 않는다. 방송 전·방송 중 아무 때나 움직일 수 있다.
    QSlider *m_volume = nullptr;
    QLabel *m_volume_label = nullptr;
    // 08-10 제거: 전송방식(MQTT↔RTP) 전환 버튼 · 노캔 토글 버튼.
    // 전송은 RTP 전용으로 확정되고 노캔은 항상 켜므로 고를 것이 없다.
};
