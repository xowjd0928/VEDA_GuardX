#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

/**
 * @brief 혼잡 critical 경보 팝업 (프레임 없는 모드리스 창)
 *
 * DB 규약이 이미 이걸 가리킨다 — `alerts.broadcast_channel = 'vms_popup'`.
 *
 * **모드리스**다. 경보가 떴다고 감시를 막으면 안 된다 — 조작을 막지 않고
 * 메인 창 위에 떠 있기만 한다. critical 동안 테두리가 점멸하고, 해당 채널이
 * 전부 해제되면 스스로 닫힌다.
 *
 * warn은 팝업을 띄우지 않는다 (LIVE 타일 색으로 충분하다 — 주의 단계마다
 * 창이 뜨면 경보 피로로 정작 critical을 무시하게 된다).
 */
class AlertPopup : public QWidget
{
    Q_OBJECT

public:
    /** @param parent 메인 창 — 이 창을 기준으로 위치를 잡는다 */
    explicit AlertPopup(QWidget *parent = nullptr);

signals:
    /** @brief "LIVE로 이동" 클릭 — MainWindow가 LIVE 화면으로 전환한다 */
    void goto_live_requested();

private:
    void on_state_changed();
    void refresh();
    void reposition();

    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QLabel *m_time = nullptr;
    QPushButton *m_btn_live = nullptr;
    QPushButton *m_btn_close = nullptr;

    QTimer *m_pulse = nullptr;   ///< 떠 있을 때만 돈다
    bool m_pulse_on = true;
    bool m_dismissed = false;    ///< 닫기 누름 — 다음 critical 전이까지 안 뜬다
};
