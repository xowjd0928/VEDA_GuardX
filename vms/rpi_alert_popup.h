#pragma once

#include "device_control_page.h"

#include <QDateTime>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

/**
 * @brief RPi A/B/C 오프라인 팝업 (프레임 없는 모드리스 창) — FireAlertPopup과 같은 디자인
 *
 * 화재/버튼처럼 **확인을 누르기 전까진 안 닫힌다** — 끊겼다가 바로 다시
 * 붙어도(m_nodes[n].offline이 false로 돌아가도) "끊긴 적이 있었다"는
 * 사실 자체는 사람이 봐야 하므로 자동으로 사라지지 않는다
 * (FireAlertPopup의 m_fire_pending과 같은 설계).
 *
 * 화재/버튼과 달리 세 노드는 서로 배타적이지 않다(A·B·C가 동시에 끊길 수
 * 있다) — 그래서 "지금 보이는 쪽 하나"를 고르는 FireAlertPopup의
 * 우선순위 방식 대신, pending인 노드를 전부 한 목록으로 보여준다.
 *
 * FireAlertFeed처럼 싱글턴 feed로 빼지 않고 DeviceControlPage를 직접
 * 구독한다 — top_bar.cpp가 RPi C 상태를 공유 클래스 없이 각자 구독하기로
 * 한 것과 같은 판단(bool 몇 개짜리라 공유 비용이 중복보다 크다). 다만
 * DeviceControlPage는 TopBar와 달리 인스턴스가 하나뿐이라(MainWindow의
 * m_device) 새 구독을 또 만드는 대신 그 시그널을 그대로 구독한다.
 */
class RpiAlertPopup : public QWidget
{
    Q_OBJECT

public:
    /** @param device node_state_changed를 구독할 DeviceControlPage 인스턴스.
     *  nullptr이면 아무 것도 구독하지 않는다(테스트/방어용). */
    explicit RpiAlertPopup(DeviceControlPage *device, QWidget *parent = nullptr);

signals:
    /** @brief "DEVICE CONTROL 보기" 클릭 — MainWindow가 그 탭을 연다 */
    void goto_device_control_requested();

private:
    void on_node_changed(DeviceControlPage::Node node, bool online);
    void refresh();
    /** @brief 창틀(면·테두리)만 다시 굽는다 — 점멸·테마 전환 공용 */
    void apply_frame();
    void reposition();
    /** @brief 지금 목록에 떠 있는 노드를 전부 확인 처리한다 */
    void dismiss_current();

    struct NodeSlot {
        QString label;          ///< "RPI-A · 환경센서" 등
        bool offline = false;   ///< 지금 실제로 끊겨 있는가 (자동으로 갱신됨)
        bool pending = false;   ///< 확인 안 된 오프라인 전이가 있는가 (자동으로 안 꺼짐)
        QDateTime since;
    };
    /// 인덱스가 DeviceControlPage::Node(A=0,B=1,C=2)와 그대로 대응
    NodeSlot m_nodes[3];

    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QLabel *m_time = nullptr;
    QPushButton *m_btn_device = nullptr;
    QPushButton *m_btn_close = nullptr;

    QTimer *m_pulse = nullptr;
    bool m_pulse_on = true;
};
