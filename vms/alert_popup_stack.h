#pragma once

#include <QObject>
#include <QVector>

class QWidget;

/**
 * @brief 경보 팝업이 서로 가리지 않게 세로로 쌓아 주는 배치자 (싱글턴)
 *
 * **왜 필요했나 (2026-08-10 리뷰).** 팝업마다 자기 y 오프셋을 상수로 들고
 * 있었다 — 혼잡 top+90, 화재 top+160, RPi top+230. `rpi_alert_popup.cpp` 의
 * 주석은 *"셋 다 같은 고정 오프셋 스태킹 방식(런타임 충돌 감지 아님)"* 이라고
 * 정확히 적어뒀는데, 실제 팝업은 **넷**이었다. 빠진 하나가 음향 팝업이고,
 * 그게 화재와 **완전히 같은 자리**(top+160)에 떠서 서로를 통째로 가렸다.
 * 미확인 경보가 안 보이는 상태다.
 *
 * 고정 오프셋 방식은 팝업이 늘 때마다 "남은 숫자"를 사람이 찾아야 하고, 그걸
 * 빠뜨리면 조용히 겹친다. 그래서 자리를 각자 정하지 않고, **지금 떠 있는 것들을
 * 보고** 여기서 한 번에 정한다. 새 팝업은 등록만 하면 자리가 생긴다.
 *
 * 등록만 하면 되는 이유: show/hide/resize 를 이벤트 필터로 직접 관찰하므로
 * 팝업 쪽에서 재배치를 호출하는 것을 잊어도 어긋나지 않는다.
 */
class AlertPopupStack : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 위에서부터의 순서. 08-10 이전의 화면상 순서를 그대로 보존한다
     *        (혼잡 → 화재 → 음향 → 장비) — 이번 수정의 목적은 겹침 제거지
     *        우선순위 재설계가 아니다.
     */
    enum Order {
        Congestion = 10,
        Fire = 20,
        Audio = 30,
        Device = 40,
    };

    static AlertPopupStack *instance();

    /** @brief 팝업 등록. 파괴되면 자동으로 빠진다 */
    void add(QWidget *popup, Order order);

    /** @brief 지금 보이는 팝업들을 위에서부터 다시 쌓는다 */
    void relayout();

    /**
     * @brief 게이트 — 잠그면 경보 팝업이 **뜨지 않는다** (로그인 전, 2026-08-11)
     *
     * 왜 팝업마다가 아니라 여기인가: 팝업이 자리를 각자 정하다 겹쳤던 것과
     * 같은 이유다. 넷에 각각 검사를 넣으면 다섯 번째 팝업에서 빠진다.
     * **등록이 이 리포의 팝업 계약**이므로 등록된 것은 전부 여기서 걸린다.
     *
     * 왜 그냥 숨기는 것으로 끝나지 않는가: 화재·버튼 팝업은 `확인`을 누르기
     * 전까지 안 닫히는 설계다(`m_fire_pending`). 로그인 안 한 사람이 그
     * `확인`을 눌러 버리면 **진짜 운영자는 그 경보를 영영 못 본다.**
     * 그래서 보류된 팝업의 내부 상태는 건드리지 않고 창만 숨겼다가, 잠금이
     * 풀리면 **그때까지도 떠 있어야 하는 것만** 다시 띄운다.
     * (그 사이 스스로 닫힌 것 — 혼잡 해제 등 — 은 다시 뜨지 않는다.)
     */
    void set_gated(bool gated);

    /**
     * @brief 팝업이 "지금 뜨겠다"고 말하는 자리 — `show(); raise();` 대신 부른다
     *
     * ⚠ 왜 `show()` 를 부른 뒤 숨기지 않는가: 팝업들은 `if (!isVisible())
     * show()` 형태라, 숨기면 **다음 갱신에 또 뜨려 한다.** 실측으로 12초에
     * 34번 — 창이 떴다 사라지기를 반복해 로그인 화면에 잔상이 남았다.
     * 뜬 것을 되돌리는 대신 **뜨기 전에** 판정한다.
     *
     * @return 실제로 띄웠으면 true. 잠겨 있으면 false (보류로 기록)
     */
    bool try_show(QWidget *popup);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit AlertPopupStack(QObject *parent = nullptr);

    struct Slot {
        QWidget *widget = nullptr;
        int order = 0;
        /// 게이트에 막혀 숨겨둔 것 — 잠금이 풀리면 이것만 다시 띄운다
        bool suppressed = false;
    };
    Slot *find(QObject *w);

    QVector<Slot> m_slots;   ///< order 오름차순 유지
    bool m_laying_out = false;
    bool m_gated = false;
    /// 우리가 부른 hide() 인가 — 팝업 자신이 닫은 것과 구별해야 한다.
    /// 구별하지 않으면 보류 표시가 그 자리에서 지워져 다시 띄우지 못한다.
    bool m_forcing_hide = false;

    /// 첫 팝업의 y (창 상단 기준) — 영상 타일을 가리지 않는 자리
    static const int FIRST_TOP = 90;
    static const int GAP = 10;
    /// 부모 창을 못 찾았을 때의 자리 (기존 각 팝업의 폴백과 같은 취지)
    static const int ORPHAN_X = 100;
};
