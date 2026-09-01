#pragma once

#include <QWidget>

#include "track_history.h"

class QImage;
class FloorMiniMap;
class QLabel;
class QPushButton;
class QVBoxLayout;

/**
 * @brief LIVE 화면 우측 TRACKING · 동선 패널 (400px)
 *
 * 원래 별도 TRACK 화면이었지만 목업대로 LIVE 안으로 들어왔다 — 감시자가
 * 영상과 동선을 같은 화면에서 봐야 하기 때문이다. 내비의 TRACK 항목은
 * 그래서 사라졌다.
 *
 * 구성 (위→아래):
 *   1. 헤더 (TRACKING · 동선 + LIVE 점)
 *   2. 선택 대상 스트립 (id · dwell · now)
 *   3. FLOOR MAP 2×2 — 채널 칸 위에 동선 폴리라인
 *   4. Active 목록 — 탭하면 그 대상을 해당 카메라에서 강조 (global_id 대기 중
 *      임시 조작. 지금은 object_id가 채널 안에서만 고유해서 패널이 대상을
 *      고를 다른 방법이 없다)
 *   5. Path Log — 채널 전환 이력
 *
 * 여러 대상을 동시에 추적한다 (08-19: Ctrl 조합 폐지 — 클릭 하나로 토글):
 *   - 클릭        = 추적에 더하기 (기존 선택은 유지)
 *   - 다시 클릭   = 해제 (박스 사라짐)
 * 영상 타일의 우클릭도 같은 규칙이다.
 * 시작 직후는 아무것도 선택되지 않은 상태다 — 자동 선택하지 않는다.
 *
 * 대상마다 Theme::track_color 색이 붙고 그 색이 동선 선과 영상 타일 박스에
 * 함께 쓰인다.
 *
 * 데이터는 전부 TrackHistory에서 오고, 이 위젯은 DB/네트워크를 모른다.
 */
class TrackingPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TrackingPanel(QWidget *parent = nullptr);

    /**
     * @brief 채널 타일에서 박스를 우클릭했을 때 — 그 대상을 추적에 토글
     *
     * 이미 추적 중이면 해제, 아니면 기존 선택에 더한다 (목록 클릭과 동일 규칙).
     */
    void select(int channel, int object_id);

    /** @brief 동시 추적 상한 — Theme::track_color 팔레트 크기 */
    static int max_targets();

    /** @brief Sets a generated Sobel edge-map background behind the trajectory map. */
    void set_edge_map(const QImage &image);

    /**
     * @brief 대상의 구분색 (선택 밖이면 무효 QColor)
     *
     * 색 정책은 패널 소유다. 영상 타일도 이걸 물어서 같은 색을 쓴다 —
     * 두 곳에서 따로 계산하면 언젠가 어긋난다.
     */
    QColor color_of(const TrackId &id) const;

    /**
     * @brief 추적 지우기 — 선택·선·로그를 한 번에 (운영자 버튼)
     *
     * 셋이 한 동작인 이유: 화면에 남는 것이 셋(타일 박스·FLOOR MAP 선·
     * Path Log)인데 원천은 둘(선택 목록, TrackHistory)이라, 하나만 비우면
     * "지웠는데 남아 있다"가 된다.
     */
    void clear_all();

signals:
    /**
     * @brief 추적 대상이 바뀜 — 영상 타일 강조를 갱신하라
     *
     * 첫 원소가 주 대상(매트릭스 송출 기준)이다.
     */
    void selection_changed(const QVector<TrackId> &targets);

    /**
     * @brief 현장 LED 매트릭스 송출 대상이 바뀜
     *
     * on=true면 target을 찍으라는 뜻이고, false면 끄라는 뜻이다(이때
     * target은 무효일 수 있다). 추적 중에 주 대상이 바뀌면 STOP 없이 새
     * 대상으로 한 번 더 온다 — 중간에 끄면 LED에서 점이 사라졌다 다시
     * 나타난다.
     *
     * 이 위젯은 네트워크를 모른다(클래스 주석 참조). 실제 발행과 화면
     * 채널 -> DB 채널 변환은 LiveViewer가 맡는다.
     */
    void matrix_output_changed(const TrackId &target, bool on);

private:
    QWidget *build_selected_strip();
    QWidget *build_map_section();
    QWidget *build_active_list();
    QWidget *build_path_log();

    /** @brief TrackHistory 변경 반영 (250ms로 묶여 들어온다) */
    void refresh();
    void refresh_strip();
    void refresh_active();
    void refresh_log();

    /** @brief FLOOR MAP 헤더 — 개략도인지 실측 도면(방 크기)인지 표기 */
    void refresh_map_caption();

    /** @brief 이력에서 사라진 대상을 선택에서 뺀다 (빈 선택은 그대로 둔다) */
    void prune_selection();

    /**
     * @brief 클릭 처리 — 이미 추적 중이면 해제, 아니면 선택에 더한다
     */
    void toggle(const TrackId &id);

    /** @brief 비어 있는 가장 낮은 색 슬롯 (없으면 -1) */
    int free_slot() const;

    /** @brief 선택된 대상들만 뽑아낸다 (시그널·순회용) */
    QVector<TrackId> selected_ids() const;

    /**
     * @brief 매트릭스 송출 상태를 현재 선택과 맞춘다
     *
     * 토글을 누를 때와 선택이 바뀔 때 모두 여기를 지난다. 상태 전이가 한
     * 함수에만 있어야 "버튼은 켜져 있는데 아무도 안 찍히는" 어긋남이
     * 생기지 않는다.
     */
    void sync_matrix_output();

    /**
     * @brief 추적 중인 대상 하나 — 대상과 그 색 슬롯
     *
     * 색을 목록 인덱스로 계산하면 중간의 대상을 해제할 때 뒤쪽 대상들의
     * 색이 밀린다 (A·B·C에서 B를 빼면 C가 보라 -> 연두). 추적 중인 사람의
     * 색이 변하면 색으로 사람을 알아보는 의미가 없어지므로, 슬롯을 대상에
     * 붙여 고정한다. 해제하면 그 슬롯이 비고 다음 추가가 그 자리를 쓴다.
     */
    struct Target {
        TrackId id;
        int slot = 0;
    };

    /// 선택된 대상들. 순서는 선택 순서(첫 원소 = 주 대상)이고, 색은 slot이다.
    QVector<Target> m_sel;

    FloorMiniMap *m_map = nullptr;
    QLabel *m_map_mode = nullptr;   ///< FLOOR MAP 헤더 우측 (개략도 / 방 크기)
    QLabel *m_sel_id = nullptr;
    QLabel *m_sel_pill = nullptr;
    QLabel *m_sel_dwell = nullptr;
    QLabel *m_sel_now = nullptr;
    QPushButton *m_sel_matrix = nullptr;
    QLabel *m_live_dot = nullptr;
    QLabel *m_active_caption = nullptr;

    QWidget *m_active_host = nullptr;
    QVBoxLayout *m_active_lay = nullptr;
    QWidget *m_log_host = nullptr;
    QVBoxLayout *m_log_lay = nullptr;

    bool m_blink_on = true;

    /// 사용자가 토글을 켜 두었는가 (대상이 있든 없든 버튼의 의사)
    bool m_matrix_on = false;
    /// 실제로 송출을 지시한 대상. m_matrix_on이면서 유효할 때만 의미 있다
    TrackId m_matrix_id;
};
