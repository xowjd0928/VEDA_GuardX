#pragma once

#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QTimer;

/**
 * @brief CAMERA 탭 [이미지] 하위 탭 — ISP 최소 세트 (기획서 §3-D, 8단계)
 *
 * 채널별 현재값 조회 + 즉시 적용 콤보:
 *  - 역광 보정: camera CompensationMode (Off/BLC/HLC/WDR)
 *  - 화이트밸런스: whitebalance WhiteBalanceMode
 *  - IR LED: irled Mode
 *  - Simple Focus: focus control (지원 기종 한정 — 미지원이면 거부 표시)
 *  - 줌: focus ZoomContinuous (누르는 동안 In/Out, 떼면 Stop)
 *
 * SSDR·플립·프리셋 등 나머지 ISP는 T3 (기획서 §8 범위 밖).
 *
 * @section zoom 줌이 왜 여기(image.cgi)에 있나
 *
 * PNM-C16083RVQ 는 **팬/틸트 모터가 없다.** `ptzcontrol.cgi` 는 통째로
 * 608(Feature Not Implemented)을 돌려준다 — 인증·문법 문제가 아니라 장비에
 * 그 기능이 없다는 뜻이다. 줌·포커스만 모터라서 `image.cgi` 의 focus
 * 서브메뉴에 있다. (조사 노트: 「카메라 줌 — SUNAPI 조작법과 608 함정」 08-19)
 *
 * 렌즈는 채널마다 **따로** 있다 — Channel 0~3 을 반드시 넣어야 하고, 넷을
 * 다 당기려면 네 번 쏴야 한다. 배율은 1.7배(화각 100°→53°)뿐이라 한 스텝
 * (`Zoom=±10`)은 눈에 거의 안 보인다. 그래서 스텝 버튼 대신 **연속 줌**을
 * 쓴다 — 누르는 동안 움직이고 떼면 선다.
 */
class ImagePanel : public QWidget
{
    Q_OBJECT

public:
    explicit ImagePanel(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *ev) override;
    /** @brief 탭을 벗어나도 렌즈가 계속 돌지 않게 반드시 세운다 */
    void hideEvent(QHideEvent *ev) override;

private:
    void fetch_all();

    /**
     * @brief 연속 줌 시작/정지 — ZoomContinuous = In · Out · Stop
     *
     * ⚠ **Stop 은 무슨 일이 있어도 나가야 한다.** 안 보내면 렌즈가 텔레
     * 끝단(53°)까지 밀려서 박힌다. 그래서 정지 경로가 셋이다:
     * 버튼 뗌 · 안전 타이머(m_zoom_guard) · 패널 숨김/파괴.
     */
    void zoom_continuous(int channel, const QString &direction);

    /** @brief 줌을 세우고(필요하면) 초점을 다시 잡는다 */
    void stop_zoom(bool refocus);

    /** @brief Mode=SimpleFocus 한 번 — 줌 뒤에는 초점이 틀어진다 */
    void run_simple_focus(int channel, bool quiet);

    /**
     * @brief 4채널 줌 격자 (미리보기 + 채널별 버튼)
     *
     * 렌즈가 채널마다 따로라 "채널을 고르고 → 줌"은 한 화면에서 넷을 맞출 때
     * 매번 콤보를 오가게 만든다. 보이는 타일 밑에 그 채널의 버튼을 두면
     * 지금 무엇을 움직이는지가 화면에 그대로 있다.
     */
    QWidget *build_zoom_grid();

    /** @brief 미리보기 스트림 시작/정지 — 이 탭이 보이는 동안만 연다 */
    void start_previews();
    void stop_previews();
    /** @brief 한 서브메뉴 view에서 키 하나를 찾아 콤보에 반영 */
    void fetch_value(const QString &submenu, const QString &key, QComboBox *into);
    void send_set(const QString &submenu, const QString &key,
                  const QString &value);
    void show_result(const QString &text, bool ok);

    QNetworkAccessManager *m_net = nullptr;
    QComboBox *m_ch = nullptr;
    QComboBox *m_comp = nullptr;   ///< 역광 보정
    QComboBox *m_wb = nullptr;     ///< 화이트밸런스
    QComboBox *m_ir = nullptr;     ///< IR LED
    QLabel *m_result = nullptr;
    bool m_loading = false;        ///< 조회 반영 중 시그널 무시

    // ---- 줌 (연속) ----
    /// 안전 타이머 — 버튼 뗌 이벤트를 놓쳐도(창 전환·포커스 상실) 여기서 선다
    QTimer *m_zoom_guard = nullptr;
    /// 지금 렌즈가 도는 채널 (-1 = 없음). 마우스로는 한 번에 버튼 하나만
    /// 누를 수 있으므로 채널 하나만 기억하면 된다.
    int m_zoom_ch = -1;

    // ---- 4채널 미리보기 ----
    QVector<class ChannelView *> m_previews;
    /// 줌 그리드 칸 (벽 배치가 바뀌면 이 위젯들을 다른 칸으로 옮긴다)
    QVector<class QWidget *> m_zoom_cells;
    bool m_previews_live = false;   ///< 스트림이 열려 있나 (중복 재생 방지)
};
