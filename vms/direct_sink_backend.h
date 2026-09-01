#pragma once

#include "video_backend.h"

#include <QMutex>
#include <QSize>
#include <QUrl>

#include <atomic>
#include <limits>

#include <gst/gst.h>
#include <gst/video/video-overlay-composition.h>

class QTimer;

/**
 * @brief Direct 렌더 경로 — appsink 없이 d3d11videosink가 Qt 위젯에 직접 그린다
 *
 * appsink 경로(GStreamerBackend)와의 차이:
 *  - 디코더 출력이 우리 손(FrameQueue/QRhiWidget)을 거치지 않고 sink가
 *    위젯의 네이티브 HWND에 바로 그린다 → mailbox·업로드 단계 소거 (~10ms)
 *  - 감지 박스·블러·타일 크롬은 QWidget 오버레이가 아니라
 *    **GstVideoOverlayComposition 메타**로 넣는다 — sink가 GPU에서 영상과
 *    합성하므로 네이티브 창 위 투명 위젯(airspace) 문제가 원천적으로 없다
 *  - 대가: ChannelSync 정렬 불가(표시 시점을 sink가 소유), 구간 계측은
 *    sink 도착까지만 (2026-08-04 direct sink 실험 보고서 참조)
 *
 * 선택: QSettings video_backend="direct" (기본은 "gstreamer"=appsink 경로 —
 * 못 박지 않기 원칙, 런마다 교체). 권장 디코더는 d3d11-flex (sink와 같은
 * D3D11 API라 zero-copy 협상이 된다; d3d12 디코더는 시스템 메모리 경유).
 */
class DirectSinkBackend : public VideoBackend
{
    Q_OBJECT

public:
    explicit DirectSinkBackend(int channel, QObject *parent = nullptr);
    ~DirectSinkBackend() override;

    QWidget *create_widget(QWidget *parent) override;
    void play(const QUrl &url) override;
    void stop() override;
    qint64 current_frame_pts_ms() const override { return m_pts_ms.load(); }
    qint64 glass_latency_ms() const override { return m_glass_latency_ms.load(); }
    qint64 current_frame_utc_ms() const override { return m_frame_utc_ms.load(); }
    QString status_text() const override;

    void set_video_delay_ms(int ms) override;

    bool gpu_composition() const override { return true; }
    bool has_frame() const override { return m_had_frame.load(); }
    void expose() override;
    void set_overlay_image(const QImage &image) override;
    QSize video_size() const override
    {
        return QSize(m_video_w.load(), m_video_h.load());
    }

private:
    /** @brief sink 입구 프로브 — 오버레이 메타 부착 + 도착 계측 + 캡스 파악 */
    static GstPadProbeReturn s_sink_in(GstPad *pad, GstPadProbeInfo *info,
                                       gpointer user);

    /**
     * @brief 파이프라인 버스를 주기적으로 비운다 (ERROR/EOS → 재접속).
     *
     * `gst_bus_add_watch` 는 GLib 메인루프가 돌아야 콜백이 뜬다. Qt 는
     * Windows 에서 GLib 루프를 돌리지 않으므로(`QT_FEATURE_glib=OFF`)
     * 워치를 걸면 **콜백이 한 번도 안 불린다** — 메시지만 버스에 쌓이고
     * 회복은 12초 워치독에만 의존하게 된다. 폴링은 메인루프 종류와 무관하다.
     */
    void poll_bus();

    void start_pipeline();
    void teardown();
    void schedule_reconnect(const QString &reason);
    void set_status(const QString &text);

    int m_channel = -1;

    GstElement *m_pipeline = nullptr;
    QTimer *m_bus_poll = nullptr;
    QWidget *m_widget = nullptr;   ///< 네이티브 HWND 위젯 (소유권은 부모)
    QTimer *m_watchdog = nullptr;
    QTimer *m_reconnect = nullptr;
    QUrl m_url;

    // 오버레이 합성 — set_overlay_image(UI 스레드)가 만들고
    // s_sink_in(스트리밍 스레드)이 버퍼마다 붙인다
    QMutex m_comp_mutex;
    GstVideoOverlayComposition *m_comp = nullptr;

    std::atomic<int> m_video_w{0};
    std::atomic<int> m_video_h{0};

    /**
     * @brief 표시 지연 (ms) — `delayq` 의 min-threshold-time 으로 실현된다
     *
     * 파이프라인 재구축을 넘어 살아남아야 해서 여기 보관한다(+/- 로 맞춰둔 값이
     * 재접속마다 0으로 돌아가면 안 된다). 런타임 변경은 살아있는 원소에 바로
     * 반영하고, 파이프라인이 없으면 다음 start_pipeline 이 집어간다.
     */
    std::atomic<int> m_video_delay_ms{0};
    /// "아직 한 번도 관측 안 됨" 센티넬. 원시 지연은 시계차 때문에 음수일 수
    /// 있어 -1을 센티넬로 쓸 수 없다.
    static constexpr qint64 FLOOR_UNSET = std::numeric_limits<qint64>::max();

    std::atomic<qint64> m_pts_ms{-1};
    std::atomic<qint64> m_last_frame_ms{0};
    /// 촬영→sink 도착 **원시값**. 시계차가 섞여 있어 절대 지연이 아니다
    /// (HUD의 "video Nms"가 이 값). 임계 판정은 m_glass_floor_ms 대비로 한다.
    std::atomic<qint64> m_glass_latency_ms{-1};
    /// 이번 세션에서 관측된 원시값의 최솟값 = 시계차 추정. 지연 누적 가드의
    /// 기준선이라 세션마다 리셋한다 — 이전 세션 값을 물려받으면 새 세션이
    /// 시작하자마자 임계를 넘어 재기동 루프가 된다.
    std::atomic<qint64> m_glass_floor_ms{FLOOR_UNSET};
    std::atomic<qint64> m_frame_utc_ms{-1};  ///< 표시 프레임의 SR 카메라 UTC
    std::atomic<qint64> m_last_latency_log_ms{0};

    /**
     * @brief 부드러움 측정 (프레임 도착 간격) — appsink 경로에서 이식 (08-12)
     *
     * 지연이 낮아도 프레임이 몰려오거나 끊기면 눈에는 끊겨 보인다. 그리고
     * direct 경로엔 이 지표가 없어 **present 가 수신 스레드를 막고 있는지**를
     * 판정할 방법이 없었다 — 파이프라인에 queue 가 없어 depay·parse·decode·
     * present 가 한 스레드에 직렬로 붙어 있기 때문에, Present() 가 블록하면
     * 도착 간격이 톱니처럼 튄다. 그 신호가 여기 나온다.
     *
     * s_sink_in(스트리밍 스레드)에서만 접근한다. 리셋은 start_pipeline 이
     * 하는데, 그 시점엔 teardown() 의 NULL 전이가 이미 끝나 콜백이 끊긴 뒤라
     * 경합이 없다.
     */
    qint64 m_sm_window_start_ms = 0;
    qint64 m_sm_prev_ms = 0;
    qint64 m_sm_max_gap = 0;
    int m_sm_count = 0;
    double m_sm_sum = 0.0;
    double m_sm_sum_sq = 0.0;

    QString m_status;
    int m_retry_count = 0;
    /// 지연 누적이 연속으로 임계를 넘은 워치독 틱 수 (GUI 스레드 전용)
    int m_creep_ticks = 0;
    std::atomic<bool> m_had_frame{false};  ///< 첫 프레임 = 진짜 연결 성공
};
