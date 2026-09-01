#pragma once

#include <QList>
#include <QWidget>

class NavButton;

class QTimer;

/**
 * @brief 상단 워크스페이스 탭 줄 (40px) — 6개 화면 전환 탭
 *
 * 08-19 워크스페이스 디자인: 좌측 세로 레일(84px)을 없애고 상단 가로 탭으로
 * 전환했다 — 영상 월이 그만큼 넓어진다. 클래스·파일명은 NavRail 그대로
 * 둔다(이름만 바꾸면 diff 가 커져 실제 변경이 리뷰에서 묻힌다).
 *
 * 활성: chromeSelBg 배경 + 상단 2px chromeSelText(오렌지) 라인 + chromeText.
 * 비활성: 투명 + chromeTextDim, 호버 시 chromeText.
 *
 * Live 탭에는 경보 배지가 붙는다 — 다른 화면을 보고 있어도 몇 채널이
 * 경보인지 눈에 들어와야 한다. critical이면 점멸하고, 평상시엔 타이머가
 * 아예 멈춰 있다. (즉시 알림은 AlertPopup이 맡는다)
 */
class NavRail : public QWidget
{
    Q_OBJECT

public:
    // ⚠ 탭을 넣고 빼면 이 상수들과 mainwindow.cpp 의 addWidget 순서·NAMES[]가
    //    함께 움직여야 한다 — 08-20 에 Analytics 가 Predictions·Flow 를
    //    대체하면서 Device 이후가 한 칸 **당겨졌다**(8탭 → 6탭).

    /** @brief 경보 배지가 붙는 탭 = Live (경보는 Live 타일에서 보인다) */
    static const int ALERT_INDEX = 0;

    /** @brief Device 탭 인덱스 — 화재/RPi 팝업의 "이동" 버튼이 참조 */
    static const int DEVICE_INDEX = 3;

    /** @brief Camera 탭 인덱스 — top_bar Apps pill 클릭 점프가 참조 */
    static const int CAMERA_INDEX = 4;

    explicit NavRail(QWidget *parent = nullptr);

    void set_current(int index);

signals:
    /** @brief 화면 선택됨 (QStackedWidget 인덱스와 동일) */
    void screen_selected(int index);

private:
    /** @brief AlertFeed 상태 -> Live 탭 배지 */
    void refresh_alert_badge();

    QList<NavButton *> m_buttons;
    QTimer *m_pulse = nullptr;
    bool m_pulse_on = true;
};
