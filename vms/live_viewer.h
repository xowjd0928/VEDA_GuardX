#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QVector>
#include <QWidget>

#include "track_history.h"

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class CameraTuner;
class ChannelView;
class OccupancyPanel;
class QGridLayout;
class QPushButton;
class TrackingPanel;
class WallLayout;

/**
 * @brief Live Monitoring 화면 (08-19 워크스페이스 배치)
 *
 * [좌 컨텍스트 패널 240px | 2×2 월 | 동선 패널 + 존 점유율].
 * (눈금자·이벤트 티커는 08-19 오후 사용자 요청으로 삭제.)
 * 고정 2×2 그리드(저해상도 서브스트림) —
 * 전체화면(FOCUS) 모드와 타일 클릭 확대는 08-19에 삭제했다.
 * 각 채널은 ChannelView가 캡슐화하며, 영상 위 감지 박스도 거기서 그린다.
 * 동선(TrackingPanel)은 원래 별도 TRACK 화면이었으나 영상과 동선을 같은
 * 화면에서 봐야 하므로 여기 우측 열에 산다. 점유율 패널은 그 아래 —
 * (DetectionFeed 감지 수 실배선, +5분 예측 틱은 실모델/유입 속도 근사).
 */
class LiveViewer : public QWidget
{
    Q_OBJECT

public:
    explicit LiveViewer(QWidget *parent = nullptr);

signals:
    /**
     * @brief 살아있는 채널 스트림 수 변화 (top_bar CAM pill용)
     *
     * "살아있다" = 마지막 상태 문구가 빈 것(재생 중). "연결 중…"·"무프레임"
     * 등 문구가 붙어 있으면 죽은 것으로 센다.
     */
    void stream_health_changed(int up, int total);

protected:
    void keyPressEvent(QKeyEvent *ev) override;

private:
    /**
     * @brief 채널·프로파일 조합의 RTSP URL 생성
     * @param ch      센서 채널 번호 (0..3)
     * @param profile 스트림 프로파일 이름
     */
    QUrl stream_url(int ch, const QString &profile) const;

    /** @brief 좌측 컨텍스트 패널 (240px): 도구(Edge Map·Face Blur) + 현장 트리 */
    QWidget *build_left_panel();

    /** @brief 트리 갱신 — 이름(zones)·스트림 생존 점 */
    void refresh_tree();

    /** @brief [좌 패널 | 월 | 동선+점유율] 한 행 (08-19 워크스페이스) */
    QWidget *build_stage();

    /**
     * @brief 추적 대상들을 각 채널 타일에 반영 (색은 동선 패널이 정한 것)
     *
     * 대상을 전 채널에 뿌리지 않는다. object_id는 채널 안에서만 고유해서
     * CH1의 3번을 고르면 CH2의 무관한 3번까지 강조된다. 채널을 넘어 같은
     * 사람을 묶는 것은 global_id의 일이고, 그것이 실리면 TrackHistory가
     * 알아서 같은 트랙으로 묶어주므로 여기 코드는 그대로 맞는다.
     */
    void apply_selection(const QVector<TrackId> &targets);

    /** @brief 박스 우클릭 시 추적 대상 토글 — 우클릭마다 추적에 더하거나 뺀다 */
    void on_object_selected(int db_channel, int object_id, const QRectF &cam_rect);

    // ---- 타일 드래그 앤 드롭 (배치 이동, 08-20) ----
    /** @brief 드래그 이동 — 고스트를 커서에 붙이고 드롭 후보를 하이라이트 */
    void on_tile_drag_moved(int ch, const QPoint &global_pos);

    /** @brief 드래그 종료 — 다른 타일 위에서 놓였으면 자리를 맞바꾼다 */
    void on_tile_drag_finished(int ch, const QPoint &global_pos);

    /** @brief global_pos 아래의 타일 채널 (타일 밖이면 -1) */
    int tile_at(const QPoint &global_pos) const;

    /** @brief 재생 시작 + 타일 캡션 갱신 */
    void play_channel(int ch, const QString &profile);

    /** @brief 스트림 상태 변화 -> 이벤트 로그 + 트리 점 갱신 */
    void on_stream_event(int ch, const QString &status);

    /**
     * @brief 세션 (재)시작 -> SUNAPI 로 키프레임 강제
     *
     * GOP 한 바퀴를 기다리던 검은 화면을 ~0.2초로 줄인다. 최초 연결·재접속
     * 복구·프로파일 전환이 전부 이 훅 하나를 지난다.
     * 채널당 최소 간격을 둔다 — 재접속 폭풍에서 카메라 제어 채널을 두들기면
     * 그 압박이 다시 세션 굶주림을 만든다 (08-04·08-05 실사고).
     */
    void on_session_started(int ch);

    /**
     * @brief 엣지맵 캡처 시작 — **카메라 스냅샷**을 채널마다 한 장씩 받는다
     *
     * ⚠ 화면(위젯)에서는 못 뜬다. `video_backend=direct` 는 d3d11videosink 가
     * 네이티브 창에 직접 그려서 디코딩된 픽셀이 **Qt 를 한 번도 거치지 않는다** —
     * `grab()` 도 `grabFramebuffer()` 도 검은 화면이거나 초기화 안 된 메모리를
     * 돌려줬다(08-13 실측). 그래서 출처를 카메라로 옮겼다: SUNAPI 정지영상은
     * 파이프라인을 건드리지 않아 지연(g2g)에 영향이 없고 해상도도 원본이다.
     *
     * 비동기다 — 응답이 다 모이면 2×2 로 합성해 소벨을 돌린다.
     */
    void capture_edge_map();

    /** @brief 스냅샷 한 장 도착 (실패면 그 채널은 검은 칸으로 남는다) */
    void on_snapshot(int channel, QNetworkReply *reply);

    /** @brief 응답이 다 모였을 때 — 합성 → 소벨 → FLOOR MAP 배경 */
    void finish_edge_map();

    /** @brief 1초 주기: 점유율/유입 속도 계산 -> 타일 배지 + 점유율 패널 */
    void update_occupancy_stats();

    /** @brief 엣지맵·이벤트 상세가 공유하는 스냅샷용 네트워크 (지연 생성) */
    QNetworkAccessManager *snapshot_net();

    /** @brief 이벤트 한 줄 — 티커 삭제(08-19 오후) 후에는 콘솔 로그로만 */
    void note_event(const QString &sev, const QString &src,
                    const QString &msg);

    // ── 엣지맵 스냅샷 ──
    QNetworkAccessManager *m_snap_net = nullptr;
    QPushButton *m_edge_btn = nullptr;      ///< 실패하면 되돌려야 한다
    QHash<int, QImage> m_snap_frames;       ///< 채널 -> 받은 정지영상
    int m_snap_pending = 0;                 ///< 아직 안 온 응답 수

    QWidget *m_grid_host = nullptr;
    QGridLayout *m_grid = nullptr;
    QList<ChannelView *> m_views;
    WallLayout *m_wall = nullptr;           ///< 고정 2×2 그리드 배치

    QLabel *m_site_lbl = nullptr;           ///< 좌 패널 현장 트리 헤더
    QLabel *m_tree_dot[4] = {};             ///< 트리 채널 행 — 스트림 생존 점
    QLabel *m_tree_name[4] = {};            ///< 트리 채널 행 — CH · 구역 이름

    OccupancyPanel *m_occ_panel = nullptr;
    TrackingPanel *m_track_panel = nullptr;
    QHash<int, QString> m_last_status;      ///< 채널별 마지막 상태 문구

    CameraTuner *m_tuner = nullptr;         ///< 기동 튜닝 + 키프레임 강제
    QHash<int, qint64> m_keyframe_ms;       ///< 채널별 마지막 키프레임 요청 시각

    // ---- 타일 드래그 앤 드롭 (배치 이동, 08-20) ----
    QLabel *m_drag_ghost = nullptr;         ///< 커서를 따라다니는 미니 타일 (톱레벨)
    int m_drop_ch = -1;                     ///< 현재 드롭 후보 채널 (-1 = 없음)

    /** @brief 유입 속도 계산용 점유율 샘플 (최근 60초) */
    struct OccSample { qint64 ms; int occ; };
    QVector<OccSample> m_occ_hist[4];
    QElapsedTimer m_uptime;

    /** @brief 실모델 +5분 예측 스냅샷 (PredictionFeed — 없거나 낡으면 외삽 폴백) */
    struct PredSnap { double p50 = -1.0; qint64 at_ms = -1; };
    PredSnap m_pred[4];

    /** @brief 전 채널 재생 지연 일괄 변경 + 저장 */
    void adjust_delay(int delta_ms);

    int m_delay_ms = 500;
};
