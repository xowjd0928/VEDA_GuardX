#pragma once

#include "video_backend.h"
#include "frame_queue.h"

#include <QHash>
#include <QMutex>
#include <QUrl>

#include <atomic>
#include <memory>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

class QTimer;
class RhiVideoWidget;

/**
 * @brief GStreamer 저지연 경로 — 이론적 하한 목표
 *
 * 지연이 생기는 모든 단계를 직접 통제한다:
 *  - rtspsrc latency=0        : 지터버퍼 제거 (기본값 2000ms!)
 *  - appsink sync=false       : 재생 클럭 대기 없이 도착 즉시 표시
 *  - max-buffers=1 drop=true  : 최신 프레임만 — 지연 누적 원천 차단
 *  - NV12 그대로 GPU 업로드   : CPU 색변환/스케일 제거 (RhiVideoWidget)
 *
 * 견고성은 세 겹으로 회수한다:
 *  - 버스 감시     : ERROR/EOS 즉시 감지 (연결 거부·카메라 재부팅)
 *  - 무프레임 워치독: 조용히 끊긴 경우
 *  - 지연 누적 감시 : TCP에서 밀림이 쌓이는 경우
 */
class GStreamerBackend : public VideoBackend
{
    Q_OBJECT

public:
    explicit GStreamerBackend(int channel, QObject *parent = nullptr);
    ~GStreamerBackend() override;

    QWidget *create_widget(QWidget *parent) override;
    void play(const QUrl &url) override;
    void stop() override;
    qint64 current_frame_pts_ms() const override;
    qint64 glass_latency_ms() const override { return m_glass_latency_ms.load(); }
    QString status_text() const override;

private:
    static GstFlowReturn s_new_sample(GstAppSink *sink, gpointer user);
    static void s_deep_element_added(GstBin *bin, GstBin *sub_bin,
                                     GstElement *element, gpointer user);

    /**
     * @brief 파이프라인 버스를 주기적으로 비운다 (ERROR/EOS → 재접속).
     *
     * `gst_bus_add_watch` 는 GLib 메인루프가 돌아야 콜백이 뜬다. Qt 는
     * Windows 에서 GLib 루프를 돌리지 않으므로(`QT_FEATURE_glib=OFF`)
     * 워치를 걸면 **콜백이 한 번도 안 불린다**. 폴링은 메인루프와 무관하다.
     */
    void poll_bus();

    /** @brief 디코더 sink pad 프로브 — 버퍼가 디코더에 들어간 시각을 기록 */
    static GstPadProbeReturn s_decoder_in(GstPad *pad, GstPadProbeInfo *info,
                                          gpointer user);
    /** @brief 디코더 원소에 프로브 부착 (명시 디코더 / decodebin 양쪽) */
    void attach_decoder_probe(GstElement *decoder);

    void start_pipeline();
    void teardown();
    void schedule_reconnect(const QString &reason);
    void set_status(const QString &text);

    int m_channel = -1;

    GstElement *m_pipeline = nullptr;
    QTimer *m_bus_poll = nullptr;
    RhiVideoWidget *m_widget = nullptr;          ///< 소유권은 부모 위젯
    std::shared_ptr<FrameQueue> m_queue;
    QTimer *m_watchdog = nullptr;
    QTimer *m_reconnect = nullptr;
    QUrl m_url;

    std::atomic<qint64> m_pts_ms{-1};
    std::atomic<qint64> m_last_frame_ms{0};
    std::atomic<bool> m_update_pending{false};
    std::atomic<qint64> m_glass_latency_ms{-1};
    std::atomic<qint64> m_last_latency_log_ms{0};
    std::atomic<qint64> m_last_relay_warn_ms{0};  ///< 중계기 의심 경고 억제

    /**
     * @brief 디코더 입구 시각 (PTS -> unix ms)
     *
     * 디코드 시간 = appsink 도착 − 여기 기록된 값. PTS를 키로 쓰는 이유는
     * 프레임이 디코더 안에서 재정렬·큐잉될 수 있어 순서로는 짝을 못 짓기
     * 때문이다. 프로브(디코더 스레드)와 appsink 콜백이 다른 스레드일 수
     * 있어 뮤텍스로 보호한다.
     */
    QMutex m_dec_mutex;
    QHash<qint64, qint64> m_dec_in_ms;
    bool m_probe_attached = false;

    // 부드러움 측정 (프레임 도착 간격) — s_new_sample(스트리밍 스레드)에서만 접근
    qint64 m_sm_window_start_ms = 0;
    qint64 m_sm_prev_ms = 0;
    qint64 m_sm_max_gap = 0;
    int m_sm_count = 0;
    double m_sm_sum = 0.0;
    double m_sm_sum_sq = 0.0;

    QString m_status;
    int m_retry_count = 0;                       ///< 지수 백오프 단계
    bool m_had_frame = false;                    ///< 첫 프레임 수신 여부
};
