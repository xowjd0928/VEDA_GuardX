#include "broadcast_rtp_sender.h"
#include "broadcast_pipeline.h"

// GStreamer는 VMS 빌드에서 옵션이다(HAVE_GSTREAMER). 없으면 이 기능만 비활성
// 스텁으로 컴파일돼, GStreamer 없는 팀원 빌드가 깨지지 않는다.
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

#include <QTimer>

#ifdef HAVE_GSTREAMER
namespace {

// 플러그인 탐지와 노이즈 게이트 상수는 broadcast_pipeline.h 로 옮겼다 —
// 벤치마크가 같은 파이프라인을 세우려면 그 판단을 공유해야 한다.
using guardx::broadcast::kGateRatioOn;
using guardx::broadcast::kGateRatioOff;

// 버스 폴링 주기. level 메시지가 100ms 간격이라 그보다 촘촘할 이유가 없다.
constexpr int kBusPollMs = 100;

/**
 * @brief level 메시지의 첫 채널 값을 꺼낸다.
 *
 * ⚠ level 엘리먼트는 값을 **GValueArray**(GLib의 구형 타입)로 싣는다 —
 * GStreamer 고유의 GstValueArray가 아니다. GST_VALUE_HOLDS_ARRAY로 검사하면
 * 영원히 거짓이라 레벨이 조용히 안 잡힌다(실측). 버전에 따라 어느 쪽이든
 * 올 수 있으므로 둘 다 받는다.
 */
bool first_channel_db(const GstStructure *s, const char *field, double *out)
{
    const GValue *v = gst_structure_get_value(s, field);
    if (!v)
        return false;

    if (GST_VALUE_HOLDS_ARRAY(v) && gst_value_array_get_size(v) > 0) {
        *out = g_value_get_double(gst_value_array_get_value(v, 0));
        return true;
    }

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    if (G_VALUE_HOLDS(v, G_TYPE_VALUE_ARRAY)) {
        auto *arr = static_cast<GValueArray *>(g_value_get_boxed(v));
        if (arr && arr->n_values > 0) {
            *out = g_value_get_double(g_value_array_get_nth(arr, 0));
            return true;
        }
    }
    G_GNUC_END_IGNORE_DEPRECATIONS

    return false;
}

} // namespace
#endif

BroadcastRtpSender::BroadcastRtpSender(QObject *parent) : QObject(parent)
{
#ifdef HAVE_GSTREAMER
    // 영상 백엔드가 이미 gst_init 했을 수 있으나 중복 호출은 안전(idempotent).
    if (!gst_is_initialized())
        gst_init(nullptr, nullptr);
#endif
}

BroadcastRtpSender::~BroadcastRtpSender()
{
    stop();
}

QString BroadcastRtpSender::build_description(const QString &host, int port,
                                              int bitrate, bool denoise,
                                              bool agc, int volume_percent)
{
    // 파이프라인 모양은 broadcast_pipeline.h 한 곳에만 있다. 여기서 다시
    // 쓰면 벤치마크가 재는 파이프라인과 실제 방송 파이프라인이 조용히
    // 갈라진다 — 숫자는 그럴듯한데 다른 것을 잰 상태가 된다.
    return guardx::broadcast::build_pipeline(host, port, bitrate, denoise, agc,
                                             volume_percent, QString(),
                                             &m_denoise_backend);
}

bool BroadcastRtpSender::start(const QString &host, int port, int bitrate,
                               bool denoise, bool agc, int volume_percent)
{
#ifdef HAVE_GSTREAMER
    if (m_pipeline)
        return true;

    const QString desc = build_description(host, port, bitrate, denoise, agc,
                                           volume_percent);

    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!pipeline || err) {
        // 설치본에 따라 속성 이름/열거값이 다를 수 있다. 개선판이 안 서면
        // 방송 자체를 못 하게 두지 말고 최소 파이프라인으로 되돌린다.
        const QString why = err ? QString::fromUtf8(err->message)
                                : QStringLiteral("unknown error");
        if (err) {
            g_error_free(err);
            err = nullptr;
        }
        if (pipeline) {
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        // 상태 표시줄에는 아래 start_rtp()가 찍는 "노캔 없음(폴백)"으로 드러난다.
        // 실패 사유까지 UI에 띄우면 곧바로 "방송 중..."에 덮여 아무도 못 본다 —
        // 진단은 로그로 남긴다.
        qWarning("[broadcast] 고음질 파이프라인 생성 실패 → 기본 경로로 폴백: %s",
                 qPrintable(why));
        m_denoise_backend = QStringLiteral("none (fallback)");
        const QString fallback =
            QStringLiteral(
                "autoaudiosrc ! audioconvert ! audioresample ! "
                "audio/x-raw,rate=48000,channels=1 ! "
                "opusenc bitrate=%1 ! rtpopuspay pt=96 ! "
                "udpsink host=%2 port=%3 sync=false")
                .arg(bitrate).arg(host).arg(port);
        pipeline = gst_parse_launch(fallback.toUtf8().constData(), &err);
        if (!pipeline || err) {
            const QString msg = err ? QString::fromUtf8(err->message)
                                    : QStringLiteral("failed to build the RTP pipeline");
            if (err)
                g_error_free(err);
            if (pipeline)
                gst_object_unref(pipeline);
            emit error(msg);
            return false;
        }
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        // ⚠ NULL 로 되돌린 뒤에 unref 해야 한다 (2026-08-10 수정).
        //
        // PLAYING 전이가 **실패**했다는 건 도중까지는 갔다는 뜻이다 — 원소들이
        // READY/PAUSED 로 올라가며 오디오 장치 핸들과 스트리밍 스레드를 이미
        // 잡았을 수 있다. NULL 전이를 건너뛰고 unref 하면 그것들이 반납되지
        // 않고, GStreamer 가 "dispose while not in NULL state" critical 을 찍는다.
        // 다음 방송 시도에서 마이크가 안 열리는 형태로 드러난다.
        //
        // 바로 위 두 실패 경로(파이프라인 생성 실패)는 gst_parse_launch 직후라
        // NULL 상태이므로 그냥 unref 하는 것이 맞다 — 여기만 다르다.
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        emit error(QStringLiteral("failed to start RTP send (check the audio device and plugins)"));
        return false;
    }

    m_pipeline = pipeline;
    // 런타임 토글용 핸들. gst_bin_get_by_name은 참조를 넘겨주지만 파이프라인이
    // 자식을 소유하므로, stop()에서 파이프라인과 함께 정리한다.
    m_dsp = gst_bin_get_by_name(GST_BIN(pipeline), "dsp");
    m_gate = gst_bin_get_by_name(GST_BIN(pipeline), "gate");
    m_volume = gst_bin_get_by_name(GST_BIN(pipeline), "vol");

    // 버스 감시 시작 — 이게 없으면 방송 시작 이후에 마이크가 뽑히거나 장치가
    // 죽어도 UI는 계속 "방송 중"으로 남는다(MQTT 경로는 QAudioSource 상태로
    // 이미 처리하고 있던 것 — RTP 경로만 빠져 있었다).
    if (!m_bus_timer) {
        m_bus_timer = new QTimer(this);
        connect(m_bus_timer, &QTimer::timeout, this, &BroadcastRtpSender::poll_bus);
    }
    m_bus_timer->start(kBusPollMs);
    return true;
#else
    Q_UNUSED(host);
    Q_UNUSED(port);
    Q_UNUSED(bitrate);
    Q_UNUSED(denoise);
    Q_UNUSED(agc);
    Q_UNUSED(volume_percent);
    emit error(QStringLiteral(
        "This VMS build has no GStreamer, so RTP broadcast is unavailable."));
    return false;
#endif
}

void BroadcastRtpSender::set_denoise(bool on)
{
#ifdef HAVE_GSTREAMER
    // 방송 중이 아니면 다음 start()에서 반영된다.
    if (m_dsp)
        g_object_set(m_dsp, "noise-suppression", on ? TRUE : FALSE, nullptr);
    if (m_gate)
        g_object_set(m_gate, "ratio",
                     on ? kGateRatioOn : kGateRatioOff, nullptr);
#endif
}

void BroadcastRtpSender::set_volume_percent(int percent)
{
#ifdef HAVE_GSTREAMER
    // 방송 중이 아니면 다음 start() 에서 반영된다 — set_denoise 와 같은 규칙.
    if (m_volume)
        g_object_set(m_volume, "volume",
                     guardx::broadcast::volume_gain(percent), nullptr);
#else
    Q_UNUSED(percent);
#endif
}

void BroadcastRtpSender::poll_bus()
{
#ifdef HAVE_GSTREAMER
    if (!m_pipeline)
        return;

    GstBus *bus = gst_element_get_bus(m_pipeline);
    if (!bus)
        return;

    // 한 번에 몰아 비운다 — 남겨두면 버스가 계속 차오른다.
    while (GstMessage *msg = gst_bus_pop_filtered(
               bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING
                                   | GST_MESSAGE_EOS | GST_MESSAGE_ELEMENT))) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            const QString text = err ? QString::fromUtf8(err->message)
                                     : QStringLiteral("unknown error");
            if (debug) {
                qWarning("[broadcast] 파이프라인 오류: %s | %s",
                         qPrintable(text), debug);
                g_free(debug);
            }
            if (err)
                g_error_free(err);
            gst_message_unref(msg);
            gst_object_unref(bus);
            // 상위(BroadcastController)가 stop()까지 처리한다.
            emit error(QString("The broadcast stopped: %1").arg(text));
            return;
        }
        case GST_MESSAGE_EOS:
            gst_message_unref(msg);
            gst_object_unref(bus);
            emit error("The microphone input ended (EOS).");
            return;
        case GST_MESSAGE_WARNING: {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_warning(msg, &err, &debug);
            // 경고는 방송을 끊지 않는다 — 로그로만 남긴다.
            qWarning("[broadcast] 경고: %s",
                     err ? err->message : "(내용 없음)");
            if (debug)
                g_free(debug);
            if (err)
                g_error_free(err);
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            double db = 0.0;
            if (s && gst_structure_has_name(s, "level")
                && first_channel_db(s, "rms", &db)) {
                emit level_changed(db);
            }
            break;
        }
        default:
            break;
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
#endif
}

void BroadcastRtpSender::stop()
{
#ifdef HAVE_GSTREAMER
    if (m_bus_timer)
        m_bus_timer->stop();
    if (!m_pipeline)
        return;
    if (m_dsp) {
        gst_object_unref(m_dsp);
        m_dsp = nullptr;
    }
    if (m_gate) {
        gst_object_unref(m_gate);
        m_gate = nullptr;
    }
    if (m_volume) {
        gst_object_unref(m_volume);
        m_volume = nullptr;
    }
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
#endif
}
