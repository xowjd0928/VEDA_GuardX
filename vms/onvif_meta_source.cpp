#include "onvif_meta_source.h"
#include "credentials.h"
#include "detection_feed.h"

#include <QDebug>
#include <QSettings>
#include <QTimer>

#include <gst/app/gstappsink.h>

// 영상 백엔드와 같은 회복 정책 (지수 백오프, 상한 15s)
static const int RETRY_BASE_MS = 1000;
static const int RETRY_MAX_MS = 15000;

// 버스 폴링 주기 — 방송 송신기(broadcast_rtp_sender.cpp)와 같은 값.
// ERROR/EOS 감지가 이만큼 늦어지지만, 대안인 8초 워치독·교차검증에 비하면
// 무시할 수 있는 지연이다.
static const int BUS_POLL_MS = 100;

OnvifMetaSource::OnvifMetaSource(int channel, DetectionFeed *feed,
                                 QObject *parent)
    : QObject(parent), m_channel(channel), m_feed(feed)
{
    static bool gst_ready = false;
    if (!gst_ready) {
        gst_init(nullptr, nullptr);
        gst_ready = true;
    }

    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, [this] { start(); });

    m_bus_poll = new QTimer(this);
    connect(m_bus_poll, &QTimer::timeout, this, &OnvifMetaSource::poll_bus);

    m_data_watch = new QTimer(this);
    m_data_watch->setSingleShot(true);
    connect(m_data_watch, &QTimer::timeout, this, [this] {
        if (!m_pipeline || m_got_data.load())
            return;
        // ⚠ 이 워치독은 **기동 굶주림 전용**이다 (건강한 세션은 PLAY 직후
        // 이벤트 버스트가 반드시 온다). 한 번이라도 문서를 받아본 세션은
        // 재시작 뒤 잠시 조용해도 여기서 죽이지 않는다 — 08-05 오후에
        // "재시작 → 무데이터 → 재시작" 되먹임으로 4분에 22회 churn이 났고,
        // 그 churn이 카메라 세션을 굶겨 영상까지 흔들었다.
        if (m_ever_had_data) {
            // 여기서 스스로 재시작하면 "재시작 → 무데이터 → 재시작" 고리가
            // 된다. 되살리는 판단은 DetectionFeed의 교차검증(사람 존재를
            // 근거로 삼는 쪽)에 맡긴다.
            qWarning() << "[OnvifMeta] ch" << m_channel
                       << "재시작 후 무데이터 — 교차검증에 위임";
            return;
        }
        schedule_restart(QStringLiteral("기동 무데이터 (세션 굶주림)"));
    });
}

OnvifMetaSource::~OnvifMetaSource()
{
    teardown();
}

void OnvifMetaSource::start()
{
    QSettings settings("GuardX", "VMS");
    if (!settings.value("onvif_meta", true).toBool())
        return;
    if (GstElementFactory *f =
            gst_element_factory_find("rtponvifmetadatadepay")) {
        gst_object_unref(f);
    } else {
        qWarning() << "[OnvifMeta] rtponvifmetadatadepay 없음 — HTTP 폴백 유지";
        return;
    }

    teardown();

    // 프로파일은 그리드와 동일 키 — 메타데이터 내용은 프로파일 무관이지만
    // 존재가 확인된 조합(profile2)을 기본으로 쓴다
    const QString profile =
        settings.value("grid_profile", "profile2").toString();
    const QString url = QString("rtsp://%1:554/%2/%3/media.smp")
                            .arg(Credentials::camera_host())
                            .arg(m_channel)
                            .arg(profile);

    // 자격은 URL이 아니라 rtspsrc 속성으로 — URL 인코딩 함정 회피.
    // latency=0: 메타는 XML 몇 장이라 지터버퍼가 잡을 것도 없다.
    const QString desc =
        QString("rtspsrc name=src location=\"%1\" protocols=tcp latency=0 "
                "do-retransmission=false "
                "! rtponvifmetadatadepay "
                "! appsink name=sink sync=false async=false "
                "max-buffers=8 drop=true")
            .arg(url);

    GError *error = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &error);
    if (!m_pipeline) {
        const QString msg = error ? QString::fromUtf8(error->message) : "?";
        if (error)
            g_error_free(error);
        qWarning() << "[OnvifMeta] ch" << m_channel << "파이프라인 실패:" << msg;
        schedule_restart("파이프라인 생성 실패");
        return;
    }
    if (error)
        g_error_free(error);

    if (GstElement *src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src")) {
        g_object_set(src, "user-id",
                     Credentials::camera_user().toUtf8().constData(),
                     "user-pw",
                     Credentials::camera_password().toUtf8().constData(),
                     nullptr);
        // application(메타데이터) 스트림만 SETUP — 영상은 요청조차 안 한다
        g_signal_connect(src, "select-stream",
                         G_CALLBACK(&OnvifMetaSource::s_select_stream), this);
        gst_object_unref(src);
    }

    if (GstElement *sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink")) {
        GstAppSinkCallbacks cb = {};
        cb.new_sample = &OnvifMetaSource::s_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, this, nullptr);
        gst_object_unref(sink);
    }

    m_bus_poll->start(BUS_POLL_MS);

    m_got_data = false;
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    m_data_watch->start(8000);
}

void OnvifMetaSource::poke_restart(const QString &reason)
{
    if (m_retry->isActive())
        return;                 // 이미 재시작 대기 중
    if (!m_pipeline)
        return;                 // 아직 기동 전 (스태거 대기)
    schedule_restart(reason);
}

void OnvifMetaSource::teardown()
{
    if (m_data_watch)
        m_data_watch->stop();
    if (m_bus_poll)
        m_bus_poll->stop();
    if (!m_pipeline)
        return;
    // NULL 전이는 동기 — TEARDOWN 송신 (유령 세션 방지, 영상 백엔드와 동일)
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
}

void OnvifMetaSource::schedule_restart(const QString &reason)
{
    teardown();
    const int delay =
        qMin(RETRY_BASE_MS << qMin(m_retry_count, 4), RETRY_MAX_MS);
    ++m_retry_count;
    qWarning().noquote()
        << QString("[OnvifMeta] ch %1 \"%2\" — 재시작 %3ms 후 (시도 %4)")
               .arg(m_channel).arg(reason).arg(delay).arg(m_retry_count);
    m_retry->start(delay);
}

gboolean OnvifMetaSource::s_select_stream(GstElement *, guint, GstCaps *caps,
                                          gpointer)
{
    // SDP 캡스에서 media=application(ONVIF 메타)만 채택
    if (!caps)
        return FALSE;
    const GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *media = s ? gst_structure_get_string(s, "media") : nullptr;
    return media && g_str_equal(media, "application");
}

void OnvifMetaSource::poll_bus()
{
    if (!m_pipeline)
        return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    if (!bus)
        return;

    // 한 번에 몰아 비운다 — 남겨두면 버스가 계속 차오른다.
    while (GstMessage *msg = gst_bus_pop_filtered(
               bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
        QString what;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr;
            gst_message_parse_error(msg, &err, nullptr);
            what = err ? QString::fromUtf8(err->message) : QStringLiteral("?");
            if (err)
                g_error_free(err);
        } else {
            what = QStringLiteral("EOS");
        }
        // pop 한 메시지는 내가 unref 한다 (워치 콜백과 다른 규약).
        gst_message_unref(msg);
        gst_object_unref(bus);
        // schedule_restart()가 teardown 하므로 이 버스는 여기서 버린다.
        schedule_restart(what);
        return;
    }
    gst_object_unref(bus);
}

GstFlowReturn OnvifMetaSource::s_sample(GstAppSink *sink, gpointer user)
{
    // 스트리밍 스레드 — XML 복사만 하고 GUI로 던진다
    auto *self = static_cast<OnvifMetaSource *>(user);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_OK;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        QByteArray xml(reinterpret_cast<const char *>(map.data),
                       int(map.size));
        gst_buffer_unmap(buf, &map);
        DetectionFeed *feed = self->m_feed;
        const int ch = self->m_channel;
        // 첫 문서가 오면 세션이 자리 잡은 것 — 워치독 해제·재시도 리셋
        self->m_got_data = true;
        self->m_ever_had_data = true;
        self->m_retry_count = 0;
        QMetaObject::invokeMethod(
            feed, [feed, ch, xml] { feed->ingest_onvif(ch, xml); },
            Qt::QueuedConnection);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
