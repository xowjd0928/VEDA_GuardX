#include "direct_sink_backend.h"
#include "clip_recorder.h"

#include <QDateTime>
#include <QDebug>
#include <QImage>
#include <QSettings>
#include <QTimer>
#include <QWidget>

#include <gst/video/video-info.h>
#include <gst/video/videooverlay.h>

#include <cmath>

// GStreamerBackend와 동일한 회복 상수 — 두 경로의 견고성 정책은 같아야 한다
static const int WATCHDOG_INTERVAL_MS = 3000;
static const int NO_FRAME_TIMEOUT_MS = 12000;
static const int RECONNECT_BASE_MS = 1000;

// 지연 누적 가드: TCP는 유실 대신 밀림이 쌓인다(대역폭 부족 시 무한 누적).
// 프레임은 계속 오므로 무프레임 워치독에는 절대 안 걸린다 — 화면만 점점
// 과거가 된다. 전송이 tcp 고정인 현행 구성에 그대로 해당한다.
// 임계는 appsink 경로와 같은 5초, 단 **연속 틱** 조건이 붙는다 (워치독 주석).
static const qint64 LATENCY_CREEP_RESTART_MS = 5000;
static const int LATENCY_CREEP_TICKS = 3;

// 버스 폴링 주기 — 방송 송신기(broadcast_rtp_sender.cpp)와 같은 값
static const int BUS_POLL_MS = 100;

// 도착 지연이 비현실적으로 작으면 RTCP SR 출처가 카메라가 아니다 (중계기 감지,
// gstreamer_backend.cpp와 동일 근거)
static const qint64 RELAY_SUSPECT_MS = 30;

DirectSinkBackend::DirectSinkBackend(int channel, QObject *parent)
    : VideoBackend(parent), m_channel(channel)
{
    static bool gst_ready = false;
    if (!gst_ready) {
        gst_init(nullptr, nullptr);
        gst_ready = true;
    }

    // 이벤트 클립 저장기의 싱글턴을 **여기(GUI 스레드)서** 만들어 둔다 —
    // 첫 접촉이 스트리밍 스레드의 probe(ingest)면 QTimer·시그널 배선이
    // 엉뚱한 스레드에 묶인다.
    ClipRecorder::instance();

    m_watchdog = new QTimer(this);
    connect(m_watchdog, &QTimer::timeout, this, [this] {
        const qint64 silent =
            QDateTime::currentMSecsSinceEpoch() - m_last_frame_ms.load();
        if (silent > NO_FRAME_TIMEOUT_MS) {
            schedule_reconnect(QString("%1ms 무프레임").arg(silent));
            return;
        }

        // 지연 누적은 **세션 최솟값 대비 초과분**으로 잰다. 절대값으로 재면
        // 안 된다 — 07-31에 appsink 경로가 정확히 그걸로 당했다: 카메라 시계가
        // PC보다 ~1.04초 앞서 원시값이 항상 음수라 5초 임계를 영원히 못 넘어
        // 가드가 죽어 있었다. 반대로 카메라 시계가 뒤처지면 같은 코드가 상시
        // 임계 초과가 되어 **4채널이 3초마다 재접속하는 폭주**가 된다.
        //
        // 최솟값을 기준으로 삼는 근거는 PipelineStats::observe_net()과 같다:
        // 최솟값 = (시계차 + 가장 빠른 프레임의 실제 지연). 빼고 남는 값은
        // "가장 빠른 프레임 대비 초과 지연"이라 시계차와 무관하다. 우리가
        // 잡으려는 것이 절대 지연이 아니라 **누적**이므로 이걸로 충분하다.
        const qint64 floor = m_glass_floor_ms.load();
        const qint64 creep =
            floor == FLOOR_UNSET ? 0 : m_glass_latency_ms.load() - floor;
        if (creep <= LATENCY_CREEP_RESTART_MS) {
            m_creep_ticks = 0;
            return;
        }

        // ⚠ 한 번 넘었다고 세션을 뜯지 않는다. 08-10 실측: 이 계측은 RTCP SR
        // 매핑이 자리잡기 전까지 크게 흔들려, 한 채널 안에서 원시값이
        // −3273 ~ +3584ms(폭 6.9초)로 튀었다. **임계보다 잡음이 크다** —
        // 즉시 재접속하게 두면 멀쩡한 세션을 정기적으로 뜯는 재시작 폭풍이
        // 된다(ONVIF 교차검증에서 두 번 겪은 실패 모양 그대로).
        //
        // 우리가 잡으려는 것은 스파이크가 아니라 **누적**이다. 연속 3틱(9초)
        // 동안 계속 넘어 있어야 누적으로 본다 — TCP 밀림은 시간이 지날수록
        // 커지므로 이 조건에 반드시 걸리고, 튐은 걸리지 않는다.
        if (++m_creep_ticks < LATENCY_CREEP_TICKS)
            return;
        schedule_reconnect(QString("지연 누적 %1ms (%2틱 연속)")
                               .arg(creep).arg(m_creep_ticks));
    });

    m_reconnect = new QTimer(this);
    m_reconnect->setSingleShot(true);
    connect(m_reconnect, &QTimer::timeout, this, [this] { start_pipeline(); });

    m_bus_poll = new QTimer(this);
    connect(m_bus_poll, &QTimer::timeout, this, &DirectSinkBackend::poll_bus);
}

DirectSinkBackend::~DirectSinkBackend()
{
    teardown();
    QMutexLocker lock(&m_comp_mutex);
    if (m_comp) {
        gst_video_overlay_composition_unref(m_comp);
        m_comp = nullptr;
    }
}

QWidget *DirectSinkBackend::create_widget(QWidget *parent)
{
    // sink가 그릴 네이티브 창. Qt가 배경을 지우면 영상과 깜박임 경쟁이
    // 생기므로 페인팅을 전부 sink에 맡긴다. 마우스는 기본 무시 → 부모
    // (ChannelView)로 전파되어 클릭/추적 선택이 그대로 동작한다.
    m_widget = new QWidget(parent);
    m_widget->setAttribute(Qt::WA_NativeWindow);
    m_widget->setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_widget->setAttribute(Qt::WA_NoSystemBackground);
    m_widget->setAttribute(Qt::WA_OpaquePaintEvent);
    (void)m_widget->winId();  // HWND를 지금 만든다 — play()에서 바로 넘기게
    return m_widget;
}

void DirectSinkBackend::play(const QUrl &url)
{
    // 같은 스트림이면 재구축하지 않는다 — 그리드=풀스크린 프로파일일 때
    // 전환이 위젯 리사이즈만으로 즉시 끝나게 (2026-08-04)
    if (url == m_url && m_pipeline)
        return;
    m_url = url;
    m_retry_count = 0;
    start_pipeline();
}

void DirectSinkBackend::stop()
{
    m_reconnect->stop();
    teardown();
    set_status(QString());
}

QString DirectSinkBackend::status_text() const
{
    return m_status;
}

void DirectSinkBackend::start_pipeline()
{
    teardown();

    // 대기 중인 재접속 예약을 취소한다. 지금 세션을 세우는 순간 "나중에 다시
    // 세워라"는 예약은 의미가 없어지고, 남겨두면 나중에 터져 **멀쩡한 세션을
    // 뜯어낸다.** 창이 열리는 경로는 실제로 있다: schedule_reconnect() 가
    // 백오프(최대 15초)를 걸어둔 사이에 레이아웃·프로파일 전환이 play() 를
    // 부르면, 새 파이프라인이 붙은 뒤에 옛 타이머가 뒤늦게 발화한다.
    //
    // play() 가 아니라 여기에 두는 이유: 세션이 시작되는 지점은 이 함수
    // 하나뿐이라, 새 호출부가 생겨도 저절로 지켜진다. (재접속 타이머 자신이
    // 이 함수를 부를 때는 이미 single-shot 발화가 끝난 뒤라 무해하다)
    m_reconnect->stop();

    m_had_frame = false;
    // 프로파일 전환(그리드↔풀스크린) = 새 파이프라인 = 새 해상도. 리셋 안
    // 하면 합성 렌더 사각형이 이전 해상도에 박제돼 크롬이 좌상단에 쪼그라든다.
    m_video_w = 0;
    m_video_h = 0;
    // 지연 계측도 세션 단위다. 이전 세션의 값을 물려받으면 새 세션이 첫
    // 프레임도 받기 전에 "누적 초과"로 판정돼 재기동 루프에 빠진다.
    m_glass_latency_ms = -1;
    m_glass_floor_ms = FLOOR_UNSET;
    m_creep_ticks = 0;
    // 부드러움 창도 세션 단위다 — 끊긴 구간의 공백이 다음 창의 maxgap 으로
    // 넘어가면 멀쩡한 세션이 나쁘게 보인다. teardown() 이 이미 콜백을 끊은
    // 뒤라 스트리밍 스레드와 경합하지 않는다.
    m_sm_window_start_ms = 0;
    m_sm_prev_ms = 0;
    m_sm_max_gap = 0;
    m_sm_count = 0;
    m_sm_sum = 0.0;
    m_sm_sum_sq = 0.0;

    QSettings settings("GuardX", "VMS");
    // 전송 기본값은 video_backend.h 의 rtsp_transport() 한 곳에만 있다
    const QString transport = ::rtsp_transport();
    const int jitter_ms = settings.value("rtsp_jitter_ms", 0).toInt();

    // 디코더 선택은 appsink 경로와 같은 키를 쓴다. direct 권장은 d3d11-flex —
    // d3d11videosink와 같은 API라 GPU 메모리 그대로 넘어간다(zero-copy).
    // d3d12 계열을 고르면 시스템 메모리를 한 번 경유한다 (동작은 함).
    QString profile_token;
    const QStringList parts = m_url.path().split('/', Qt::SkipEmptyParts);
    if (parts.size() >= 2)
        profile_token = parts[parts.size() - 2];
    const QString decoder =
        settings.value("decoder_" + profile_token,
                       settings.value("decoder", "d3d11-flex")).toString();

    QString decode_seg;
    if (decoder == "software")
        // ⚠ caps 를 **못 박는다** (2026-08-12 A/B). 이게 없으면 videoconvert 가
        // d3d11videosink 와 협상해 무엇으로든 갈 수 있고, 실측 결과 프레임당
        // 작업이 33ms 예산을 넘겨 **재시작 루프**가 됐다: 지연이 7~11초까지
        // 기어올라 creep 가드가, 혹은 12~14초 무프레임으로 워치독이 세션을
        // 뜯는 일이 7분에 20회. CPU 는 7.55% 로 한가했으므로 연산량이 아니라
        // 협상된 변환·업로드 경로가 원인이다.
        // appsink 경로는 처음부터 NV12 를 명시하고 있었다 — 같은 계약으로 맞춘다.
        decode_seg = "avdec_h264 ! videoconvert n-threads=2 "
                     "! video/x-raw,format=NV12";
    else if (decoder == "d3d11")
        decode_seg = "d3d11h264dec";
    else if (decoder == "d3d12")
        decode_seg = "d3d12h264dec";
    else if (decoder == "d3d12-flex")
        decode_seg = "d3d12h264dec compliance=flexible";
    else  // 기본: d3d11-flex
        decode_seg = "d3d11h264dec compliance=flexible";

    // ⚠ 이 파이프라인엔 ONVIF 메타데이터 분기를 두지 않는다 (2026-08-05
    // 실사고): 영상 세션에 메타 pad를 얹으면 rtspsrc의 pad 노출이 세션마다
    // 비결정적으로 절름발이가 됐다 (무작위 채널이 video만/meta만/무pad —
    // 12s 무프레임 재접속 루프). 메타데이터는 OnvifMetaSource가 채널별
    // 전용 경량 세션으로 받는다 — 여긴 검증된 원형 그대로.
    const QString desc =
        QString("rtspsrc name=src location=\"%1\" protocols=%2 latency=%3 "
                "drop-on-latency=%4 do-retransmission=false "
                "add-reference-timestamp-meta=true "
                "! rtph264depay ! h264parse "
                // 표시 지연 큐 (+/- 키). **압축 상태에서** 붙든다 — 디코드
                // 뒤에 두면 d3d11 디코더의 출력 서피스 풀을 잠식해 파이프라인이
                // 굶는다. 100ms 분량이 압축이면 ~19KB, 디코드 후면 1.6MB다.
                // min-threshold-time=0 이면 통과 큐라 지연 없이 동작한다.
                "! queue name=delayq max-size-buffers=0 max-size-bytes=0 "
                "max-size-time=3000000000 min-threshold-time=%6 "
                "! %5 "
                // force-aspect-ratio=false: appsink 경로(RhiVideoWidget)와
                // 동일하게 위젯을 꽉 채운다 — 박스 좌표 변환이 단순 비례 유지
                "! d3d11videosink name=vsink sync=false force-aspect-ratio=false "
                "draw-on-shared-texture=false")
            .arg(m_url.toString(QUrl::FullyEncoded), transport)
            .arg(jitter_ms)
            .arg(jitter_ms == 0 ? "true" : "false")
            .arg(decode_seg)
            .arg(qint64(m_video_delay_ms.load()) * GST_MSECOND);

    qInfo().noquote() << "[DirectSink] ch" << m_channel
                      << "profile=" << profile_token
                      << "transport=" << transport << "decoder=" << decoder;

    GError *error = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &error);
    if (!m_pipeline) {
        const QString msg = error ? QString::fromUtf8(error->message) : "?";
        if (error)
            g_error_free(error);
        qWarning() << "[DirectSink] ch" << m_channel << "파이프라인 생성 실패:" << msg;
        schedule_reconnect("파이프라인 생성 실패");
        return;
    }
    if (error)
        g_error_free(error);

    m_bus_poll->start(BUS_POLL_MS);

    // 이벤트 클립 링버퍼 (전후 15초 녹화). 새 세션 = 새 PTS 축이라 링을
    // 비우고 다시 쌓는다. 탭 지점은 delayq **입구** — h264parse 직후·표시
    // 지연(min-threshold-time) 앞이라, +/- 키로 화면을 늦춰도 녹화 시각은
    // 실시간이다. probe 는 버퍼를 ref 만 하므로 표시 경로 비용이 없다.
    ClipRecorder::instance()->on_session_reset(m_channel);
    if (GstElement *delayq = gst_bin_get_by_name(GST_BIN(m_pipeline), "delayq")) {
        if (GstPad *pad = gst_element_get_static_pad(delayq, "sink")) {
            gst_pad_add_probe(
                pad, GST_PAD_PROBE_TYPE_BUFFER,
                [](GstPad *p, GstPadProbeInfo *info, gpointer user)
                    -> GstPadProbeReturn {
                    if (GstBuffer *buf = gst_pad_probe_info_get_buffer(info))
                        ClipRecorder::instance()->ingest(
                            GPOINTER_TO_INT(user), p, buf);
                    return GST_PAD_PROBE_OK;
                },
                GINT_TO_POINTER(m_channel), nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(delayq);
    }

    if (GstElement *vsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "vsink")) {
        // Qt 위젯의 HWND에 직접 그리게 한다 — 그리드 배치는 Qt가 유지
        if (m_widget)
            gst_video_overlay_set_window_handle(
                GST_VIDEO_OVERLAY(vsink), (guintptr)m_widget->winId());

        // sink 입구 프로브: 오버레이 메타 부착 + 도착 계측 + 해상도 파악
        if (GstPad *pad = gst_element_get_static_pad(vsink, "sink")) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                              &DirectSinkBackend::s_sink_in, this, nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(vsink);
    }

    m_last_frame_ms = QDateTime::currentMSecsSinceEpoch();
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    m_watchdog->start(WATCHDOG_INTERVAL_MS);
    set_status(QStringLiteral("connecting..."));
    emit session_started();   // 상위가 키프레임을 강제한다 (검은 화면 단축)
}

void DirectSinkBackend::teardown()
{
    m_watchdog->stop();
    if (m_bus_poll)
        m_bus_poll->stop();
    // 파이프라인이 없으면 그리는 것도 없다 — ChannelView 가 이 값을 보고
    // 네이티브 창을 숨긴다(잔상 방지). stop() 경로에서도 반드시 내려가야 한다.
    m_had_frame = false;
    if (!m_pipeline)
        return;
    // NULL 전이는 동기 — 리턴 후엔 스트리밍 콜백이 더 안 온다 (TEARDOWN 송신)
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
}

void DirectSinkBackend::set_video_delay_ms(int ms)
{
    ms = qBound(0, ms, 3000);            // max-size-time 과 같은 상한
    if (m_video_delay_ms.exchange(ms) == ms)
        return;

    // ⚠ 지연을 바꾸면 creep 가드의 기준선을 반드시 다시 잡아야 한다.
    // 가드는 "세션 최솟값 대비 초과분"으로 누적을 판정하는데, 지연을 +500ms
    // 주면 그 순간부터 모든 표본이 옛 최솟값보다 500ms 크다 — 우리가 의도해서
    // 넣은 지연을 가드가 **장애로 오인해** 멀쩡한 세션을 뜯는다.
    m_glass_floor_ms = FLOOR_UNSET;
    m_creep_ticks = 0;

    qInfo().noquote() << QString("[DirectSink] ch %1 표시 지연 = %2 ms")
                             .arg(m_channel).arg(ms);

    if (!m_pipeline)
        return;                          // 다음 start_pipeline 이 반영한다
    if (GstElement *q = gst_bin_get_by_name(GST_BIN(m_pipeline), "delayq")) {
        g_object_set(q, "min-threshold-time",
                     guint64(ms) * GST_MSECOND, nullptr);
        gst_object_unref(q);
    }
}

void DirectSinkBackend::expose()
{
    if (!m_pipeline)
        return;
    // 마지막 버퍼를 다시 제시한다 (탭 복귀 등으로 창이 다시 떴을 때).
    // 프레임이 한 장도 없었으면 sink 가 그릴 게 없다 — 그 경우는
    // ChannelView 가 창을 숨기고 Qt 로 직접 그리는 쪽이 담당한다.
    if (GstElement *vsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "vsink")) {
        gst_video_overlay_expose(GST_VIDEO_OVERLAY(vsink));
        gst_object_unref(vsink);
    }
}

void DirectSinkBackend::schedule_reconnect(const QString &reason)
{
    teardown();
    const int delay = qMin(RECONNECT_BASE_MS << qMin(m_retry_count, 4), 15000);
    ++m_retry_count;
    qWarning().noquote() << QString("[DirectSink] ch %1 \"%2\" — 재접속 %3 ms 후 (시도 %4)")
                                .arg(m_channel).arg(reason).arg(delay).arg(m_retry_count);
    set_status(QString("reconnecting... (%1)").arg(m_retry_count));
    m_reconnect->start(delay);
}

void DirectSinkBackend::set_status(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    emit status_changed();
}

void DirectSinkBackend::set_overlay_image(const QImage &image)
{
    // UI 스레드에서 호출. QImage(ARGB32_Premultiplied)의 메모리 배치는
    // little-endian에서 BGRA — GStreamer BGRA 포맷과 일치한다.
    GstVideoOverlayComposition *comp = nullptr;

    if (!image.isNull()) {
        QImage img = image;
        if (img.format() != QImage::Format_ARGB32_Premultiplied)
            img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        const int w = img.width(), h = img.height();
        GstBuffer *buf =
            gst_buffer_new_allocate(nullptr, gsize(img.sizeInBytes()), nullptr);
        gst_buffer_fill(buf, 0, img.constBits(), gsize(img.sizeInBytes()));
        gst_buffer_add_video_meta(buf, GST_VIDEO_FRAME_FLAG_NONE,
                                  GST_VIDEO_FORMAT_BGRA, w, h);

        // 렌더 좌표는 영상 프레임 픽셀 공간 — 캔버스가 프레임과 같은 비율로
        // 그려졌으므로 (0,0,영상폭,영상높이)에 얹으면 위젯 전체를 정확히 덮는다
        const int vw = m_video_w.load() > 0 ? m_video_w.load() : w;
        const int vh = m_video_h.load() > 0 ? m_video_h.load() : h;
        GstVideoOverlayRectangle *rect = gst_video_overlay_rectangle_new_raw(
            buf, 0, 0, guint(vw), guint(vh),
            GST_VIDEO_OVERLAY_FORMAT_FLAG_PREMULTIPLIED_ALPHA);
        comp = gst_video_overlay_composition_new(rect);
        gst_video_overlay_rectangle_unref(rect);
        gst_buffer_unref(buf);
    }

    QMutexLocker lock(&m_comp_mutex);
    if (m_comp)
        gst_video_overlay_composition_unref(m_comp);
    m_comp = comp;  // nullptr이면 오버레이 없음 (그대로 유효)
}

GstPadProbeReturn DirectSinkBackend::s_sink_in(GstPad *pad, GstPadProbeInfo *info,
                                               gpointer user)
{
    auto *self = static_cast<DirectSinkBackend *>(user);
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer)
        return GST_PAD_PROBE_OK;

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    self->m_last_frame_ms = now_ms;
    if (GST_BUFFER_PTS_IS_VALID(buffer))
        self->m_pts_ms = qint64(GST_BUFFER_PTS(buffer) / GST_MSECOND);

    // ---- 부드러움(smoothness): 프레임 도착 간격의 일관성 (08-12 이식) ----
    // 30fps면 이상적으로 33ms 간격. 5초 창마다 실효 fps / 간격 표준편차(지터) /
    // 최대 공백을 낸다. 지터가 크고 maxgap 이 튀면 present 가 수신 스레드를
    // 막고 있다는 신호다 (파이프라인에 queue 가 없어 한 스레드에 직렬).
    if (self->m_sm_prev_ms > 0) {
        const qint64 gap = now_ms - self->m_sm_prev_ms;
        self->m_sm_count++;
        self->m_sm_sum += double(gap);
        self->m_sm_sum_sq += double(gap) * double(gap);
        self->m_sm_max_gap = qMax(self->m_sm_max_gap, gap);
    }
    self->m_sm_prev_ms = now_ms;
    if (self->m_sm_window_start_ms == 0)
        self->m_sm_window_start_ms = now_ms;
    if (const qint64 sm_win = now_ms - self->m_sm_window_start_ms;
        sm_win >= 5000 && self->m_sm_count > 1) {
        const double mean = self->m_sm_sum / self->m_sm_count;
        const double var = self->m_sm_sum_sq / self->m_sm_count - mean * mean;
        const double jitter = var > 0.0 ? std::sqrt(var) : 0.0;
        const double fps = self->m_sm_count * 1000.0 / double(sm_win);
        qDebug().noquote()
            << QString("[DirectSink] ch %1 smoothness: fps %2 jitter %3 ms "
                       "maxgap %4 ms")
                   .arg(self->m_channel)
                   .arg(fps, 0, 'f', 1)
                   .arg(qRound(jitter))
                   .arg(self->m_sm_max_gap);
        self->m_sm_window_start_ms = now_ms;
        self->m_sm_count = 0;
        self->m_sm_sum = 0.0;
        self->m_sm_sum_sq = 0.0;
        self->m_sm_max_gap = 0;
    }
    // ------------------------------------------------------------------

    // "연결 중…" 해제는 PLAYING 전이가 아니라 **첫 프레임**에서 — PLAYING은
    // 협상 성공일 뿐 프레임 도착을 보장하지 않는다 (appsink 경로와 동일 정책)
    if (!self->m_had_frame.exchange(true)) {
        QMetaObject::invokeMethod(self, [self] {
            self->m_retry_count = 0;
            self->set_status(QString());
        });
    }

    // 영상 해상도 — 오버레이 캔버스 크기의 기준 (첫 버퍼에서 한 번)
    if (self->m_video_w.load() == 0) {
        if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
            GstVideoInfo vinfo;
            if (gst_video_info_from_caps(&vinfo, caps)) {
                self->m_video_w = GST_VIDEO_INFO_WIDTH(&vinfo);
                self->m_video_h = GST_VIDEO_INFO_HEIGHT(&vinfo);
            }
            gst_caps_unref(caps);
        }
    }

    // 도착 계측 (P0→sink 입구). appsink 경로의 glass-to-arrival과 같은 원천
    // (RTCP SR meta) — 단 구간 분해(net/decode)는 없다.
    static GstCaps *ntp_caps = gst_caps_new_empty_simple("timestamp/x-ntp");
    if (GstReferenceTimestampMeta *meta =
            gst_buffer_get_reference_timestamp_meta(buffer, ntp_caps)) {
        const qint64 capture_ms =
            qint64(meta->timestamp / GST_MSECOND) - Q_INT64_C(2208988800000);
        const qint64 raw = now_ms - capture_ms;
        self->m_glass_latency_ms = raw;
        // 세션 최솟값 갱신 — 워치독의 지연 누적 판정 기준선.
        // 쓰는 쪽은 이 스트리밍 스레드 하나뿐이라(리셋은 teardown 이후 =
        // 콜백이 끊긴 뒤) load-비교-store 로 충분하다.
        if (raw < self->m_glass_floor_ms.load())
            self->m_glass_floor_ms = raw;
        // 박스↔프레임 매칭 기준 시각 — ONVIF UtcTime과 같은 시계 (실증)
        self->m_frame_utc_ms = capture_ms;
        if (now_ms - self->m_last_latency_log_ms.load() > 5000) {
            self->m_last_latency_log_ms = now_ms;
            qDebug().noquote()
                << QString("[DirectSink] ch %1 glass-to-sink %2 ms (원시)%3")
                       .arg(self->m_channel).arg(raw)
                       .arg(raw >= 0 && raw < RELAY_SUSPECT_MS
                                ? QStringLiteral(" ⚠ 비현실적 — 중계기/시계 의심")
                                : QString());
        }
    }

    // 오버레이 합성 메타 부착 — sink가 GPU에서 영상 위에 그린다
    QMutexLocker lock(&self->m_comp_mutex);
    if (self->m_comp) {
        buffer = gst_buffer_make_writable(buffer);
        GST_PAD_PROBE_INFO_DATA(info) = buffer;
        gst_buffer_add_video_overlay_composition_meta(buffer, self->m_comp);
    }
    return GST_PAD_PROBE_OK;
}

void DirectSinkBackend::poll_bus()
{
    if (!m_pipeline)
        return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    if (!bus)
        return;

    // 한 번에 몰아 비운다 — 남겨두면 버스가 계속 차오른다.
    while (GstMessage *msg = gst_bus_pop_filtered(
               bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        QString reason;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            reason = QString("stream error: %1")
                         .arg(err ? QString::fromUtf8(err->message)
                                  : QStringLiteral("unknown"));
            if (err)
                g_error_free(err);
            g_free(debug);
        } else {
            reason = QStringLiteral("EOS");
        }
        // pop 한 메시지는 내가 unref 한다 (워치 콜백과 다른 규약).
        gst_message_unref(msg);
        gst_object_unref(bus);
        // 폴링은 GUI 스레드라 invokeMethod 로 넘길 필요가 없다. 다만
        // schedule_reconnect()가 teardown 하므로 이 버스는 여기서 버린다.
        schedule_reconnect(reason);
        return;
    }
    gst_object_unref(bus);
}
