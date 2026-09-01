#pragma once

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QSet>
#include <QVector>
#include <QWidget>

#include "box_source.h"

class QPainter;
class QTimer;
class QUrl;
class VideoBackend;
class ChannelView;

/**
 * @brief 화면에 그려지는 박스 하나
 */
struct AnimatedBox {
    int object_id;
    QRectF current;  ///< 지금 그리는 위치 (위젯 좌표)
    QRectF target;   ///< (미사용 — 구조 유지용)
    QDateTime ts;    ///< 이 감지가 촬영된 시각
    int category = 1; ///< v15: 1=Human, 2=Face, 3=Head
    int parent_id = 0; ///< v15: Face/Head가 속한 사람 (Human은 0)
};

/**
 * @brief 메타 프레임 하나 — 웹 UI metaManager.overlayList 항목의 등가물
 *
 * 카메라가 프레임 단위로 발행한 박스 묶음을 그대로 담는다 (2026-08-05,
 * 웹 UI 동형 구조 전환). 궤적 재생·보간·외삽은 없다.
 *
 * 그리기는 **가장 최근 항목**을 쓴다 (2026-08-24 기본값 — 도착 즉시).
 * 영상과의 정합은 여기서 계산하지 않고 **영상을 늦춰서** 맞춘다
 * (set_playback_delay → delayq). 옛 방식(표시 중 프레임의 카메라 UTC에
 * ±250ms 최근접 매칭)은 QSettings `box_match_to_video=1` 로 되돌릴 수 있다.
 */
struct OverlayFrame {
    qint64 utc_ms = -1;            ///< tt:Frame UtcTime (카메라 시계, ms)
    qint64 arrival_ms = 0;         ///< PC 도착 시각 — 오래된 항목 청소용
    QVector<DetectionBox> boxes;   ///< 그 프레임의 박스들 (카메라 좌표)
};

/**
 * @brief 얼굴 가림 영역 하나 (뷰 좌표)
 *
 * 같은 사람의 Face(2)·Head(3) 박스를 합친 사각형. object_id는 모자이크 톤
 * 해시의 시드 — 같은 사람은 프레임이 바뀌어도 같은 무늬라 눈이 편하다.
 */
struct BlurRegion {
    int object_id = 0;
    QRectF rect;
};

/**
 * @brief 영상 위에 박스/상태를 그리는 투명 오버레이
 *
 * 영상 위젯(QRhiWidget)은 GPU가 직접 그리므로 그 위에 QPainter로 덧그릴 수
 * 없다. 대신 투명한 형제 위젯을 위에 올려 합성한다. 마우스는 통과시켜
 * ChannelView가 그대로 클릭을 받는다.
 */
class BoxOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit BoxOverlay(ChannelView *view);

protected:
    void paintEvent(QPaintEvent *ev) override;

private:
    ChannelView *m_view;
};

/**
 * @brief 단일 채널 뷰 — 영상 + 감지 박스 오버레이를 캡슐화
 *
 * 구성: [ChannelView] -> 자식으로 영상 위젯(백엔드 제공) + BoxOverlay
 */
class ChannelView : public QWidget
{
    Q_OBJECT

public:
    /**
     * @param channel    화면/RTSP 채널 번호
     * @param db_channel DB detections.channel 값 (RTSP 채널과 다를 수 있음)
     */
    explicit ChannelView(int channel, int db_channel, QWidget *parent = nullptr);

    /** @brief 재생할 RTSP 주소 지정 후 재생 시작 */
    void play_stream(const QUrl &url);

    /**
     * @brief 세션 종료 — 파이프라인을 세운다
     *
     * LIVE 벽은 이걸 부르지 않는다(항상 켜져 있어야 경보를 놓치지 않는다).
     * CAMERA ▸ Image 의 줌 미리보기처럼 **보이는 동안만** 스트림을 여는
     * 화면을 위한 것이다 — 안 부르면 그 탭을 한 번 열었다는 이유로 디코드
     * 세션 4개가 앱이 끝날 때까지 남는다.
     */
    void stop_stream();

    int channel() const { return m_channel; }

    /**
     * @brief 영상 표시 지연 (ms) — 박스↔영상 정합의 유일한 손잡이
     *
     * 박스는 **받는 대로** 그린다(도착 즉시). 그래서 박스가 사람보다 앞서
     * 보이면 영상을 그만큼 늦춰 맞춘다 — 값은 백엔드의 delayq 로 간다.
     * HUD 의 "delay Nms" 가 이 값이다.
     */
    void set_playback_delay(int ms);

    /** @brief 오버레이가 그릴 내용 (BoxOverlay에서 사용) */
    const QVector<AnimatedBox> &boxes() const { return m_anim_boxes; }

    /**
     * @brief 추적 중인 대상의 구분색 (추적 대상이 아니면 무효 QColor)
     *
     * 색은 동선 패널이 정한다 — 패널의 선과 여기 박스가 같은 색이어야
     * "저 선이 저 사람"이 읽힌다 (Theme::track_color).
     */
    QColor selection_color(int object_id) const
    {
        return m_selected.value(object_id);
    }
    int playback_delay_ms() const { return m_playback_delay_ms; }
    QString status_text() const;
    qint64 glass_latency_ms() const;

    // ---- 타일 크롬 (UI_REDESIGN_SPEC §3.3) ----
    /**
     * @brief 현재 표시 중인 인원 수 = OCC 배지의 n
     *
     * v15부터 Face/Head 박스가 별도 객체로 함께 오므로 Human만 센다 —
     * 전부 세면 점유율이 2~3배 증폭된다 (DATA_FLOW_MAP.md v15 §1).
     */
    int occupancy() const
    {
        int n = 0;
        for (const AnimatedBox &box : m_anim_boxes)
            n += (box.category == 1);
        return n;
    }

    /**
     * @brief 모든 감지 박스 표시 토글 (기본 꺼짐 — 추적 대상만)
     *
     * 켜면 추적하지 않는 사람에게도 박스가 붙는다. 모양은 다르다 —
     * 추적 대상은 굵은 구분색 + P-태그, 나머지는 가는 중립선에 태그 없음.
     */
    void set_show_all_boxes(bool on);
    bool show_all_boxes() const { return m_show_all_boxes; }

    /** @brief 얼굴 블러 토글 — category 2(Face)/3(Head) 영역을 가린다 */
    void set_face_blur(bool on);
    bool face_blur() const { return m_face_blur; }

    /** @brief 백엔드가 실픽셀 모자이크를 그리는가 (오버레이 마스크 생략용) */
    bool pixel_mosaic() const;

    /** @brief 유입 속도 (명/분) — LiveViewer가 1초 주기로 계산해 넣는다 */
    void set_flow_per_min(double f) { m_flow_per_min = f; }
    double flow_per_min() const { return m_flow_per_min; }

    /** @brief 우상단 스트림 캡션 (예: "Profile4 · H.264") */
    void set_stream_caption(const QString &c) { m_stream_caption = c; }
    QString stream_caption() const { return m_stream_caption; }

    /** @brief LIVE 점 깜박임 위상 (BoxOverlay에서 사용) */
    bool blink_on() const { return m_blink_on; }

    /**
     * @brief 타일 크롬 전체(테두리·박스·블러·배지·HUD)를 그린다
     *
     * 두 소비자가 공유한다: BoxOverlay(투명 위젯, appsink 경로)와
     * update_overlay의 QImage 렌더(direct 경로 — sink가 GPU에서 영상과 합성).
     * canvas = 그릴 좌표계의 크기 (박스 좌표는 뷰 크기 기준이므로 direct
     * 경로는 painter에 뷰→캔버스 스케일을 걸고 뷰 크기를 넘긴다).
     */
    void paint_chrome(QPainter &painter, const QSize &canvas) const;

    // ---- 혼잡 경보 (AlertFeed — RPi B task_alert 판정) ----
    /** @brief 0 = 평상 · 1 = warn · 2 = critical */
    int alert_severity() const { return m_alert_severity; }
    /** @brief 타일에 띄울 짧은 경보 문구 ("Critical 8/10" 등, 없으면 빈 값) */
    QString alert_text() const { return m_alert_text; }

    /**
     * @brief 드롭 후보 하이라이트 — 끌던 타일이 이 위에 놓이려 할 때 켠다
     *
     * refresh_alert와 같은 규칙으로 즉시 다시 그린다. direct 경로는 크롬이
     * sink GPU 합성(QImage)이라 영상 위에서도 보인다 — Qt 형제 위젯으로는
     * airspace에 가려 못 그리는 자리다.
     */
    void set_drop_hint(bool on);

public slots:
    /**
     * @brief 추적 대상 지정 — object_id -> 구분색
     *
     * 여러 대상을 동시에 추적하므로 단일 id가 아니라 맵이다. 이 채널에
     * 속한 대상만 담아 넘긴다 (object_id는 채널 안에서만 고유하다).
     */
    void set_selected_objects(const QHash<int, QColor> &by_object_id)
    {
        if (m_selected == by_object_id)
            return;
        m_selected = by_object_id;
        update_overlay();
    }

signals:
    /**
     * @brief 박스 안이 우클릭됨 — 추적 대상 토글 (cam_rect: 카메라 원본 좌표)
     *
     * 우클릭마다 그 사람이 추적에 더해지거나(이미 추적 중이면) 빠진다 —
     * 여러 명을 우클릭만으로 쌓을 수 있다 (Ctrl 조합은 08-19에 폐지).
     */
    void object_selected(int db_channel, int object_id, const QRectF &cam_rect);

    /** @brief 스트림 상태 문구 변경 — 이벤트 타임라인 배선용 */
    void stream_event(int channel, const QString &status);

    /**
     * @brief 백엔드가 세션을 (재)요청했다 — LiveViewer가 키프레임을 강제한다
     *
     * 근거·시점은 VideoBackend::session_started 참고.
     */
    void session_started(int channel);

    /**
     * @brief 타일이 좌클릭 드래그로 끌리는 중 (배치 이동, 08-20)
     *
     * 임계 거리(startDragDistance)를 넘은 뒤부터 마우스 이동마다 온다.
     * 고스트 이동·드롭 후보 판정은 LiveViewer가 한다 — 다른 타일을 아는
     * 것은 거기뿐이다.
     */
    void tile_drag_moved(int channel, const QPoint &global_pos);

    /** @brief 타일 드래그가 끝났다 (버튼 놓음) — 드롭 판정도 LiveViewer가 */
    void tile_drag_finished(int channel, const QPoint &global_pos);

protected:
    void resizeEvent(QResizeEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void showEvent(QShowEvent *ev) override;
    void paintEvent(QPaintEvent *ev) override;

private:
    /**
     * @brief 영상이 흐르는지에 따라 네이티브 창을 보였다/숨겼다 한다
     *
     * direct 경로 전용. 네이티브 창은 sink 가 버퍼를 받을 때만 그려지고 Qt 는
     * 그 영역을 칠하지 않으므로, 프레임이 없는 동안 창을 띄워두면 직전 화면
     * (다른 탭 등)이 그대로 남는다. 숨기면 ChannelView 의 paintEvent 가
     * 배경과 크롬(상태 문구 포함)을 그린다 — 그래야 "재접속 중"이 보인다.
     */
    void update_video_visibility();

    void on_boxes_updated(const QVector<DetectionBox> &boxes,
                          qint64 frame_utc_ms);
    void update_overlay();

    /** @brief AlertFeed 상태를 이 채널 기준으로 읽어온다 */
    void refresh_alert();

    /** @brief 마지막으로 그린 내용과 달라졌는지 (같으면 재도색 생략) */
    bool boxes_changed();

    /** @brief 박스 좌표(카메라 원본) -> 현재 위젯 좌표로 변환 */
    QRectF to_view_rect(const DetectionBox &box) const;

    /** @brief pos 아래의 사람(Human) 박스 — 없으면 nullptr (겹치면 최소 박스) */
    const AnimatedBox *human_box_at(const QPointF &pos) const;

    // 카메라 원본 해상도 (rect 좌표의 기준) — heatmap과 동일
    static const int FRAME_W = 2592;
    static const int FRAME_H = 1520;

    int m_channel = -1;
    int m_db_channel = -1;   ///< DB detections.channel 값

    VideoBackend *m_backend = nullptr;             ///< 영상 표시 담당
    QWidget *m_video = nullptr;                    ///< 백엔드가 만든 영상 위젯
    BoxOverlay *m_overlay = nullptr;
    BoxSource *m_box_source = nullptr;
    QTimer *m_anim_timer = nullptr;

    QVector<AnimatedBox> m_anim_boxes;             ///< 이번 프레임에 그릴 박스들
    QVector<AnimatedBox> m_drawn_boxes;            ///< 마지막으로 그린 박스들
    QString m_drawn_status;                        ///< 마지막으로 그린 상태 문구
    QHash<int, QColor> m_drawn_selected;           ///< 마지막으로 그린 선택 대상
    QVector<OverlayFrame> m_overlay_list;          ///< 메타 프레임 버퍼 (웹 UI 동형)
    QVector<DetectionBox> m_fallback_boxes;        ///< HTTP 스냅샷 (ONVIF 부재 시)
    QVector<QRectF> m_last_masks;                  ///< 백엔드에 넘긴 모자이크 영역

    // ---- 얼굴 가림 (Face·Head 박스를 사람 단위로 합쳐 가린다) ----
    QVector<BlurRegion> m_blur_rects;              ///< 이번 틱에 가릴 영역
    QVector<BlurRegion> m_drawn_blur;              ///< 마지막으로 그린 영역

    QHash<int, QColor> m_selected;                 ///< 추적 중인 object_id -> 구분색
    int m_playback_delay_ms = 500;                 ///< 재생 지연 (+/- 키로 보정)

    // ---- 타일 크롬 상태 ----
    bool m_face_blur = true;                       ///< Face Blur 토글 (기본 켬)
    bool m_show_all_boxes = false;                 ///< All Boxes 토글 (기본 끔)
    bool m_blink_on = true;                        ///< LIVE 점 깜박임 위상
    double m_flow_per_min = 0.0;                   ///< 유입 속도 (명/분)
    QString m_stream_caption;                      ///< 우상단 캡션
    QTimer *m_blink_timer = nullptr;

    int m_alert_severity = 0;                      ///< 0 none · 1 warn · 2 critical
    QString m_alert_text;                          ///< 타일 칩 문구

    // ---- 타일 드래그 (배치 이동, 08-20) ----
    QPoint m_press_pos{-1, -1};  ///< 좌클릭 눌린 곳 (x<0 = 없음)
    bool m_dragging = false;     ///< 임계 거리를 넘어 드래그 확정
    bool m_drop_hint = false;    ///< 드롭 후보 하이라이트 (paint_chrome)

    bool m_gpu_overlay = false;  ///< direct 경로 — 크롬을 sink 합성으로 보냄
    QImage m_overlay_img;        ///< 합성 캔버스 재사용 (틱당 재할당 방지)

    // ---- 매칭 진단 (채널 비대칭 "버벅" 추적, 2026-08-05 — 5s 로그) ----
    int m_diag_matched = 0;      ///< ±250ms 매칭 성공 틱
    int m_diag_hold = 0;         ///< 짧은 공백을 직전 그림으로 버틴 틱
    int m_diag_repaint = 0;      ///< 실제 재도색 횟수 (오버레이 CPU 비용 지표)
    qint64 m_last_match_ms = 0;  ///< 마지막 매칭 성공 시각 (공백 유지 판정)
    int m_diag_none = 0;         ///< 목록은 있는데 매칭 실패(박스 공백) 틱
    int m_diag_latest = 0;       ///< 영상 시각 미상 → 최신 프레임 모드 틱
    int m_diag_fallback = 0;     ///< HTTP 폴백 틱
    int m_diag_eback = 0;        ///< 영상 시각(e) 역행 횟수
    qint64 m_diag_prev_e = -1;
    qint64 m_diag_log_ms = 0;
    QVector<int> m_diag_offsets; ///< |e − 선택 프레임 utc| 표본 (ms)
};
