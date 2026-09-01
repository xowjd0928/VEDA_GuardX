#include "gstreamer_backend.h"
#include "channel_sync.h"
#include "rhi_video_widget.h"

#include <QDateTime>
#include <QDebug>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <gst/video/video.h>

#include <cmath>
#include <cstring>

// 워치독 주기와 무프레임 판정.
// 첫 프레임까지는 RTSP 핸드셰이크 + I프레임 대기 + 디코더 초기화가 필요해
// 링크가 나쁘면 수 초가 걸린다. 너무 짧으면 무한 재기동에 빠진다.
static const int WATCHDOG_INTERVAL_MS = 3000;
static const int STALL_RESTART_MS = 12000;

// 지연 누적 가드: TCP는 유실 대신 밀림이 쌓인다(대역폭 부족 시 무한 누적).
static const qint64 LATENCY_CREEP_RESTART_MS = 5000;

// 이보다 낮은 "촬영→도착"은 물리적으로 불가능하다(카메라 인코딩만으로도
// 실측 최저 84ms). 나오면 RTCP SR 을 카메라가 아닌 누군가가 만들고 있다.
static const qint64 RELAY_SUSPECT_MS = 30;

// 재접속 백오프 — 카메라가 동시 스트림 수 초과로 거부하는 상황에서
// 쉬지 않고 재시도하면 카메라와 망을 더 괴롭힌다.
static const int RECONNECT_BASE_MS = 1000;
static const int RECONNECT_MAX_MS = 15000;

// 버스 폴링 주기 — 방송 송신기(broadcast_rtp_sender.cpp)와 같은 값
static const int BUS_POLL_MS = 100;

GStreamerBackend::GStreamerBackend(int channel, QObject *parent)
    : VideoBackend(parent), m_channel(channel),
      m_queue(std::make_shared<FrameQueue>())
{
    static bool gst_inited = false;
    if (!gst_inited) {
        gst_init(nullptr, nullptr);
        gst_inited = true;
    }

    // 채널 싱크가 켜지면 느린 채널을 기다리는 동안 프레임을 보관해야 한다.
    // 30fps 기준 MAX_HOLD_MS 를 덮을 만큼 + 여유.
    m_queue->set_capacity(ChannelSync::instance()->enabled()
                              ? (ChannelSync::MAX_HOLD_MS * 30) / 1000 + 4
                              : 1);

    m_watchdog = new QTimer(this);
    connect(m_watchdog, &QTimer::timeout, this, [this] {
        if (!m_pipeline)
            return;
        const qint64 idle =
            QDateTime::currentMSecsSinceEpoch() - m_last_frame_ms.load();
        if (idle > STALL_RESTART_MS) {
            schedule_reconnect(QString("%1ms 무프레임").arg(idle));
            return;
        }
        const qint64 glass = m_glass_latency_ms.load();
        if (glass > LATENCY_CREEP_RESTART_MS)
            schedule_reconnect(QString("지연 누적 %1ms").arg(glass));
    });

    m_reconnect = new QTimer(this);
    m_reconnect->setSingleShot(true);
    connect(m_reconnect, &QTimer::timeout, this, [this] { start_pipeline(); });

    m_bus_poll = new QTimer(this);
    connect(m_bus_poll, &QTimer::timeout, this, &GStreamerBackend::poll_bus);
}

GStreamerBackend::~GStreamerBackend()
{
    teardown();
    ChannelSync::instance()->unregister_channel(m_channel);
}

QWidget *GStreamerBackend::create_widget(QWidget *parent)
{
    m_widget = new RhiVideoWidget(m_queue, parent);
    m_widget->set_sync_channel(m_channel);  // 정렬 이탈 판정용
    return m_widget;
}

QString GStreamerBackend::status_text() const
{
    return m_status;
}

void GStreamerBackend::set_status(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    emit status_changed();
}

void GStreamerBackend::play(const QUrl &url)
{
    // 같은 스트림이면 재구축하지 않는다 — 그리드=풀스크린 프로파일일 때
    // 전환이 위젯 리사이즈만으로 즉시 끝나게 (2026-08-04)
    if (url == m_url && m_pipeline)
        return;
    m_url = url;
    m_retry_count = 0;
    start_pipeline();
}

void GStreamerBackend::start_pipeline()
{
    teardown();

    m_glass_latency_ms = -1;  // 이전 세션 값으로 재기동 루프에 빠지지 않게
    m_had_frame = false;
    m_queue->clear();

    // ⚠ 2026-07-31: 계측 상태를 여기서 되돌리지 않아, **한 번이라도 재접속한
    // 채널은 디코드 구간을 영영 못 쟀다**. m_probe_attached 가 true 로 남아
    // 새 파이프라인의 디코더에 프로브가 안 붙었기 때문(ch3 가 매 실행마다
    // "decode — 표본 없음"이던 이유). 죽은 파이프라인의 PTS 표도 같이 비운다.
    m_probe_attached = false;
    {
        QMutexLocker lock(&m_dec_mutex);
        m_dec_in_ms.clear();
    }
    // 끊긴 구간의 표본이 다음 창에 섞이지 않게 (reset() 은 지금까지 아무도
    // 부르지 않던 죽은 코드였다)
    m_queue->stats().reset();
    m_queue->trace().reset();

    QSettings settings("GuardX", "VMS");
    // 전송 기본값은 video_backend.h 의 rtsp_transport() 한 곳에만 있다
    const QString transport = ::rtsp_transport();
    const int jitter_ms = settings.value("rtsp_jitter_ms", 0).toInt();

    // 프레임 한 장 전 구간 추적 주기 (0이면 끔). 30fps × 4채널을 전부 찍으면
    // 초당 120줄이라 기본은 5초에 한 장.
    m_queue->trace().configure(m_channel,
                               settings.value("trace_interval_ms", 5000).toInt());

    // 디코더 선택 — 프로파일별로 다르게 줄 수 있다 (실측: HW 디코더는 내부적으로
    // ~6프레임(~400ms)을 큐잉해 저해상도에선 software가 4배 빠르다. 하지만 1080p를
    // 여러 채널 software로 돌리면 CPU가 급증하므로, 스트림 역할별로 골라야 한다).
    //   software : avdec_h264 (force-sw-decoders) — 최저지연, 해상도에 비례해 CPU↑
    //   d3d11/d3d12 : Intel Iris Xe Quick Sync HW — 저CPU, 하지만 +~400ms
    //   auto     : decodebin 자동 (이 PC에선 d3d12 HW)
    //
    // 우선순위: decoder_<profile>  >  decoder(전역)  >  "software"(기본)
    // 예) sub=640x480 그리드는 software, main=1080p 풀스크린은 필요시 d3d12.
    // profile 토큰은 RTSP 경로에서 뽑는다: rtsp://.../<ch>/<profile>/media.smp
    QString profile_token;
    const QStringList path_parts = m_url.path().split('/', Qt::SkipEmptyParts);
    if (path_parts.size() >= 2)
        profile_token = path_parts[1];  // "profile4", "profile2" 등

    const QString decoder =
        settings.value("decoder_" + profile_token,
                       settings.value("decoder", "software")).toString();
    // 저지연 HW 디코드 시도: d3d11/d3d12 디코더엔 low-latency 속성이 없지만
    // (실측), compliance=flexible 는 DPB 재정렬을 완화해 프레임을 더 일찍
    // 내보낼 수 있다. B프레임이 없는 이 카메라에선 재정렬 대기가 불필요하므로
    // HW 디코더의 ~400ms 큐를 줄일 여지가 있다. qsv는 별도 Intel HW 경로.
    // name=dec — 디코드 시간 측정 프로브를 걸기 위한 이름. decodebin 경로는
    // 내부 디코더가 동적으로 생기므로 deep-element-added 에서 잡는다.
    QString decode_seg;
    if (decoder == "software")
        // 명시 체인, decodebin 아님: 이 RTSP 세션엔 ONVIF 메타데이터 서브스트림
        // (pt 107)이 동승하는데, decodebin은 그 pad를 먼저 잡는 경합이 있어 기동
        // 직후 확률적으로 not-linked로 죽고 재접속 백오프가 가려왔다 (2026-08-04
        // gst-launch 재현). rtph264depay는 영상 pad만 받아 경합이 원천 소거된다.
        // H.264 전제 — H.265 프로파일(profile3)을 쓰려면 decoder=auto(decodebin).
        decode_seg = "rtph264depay ! h264parse ! avdec_h264 name=dec";
    else if (decoder == "d3d11")
        decode_seg = "rtph264depay ! h264parse ! d3d11h264dec name=dec";
    else if (decoder == "d3d12")
        decode_seg = "rtph264depay ! h264parse ! d3d12h264dec name=dec";
    else if (decoder == "d3d11-flex")  // 저지연 HW 시도
        decode_seg = "rtph264depay ! h264parse ! d3d11h264dec name=dec compliance=flexible";
    else if (decoder == "d3d12-flex")  // 저지연 HW 시도
        decode_seg = "rtph264depay ! h264parse ! d3d12h264dec name=dec compliance=flexible";
    else if (decoder == "qsv")         // Intel Quick Sync (oneVPL) 경로
        decode_seg = "rtph264depay ! h264parse ! qsvh264dec name=dec";
    else
        decode_seg = "decodebin";  // auto

    // drop-on-latency는 지터버퍼 0일 때만 — 버퍼를 준 경우엔 흡수가 목적이므로
    // 늦은 패킷을 버리면 안 된다 (고지터 링크에서 스트림이 굶어 죽는다)
    //
    // decodebin(v2)이어야 한다: decodebin3는 RTP depayloader를 자동 연결하지
    // 못해 rtspsrc 뒤에서 "Internal data stream error"로 죽는다 (실측 확인)
    //
    // NV12로 받는다: 디코더 native 포맷이라 색변환이 없고, GPU 셰이더에서
    // RGB로 바꾸므로 CPU가 픽셀을 만지지 않는다.
    //
    // add-reference-timestamp-meta: RTCP SR의 카메라 절대시각(NTP)을 버퍼에
    // 붙인다 — 지연 실측과 채널 간 정렬의 기준이 된다.
    const QString desc =
        QString("rtspsrc name=src location=\"%1\" protocols=%2 latency=%3 "
                "drop-on-latency=%4 do-retransmission=false "
                "add-reference-timestamp-meta=true "
                "! %5 "
                "! videoconvert n-threads=2 ! video/x-raw,format=NV12 "
                "! appsink name=sink sync=false max-buffers=%6 drop=true "
                "enable-last-sample=false")
            .arg(m_url.toString(QUrl::FullyEncoded), transport)
            .arg(jitter_ms)
            .arg(jitter_ms == 0 ? "true" : "false")
            .arg(decode_seg)
            .arg(ChannelSync::instance()->enabled() ? 24 : 1);

    qInfo().noquote() << "[GStreamerBackend] ch" << m_channel
                      << "profile=" << profile_token
                      << "transport=" << transport << "jitter=" << jitter_ms
                      << "decoder=" << decoder
                      << "sync=" << ChannelSync::instance()->enabled();

    GError *error = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &error);
    if (!m_pipeline) {
        const QString msg = error ? QString::fromUtf8(error->message) : "?";
        if (error)
            g_error_free(error);
        qWarning() << "[GStreamerBackend] ch" << m_channel
                   << "파이프라인 생성 실패:" << msg;
        schedule_reconnect("파이프라인 생성 실패");
        return;
    }
    if (error)
        g_error_free(error);

    // 버스 감시 — 연결 거부/EOS를 즉시 알 수 있는 유일한 경로.
    // 이게 없으면 카메라가 스트림을 거절해도 화면만 까맣고 아무 신호가 없다.
    // (워치가 아니라 폴링인 이유는 poll_bus() 선언부 참고)
    m_bus_poll->start(BUS_POLL_MS);

    g_signal_connect(m_pipeline, "deep-element-added",
                     G_CALLBACK(&GStreamerBackend::s_deep_element_added), this);

    // 명시 디코더 경로: 여기서 바로 프로브를 건다 (decodebin 은 위 시그널이 맡는다)
    if (GstElement *dec = gst_bin_get_by_name(GST_BIN(m_pipeline), "dec")) {
        attach_decoder_probe(dec);
        gst_object_unref(dec);
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = &GStreamerBackend::s_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
    gst_object_unref(sink);

    m_last_frame_ms = QDateTime::currentMSecsSinceEpoch();
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    m_watchdog->start(WATCHDOG_INTERVAL_MS);
    set_status(QStringLiteral("connecting..."));
    emit session_started();   // 상위가 키프레임을 강제한다 (검은 화면 단축)
}

void GStreamerBackend::schedule_reconnect(const QString &reason)
{
    teardown();

    // 지수 백오프 (1s, 2s, 4s … 최대 15s)
    const int delay = qMin(RECONNECT_BASE_MS << qMin(m_retry_count, 4),
                           RECONNECT_MAX_MS);
    ++m_retry_count;

    qWarning() << "[GStreamerBackend] ch" << m_channel << reason
               << "— 재접속" << delay << "ms 후 (시도" << m_retry_count << ")";
    set_status(QString("stream lost - reconnecting (%1)").arg(m_retry_count));

    ChannelSync::instance()->unregister_channel(m_channel);
    m_reconnect->start(delay);
}

void GStreamerBackend::stop()
{
    m_reconnect->stop();
    teardown();
    set_status(QString());
}

void GStreamerBackend::teardown()
{
    m_watchdog->stop();
    if (m_bus_poll)
        m_bus_poll->stop();
    if (!m_pipeline)
        return;
    // NULL 전이는 동기 — 리턴 후엔 스트리밍 콜백이 더 안 온다
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
}

qint64 GStreamerBackend::current_frame_pts_ms() const
{
    return m_pts_ms.load();
}

void GStreamerBackend::poll_bus()
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
            reason = QString("스트림 오류: %1")
                         .arg(err ? QString::fromUtf8(err->message)
                                  : QStringLiteral("unknown"));
            if (err)
                g_error_free(err);
            g_free(debug);
        } else {
            reason = QStringLiteral("stream ended (EOS)");
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

GstFlowReturn GStreamerBackend::s_new_sample(GstAppSink *sink, gpointer user)
{
    auto *self = static_cast<GStreamerBackend *>(user);

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_OK;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (buffer && GST_BUFFER_PTS_IS_VALID(buffer))
        self->m_pts_ms = qint64(GST_BUFFER_PTS(buffer) / GST_MSECOND);

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    self->m_last_frame_ms = now_ms;

    // ---- 부드러움(smoothness): 프레임 도착 간격의 일관성 ----
    // 지연이 낮아도 프레임이 몰려오거나 끊기면 눈에는 끊겨 보인다. 30fps면
    // 이상적으로 33ms 간격. 5초 창마다 실효 fps / 간격 표준편차(지터) /
    // 최대 공백(가장 큰 끊김)을 낸다. 비트레이트↑ 시 I프레임 버스트가 커져
    // 지터/공백이 커지는지 여기서 본다.
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
    const qint64 sm_win = now_ms - self->m_sm_window_start_ms;
    if (sm_win >= 5000 && self->m_sm_count > 1) {
        const double mean = self->m_sm_sum / self->m_sm_count;
        const double var = self->m_sm_sum_sq / self->m_sm_count - mean * mean;
        const double jitter = var > 0.0 ? std::sqrt(var) : 0.0;
        const double fps = self->m_sm_count * 1000.0 / double(sm_win);
        qDebug().noquote()
            << QString("[GStreamerBackend] ch %1 smoothness: fps %2 jitter %3 ms maxgap %4 ms")
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
    // -------------------------------------------------------------

    // ---- 디코드 구간: 디코더 입구(프로브) → 지금(appsink 도착) ----
    const qint64 pts =
        (buffer && GST_BUFFER_PTS_IS_VALID(buffer)) ? qint64(GST_BUFFER_PTS(buffer))
                                                    : -1;
    qint64 dec_in_ms = -1;   ///< 디코더 입구 시각 (절대) — 추적 로그에 쓴다
    qint64 decode_ms = -1;
    if (pts >= 0) {
        QMutexLocker lock(&self->m_dec_mutex);
        if (const auto it = self->m_dec_in_ms.constFind(pts);
            it != self->m_dec_in_ms.cend()) {
            dec_in_ms = it.value();
            decode_ms = now_ms - dec_in_ms;
            self->m_dec_in_ms.erase(it);
        }
    }
    PipelineStats &stats = self->m_queue->stats();
    if (decode_ms >= 0)
        stats.add(PipelineStats::Decode, decode_ms);

    // 카메라 촬영 절대시각(RTCP SR 기반). 지연 실측 + 채널 정렬의 기준.
    // RTCP SR 도착 전(첫 수 초)엔 메타가 없다.
    qint64 capture_unix_ms = -1;
    if (buffer) {
        static GstCaps *ntp_caps = gst_caps_new_empty_simple("timestamp/x-ntp");
        if (GstReferenceTimestampMeta *meta =
                gst_buffer_get_reference_timestamp_meta(buffer, ntp_caps)) {
            // NTP epoch(1900) -> Unix epoch(1970)
            capture_unix_ms = qint64(meta->timestamp / GST_MSECOND)
                              - Q_INT64_C(2208988800000);
            const qint64 raw = now_ms - capture_unix_ms;

            // 전송 구간 = 촬영→도착 전체에서 디코드를 뺀 나머지.
            // 디코드를 못 쟀으면(프로브 미부착 등) 굳이 추정하지 않는다.
            //
            // ⚠ 2026-07-31: 여기서 raw 를 그대로 넣고 있었다. 카메라 시계가
            // PC보다 ~1.04초 앞서 raw 가 항상 −1040ms 대였고, add() 가 음수를
            // 조용히 버려 net·TOTAL 이 통째로 비었다. 리포트는 그걸 "RTCP SR
            // 미수신"이라 오진했지만 SR 은 멀쩡히 오고 있었다(이 블록 안에서
            // glass-to-display 를 찍고 있었던 것이 증거). 시계 오프셋을
            // 추정해 빼고 넣는다 — 자세한 근거는 PipelineStats::observe_net().
            if (decode_ms >= 0)
                stats.add(PipelineStats::Net, stats.observe_net(raw - decode_ms));

            // 워치독의 지연 누적 가드도 보정값을 봐야 한다. 원시값은 음수라
            // 임계(5초)를 영원히 못 넘어 가드가 죽어 있었다.
            const qint64 corrected = stats.apply_offset(raw);
            self->m_glass_latency_ms = corrected;
            ChannelSync::instance()->report_latency(self->m_channel, raw);

            // ---- 중계기(RTSP 릴레이) 개입 감지 ----
            // 지연 측정과 ChannelSync 는 전적으로 **카메라의** RTCP SR NTP 에
            // 의존한다. 경로에 RTSP 릴레이(MediaMTX/ffmpeg/gst-rtsp-server)가
            // 끼면 릴레이가 새 RTP 송신자가 되어 SR 을 **자기 시계로** 다시
            // 만든다. 그러면 이 값은 카메라→VMS 가 아니라 릴레이→VMS 가 되고,
            // 실제 지연은 오히려 늘었는데 숫자만 좋아져 개선으로 오인한다.
            // (RPi 를 경로에 넣을 때 L2 투명 브리지만 안전한 이유)
            //
            // 왜 corrected 가 아니라 raw 로 보는가: observe_net() 의 오프셋은
            // 관측 최솟값이라 corrected 의 하한은 **구조적으로 0**이다. 즉
            // corrected 로 재면 정상 카메라에서도 매 창마다 경고가 뜬다.
            // raw 가 [0,30) 이라는 건 "시계가 사실상 맞는데 촬영→도착이 30ms
            // 미만"이라는 뜻 — 카메라 인코딩만 해도 실측 최저가 84ms 였으므로
            // 이 조합은 SR 출처가 카메라가 아님을 가리킨다.
            if (raw >= 0 && raw < RELAY_SUSPECT_MS
                && now_ms - self->m_last_relay_warn_ms > 30000) {
                self->m_last_relay_warn_ms = now_ms;
                qWarning().noquote()
                    << QString("[GStreamerBackend] ch %1 비현실적 지연 %2 ms "
                               "— RTCP SR 출처가 카메라가 아닐 수 있음(중계기 "
                               "개입?). 측정도 ChannelSync 도 신뢰 불가.")
                           .arg(self->m_channel)
                           .arg(raw);
            }

            if (now_ms - self->m_last_latency_log_ms > 5000) {
                self->m_last_latency_log_ms = now_ms;
                // 이름 주의: 이 값은 **appsink 도착**까지다 (표시 아님).
                // 큐 대기·렌더 제출은 [Pipeline]/[Trace] 로그가 따로 낸다.
                qDebug().noquote()
                    << QString("[GStreamerBackend] ch %1 glass-to-arrival %2 ms "
                               "(원시 %3, 시계보정 %4)")
                           .arg(self->m_channel)
                           .arg(corrected)
                           .arg(raw)
                           .arg(stats.offset_ms());
            }
        }
    }

    // ---- 프레임 한 장 전 구간 추적 (기본 5초에 한 장) ----
    // 여기서 앞 세 단계를 한꺼번에 채운다. 나머지(표시 선택·렌더 제출)는
    // UI 스레드가 같은 pts 로 채워 완성한다.
    self->m_queue->trace().begin(pts, capture_unix_ms, dec_in_ms, now_ms,
                                 stats.offset_ms());

    // 30초 구간별 리포트 — 접속 직후 워밍업은 첫 창이 흡수한다
    if (const QString rep = stats.take_report(self->m_channel, now_ms);
        !rep.isEmpty()) {
        qInfo().noquote() << rep;
    }

    self->m_queue->put(sample, capture_unix_ms, pts);  // 소유권 이전

    if (!self->m_update_pending.exchange(true)) {
        QMetaObject::invokeMethod(self, [self] {
            self->m_update_pending = false;
            if (!self->m_had_frame) {
                self->m_had_frame = true;
                self->m_retry_count = 0;   // 정상화 — 백오프 리셋
                self->set_status(QString());
            }
            if (self->m_widget)
                self->m_widget->request_update();
        }, Qt::QueuedConnection);
    }
    return GST_FLOW_OK;
}

GstPadProbeReturn GStreamerBackend::s_decoder_in(GstPad *, GstPadProbeInfo *info,
                                                 gpointer user)
{
    auto *self = static_cast<GStreamerBackend *>(user);
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf && GST_BUFFER_PTS_IS_VALID(buf)) {
        const qint64 pts = qint64(GST_BUFFER_PTS(buf));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QMutexLocker lock(&self->m_dec_mutex);
        // 디코더가 삼킨 프레임(드롭·참조용)이 쌓이지 않게 상한을 둔다.
        // 120 = 30fps 기준 4초치 — 정상이면 수 개를 넘지 않는다.
        if (self->m_dec_in_ms.size() > 120)
            self->m_dec_in_ms.clear();
        self->m_dec_in_ms.insert(pts, now);
    }
    return GST_PAD_PROBE_OK;
}

void GStreamerBackend::attach_decoder_probe(GstElement *decoder)
{
    if (m_probe_attached || !decoder)
        return;
    GstPad *pad = gst_element_get_static_pad(decoder, "sink");
    if (!pad)
        return;
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                      &GStreamerBackend::s_decoder_in, this, nullptr);
    gst_object_unref(pad);
    m_probe_attached = true;

    gchar *name = gst_element_get_name(decoder);
    qInfo() << "[GStreamerBackend] ch" << m_channel
            << "디코드 계측 부착:" << name;
    g_free(name);
}

void GStreamerBackend::s_deep_element_added(GstBin *, GstBin *,
                                            GstElement *element, gpointer user)
{
    // decodebin 이 고른 디코더에 프로브를 건다 (명시 디코더는 start_pipeline).
    // 판별: 팩토리 klass 에 "Decoder" 가 들어간 원소.
    if (auto *self = static_cast<GStreamerBackend *>(user)) {
        if (GstElementFactory *f = gst_element_get_factory(element)) {
            const gchar *klass_str = gst_element_factory_get_metadata(
                f, GST_ELEMENT_METADATA_KLASS);
            if (klass_str && strstr(klass_str, "Decoder"))
                self->attach_decoder_probe(element);
        }
    }

    // 자동 선택된 디코더에 "존재하는 속성만" 골라 저지연으로 튠한다.
    GObjectClass *klass = G_OBJECT_GET_CLASS(element);

    // avdec 계열: FFmpeg 프레임 스레딩은 스레드 수만큼 프레임 지연을 추가한다.
    if (g_object_class_find_property(klass, "max-threads"))
        g_object_set(element, "max-threads", 1, nullptr);
    // 깨진 프레임을 내보내면 화면 오염 + 후속 프레임 대기 유발
    if (g_object_class_find_property(klass, "output-corrupt"))
        g_object_set(element, "output-corrupt", FALSE, nullptr);
    // qsv/nv 계열 하드웨어 디코더의 저지연 모드
    if (g_object_class_find_property(klass, "low-latency"))
        g_object_set(element, "low-latency", TRUE, nullptr);
}
