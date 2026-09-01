#include "clip_recorder.h"
#include "alert_feed.h"
#include "fire_alert_feed.h"

#include <QDateTime>
#include <QMetaObject>
#include <QDebug>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#ifdef HAVE_GSTREAMER
#include <QMutexLocker>
#include <QTimer>
#include <gst/app/gstappsrc.h>
#endif

// 링 유지량 = PRE_MS + 이 여유. 청소는 벽시계 컷오프로만 하고 키프레임
// 경계를 따로 지키지 않는다 — GOV=1s(CameraTuner 강제)라 여유 5초 안에는
// 반드시 키프레임이 남아 "T−15s 이전의 마지막 키프레임"이 항상 존재한다.
static const int RING_SLACK_MS = 5000;

// 한 클립의 총 길이 상한. critical 이 연달아 오면 마감을 연장하는데,
// 상한이 없으면 경보가 이어지는 내내 파일이 자란다.
static const int MAX_CLIP_MS = 120000;

// EOS 를 보낸 뒤 mp4mux 마무리를 기다리는 상한
static const int EOS_TIMEOUT_MS = 5000;

static const char *DIR_KEY = "storage_dir";

ClipRecorder *ClipRecorder::instance()
{
    // ⚠ 첫 호출은 반드시 GUI 스레드여야 한다 (QTimer·시그널 배선) —
    //   DirectSinkBackend 생성자가 보증한다 (probe 가 돌기 전에 생성됨)
    static ClipRecorder *inst = new ClipRecorder();
    return inst;
}

QString ClipRecorder::storage_dir()
{
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
        + "/GuardX";
    return QSettings("GuardX", "VMS").value(DIR_KEY, fallback).toString();
}

void ClipRecorder::set_storage_dir(const QString &dir)
{
    QSettings("GuardX", "VMS").setValue(DIR_KEY, dir);
}

ClipRecorder::ClipRecorder(QObject *parent)
    : QObject(parent)
{
    // 트리거는 스스로 구독한다 — 화면(LiveViewer)을 거치지 않아야 탭 구성이
    // 바뀌어도 녹화가 끊기지 않는다 (ChannelView 의 AlertFeed 패턴).
    connect(AlertFeed::instance(), &AlertFeed::alert_raised, this,
            [this](int ch, int sev) {
        // 장비 경보(DEV_*, 음수 키)는 영상 사건이 아니다 — 채널 경보만.
        if (ch >= 0 && ch < NUM_CH && sev == AlertFeed::Critical)
            trigger_channel(ch, QStringLiteral("crowd"));
    });

    // 기동 시점에 이미 화재가 열려 있으면 녹화하지 않는다 — 사건 시각이
    // 한참 전이라 "전후 15초"가 성립하지 않는다 (링도 아직 비어 있다).
    m_fire_prev = FireAlertFeed::instance()->state().active;
    connect(FireAlertFeed::instance(), &FireAlertFeed::state_changed, this,
            [this] {
        const bool active = FireAlertFeed::instance()->state().active;
        if (active && !m_fire_prev)
            trigger_all(QStringLiteral("fire"));   // 화재는 사이트 전역
        m_fire_prev = active;
    });

    // 비상 버튼 — "사람이 눌렀다"는 사건 (FireAlertPopup 과 같은 해석: 매
    // 눌림이 별개 사건이다 — 연타는 trigger 쪽 마감 연장이 파일 쪼개짐을
    // 막는다). zone→channel 매핑으로 한 채널만 고르지 않는다: 누른 사람이
    // 구역을 옮겨 다닐 수 있고 매핑 자체가 낡을 수 있어(zone_config.h 의
    // zone_id 주석) 화재처럼 전 채널을 굽는 쪽이 증거로 안전하다.
    connect(FireAlertFeed::instance(), &FireAlertFeed::button_pressed, this,
            [this](int, int, const QDateTime &) {
        trigger_all(QStringLiteral("button"));
    });

#ifdef HAVE_GSTREAMER
    m_tick = new QTimer(this);
    m_tick->setInterval(500);
    connect(m_tick, &QTimer::timeout, this, &ClipRecorder::tick);
#endif
}

/**
 * @brief 결과 통보를 **락 밖으로** 미룬다 (큐드 호출)
 *
 * ⚠ emit 을 m_mutex 안에서 하면, 직결(direct) 슬롯이 그 자리에서 실행된다 —
 *   그 슬롯이 ClipRecorder 를 다시 부르면 **비재귀 뮤텍스라 즉사**한다.
 *   게다가 스트리밍 스레드(ingest)가 기다리는 락을 슬롯 실행 시간만큼 더
 *   붙잡는다. 지금 붙어 있는 슬롯(SETTINGS 카드)은 그러지 않지만, 이건
 *   "지금 안전"일 뿐이라 구조로 막는다.
 */
void ClipRecorder::emit_result(int channel, bool ok, const QString &text)
{
    QMetaObject::invokeMethod(this, [this, channel, ok, text] {
        if (ok)
            emit clip_saved(channel, text);
        else
            emit clip_failed(channel, text);
    }, Qt::QueuedConnection);
}

void ClipRecorder::trigger_all(const QString &label)
{
    for (int ch = 0; ch < NUM_CH; ++ch)
        trigger_channel(ch, label);
}

#ifndef HAVE_GSTREAMER

void ClipRecorder::trigger_channel(int channel, const QString &label)
{
    // GStreamer 없는 빌드 — 표시 경로도 QMediaPlayer 뿐인 구성이라 압축
    // 스트림 탭 자체가 없다. 조용히 죽지 않고 이유를 남긴다.
    qWarning() << "[ClipRec] ch" << channel << label
               << "— GStreamer 없는 빌드라 이벤트 녹화 불가";
    emit_result(channel, false,
                QStringLiteral("recording unavailable in this build"));
}

#else

void ClipRecorder::trigger_channel(int channel, const QString &label)
{
    if (channel < 0 || channel >= NUM_CH)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QMutexLocker lock(&m_mutex);

    // 이미 이 채널을 굽는 중이면 마감만 민다 — 같은 사건의 연쇄 경보
    // (승급·재발행)마다 파일을 새로 만들면 같은 장면이 여러 파일로 쪼개진다.
    for (Job *job : m_jobs) {
        if (job->channel != channel || job->eos_sent)
            continue;
        const qint64 cap = job->started_wall_ms + MAX_CLIP_MS;
        job->deadline_wall_ms = qMin(now + POST_MS, cap);
        qInfo() << "[ClipRec] ch" << channel << "진행 중 — 마감 연장";
        return;
    }

    start_job_locked(channel, label, now);
}

void ClipRecorder::ingest(int channel, GstPad *pad, GstBuffer *buf)
{
    if (channel < 0 || channel >= NUM_CH || !buf)
        return;
    const bool key = !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QMutexLocker lock(&m_mutex);
    Ring &ring = m_ring[channel];

    // caps 는 키프레임마다 갱신 (~1Hz) — 세션 중 재협상도 따라간다
    if (key) {
        if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
            if (ring.caps)
                gst_caps_unref(ring.caps);
            ring.caps = caps;
        }
    }

    ring.aus.append({ gst_buffer_ref(buf), key, now });

    const qint64 cutoff = now - (PRE_MS + RING_SLACK_MS);
    while (!ring.aus.isEmpty() && ring.aus.first().wall_ms < cutoff) {
        gst_buffer_unref(ring.aus.first().buf);
        ring.aus.removeFirst();
    }

    // 진행 중 클립에 실시간 공급 (post-roll)
    for (Job *job : m_jobs)
        if (job->channel == channel && job->accepting)
            push_au_locked(job, buf);
}

void ClipRecorder::on_session_reset(int channel)
{
    if (channel < 0 || channel >= NUM_CH)
        return;
    QMutexLocker lock(&m_mutex);
    Ring &ring = m_ring[channel];
    for (const Au &au : ring.aus)
        gst_buffer_unref(au.buf);
    ring.aus.clear();
    if (ring.caps) {
        gst_caps_unref(ring.caps);
        ring.caps = nullptr;
    }
    // 진행 중 클립은 그때까지 분량으로 조기 마감 — 새 세션의 새 PTS 축을
    // 섞으면 타임라인이 깨진다. 마감을 지금으로 당기면 다음 틱이 EOS 한다.
    for (Job *job : m_jobs)
        if (job->channel == channel && !job->eos_sent) {
            job->accepting = false;
            job->deadline_wall_ms = 0;
        }
}

void ClipRecorder::start_job_locked(int channel, const QString &label,
                                    qint64 now_ms)
{
    Ring &ring = m_ring[channel];
    if (ring.aus.isEmpty() || !ring.caps) {
        qWarning() << "[ClipRec] ch" << channel << label
                   << "— 버퍼된 영상이 없음 (스트림 다운? direct 백엔드 아님?)";
        emit_result(channel, false, QStringLiteral("no video buffered"));
        return;
    }

    // 시작점 = T−15s 이전(같음 포함)의 마지막 키프레임. 링이 아직 어리면
    // (기동·재접속 직후) 첫 키프레임부터 — pre-roll 이 짧아질 뿐 실패는 아니다.
    const qint64 want = now_ms - PRE_MS;
    int start = -1;
    for (int i = 0; i < ring.aus.size(); ++i) {
        if (!ring.aus[i].key)
            continue;
        if (ring.aus[i].wall_ms <= want || start < 0)
            start = i;
        if (ring.aus[i].wall_ms > want && start >= 0)
            break;
    }
    if (start < 0) {
        emit_result(channel, false, QStringLiteral("no keyframe buffered"));
        return;
    }

    const QString dir = storage_dir();
    if (!QDir().mkpath(dir)) {
        qWarning() << "[ClipRec] 저장 폴더 생성 실패:" << dir;
        emit_result(channel, false,
                    QStringLiteral("cannot create folder %1").arg(dir));
        return;
    }
    const QString path = QString("%1/GuardX_%2_CH%3_%4.mp4")
                             .arg(dir, label)
                             .arg(channel + 1)
                             .arg(QDateTime::fromMSecsSinceEpoch(now_ms)
                                      .toString("yyyyMMdd-HHmmss"));

    // 경로·caps 는 파이프라인 문자열에 넣지 않는다 — Windows 역슬래시·한글
    // 폴더가 parse 규칙과 싸운다. 요소를 이름으로 찾아 API 로 준다.
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=asrc ! h264parse ! mp4mux fragment-duration=1000 "
        "! filesink name=fsink", &error);
    if (!pipeline) {
        const QString msg = error ? QString::fromUtf8(error->message) : "?";
        if (error)
            g_error_free(error);
        qWarning() << "[ClipRec] 쓰기 파이프라인 생성 실패:" << msg;
        emit_result(channel, false, msg);
        return;
    }
    if (error)
        g_error_free(error);

    GstElement *asrc = gst_bin_get_by_name(GST_BIN(pipeline), "asrc");
    GstElement *fsink = gst_bin_get_by_name(GST_BIN(pipeline), "fsink");
    // fragment-duration(조각 MP4)인 이유: 저장 도중 앱이 죽어도 그때까지의
    // 조각은 재생된다 — 일반 MP4 는 EOS 때 쓰는 moov 없이는 통째로 못 연다.
    g_object_set(fsink, "location",
                 QDir::toNativeSeparators(path).toUtf8().constData(), nullptr);
    g_object_set(asrc,
                 "format", GST_FORMAT_TIME,
                 "is-live", FALSE,
                 "block", FALSE,
                 // 백로그(15초치)를 한 번에 밀어 넣는다 — 기본 200KB 면 넘친다
                 "max-bytes", guint64(64) * 1024 * 1024,
                 nullptr);
    gst_app_src_set_caps(GST_APP_SRC(asrc), ring.caps);
    gst_object_unref(fsink);

    Job *job = new Job();
    job->channel = channel;
    job->path = path;
    job->pipeline = pipeline;
    job->appsrc = asrc;   // ref 는 job 이 보유 (finalize 에서 unref)
    job->started_wall_ms = now_ms;
    job->deadline_wall_ms = now_ms + POST_MS;

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    for (int i = start; i < ring.aus.size(); ++i)
        push_au_locked(job, ring.aus[i].buf);

    m_jobs.append(job);
    if (!m_tick->isActive())
        m_tick->start();

    qInfo().noquote() << QString("[ClipRec] ch %1 클립 시작 (%2) — pre %3 AU, "
                                 "저장: %4")
                             .arg(channel).arg(label)
                             .arg(ring.aus.size() - start).arg(path);
}

void ClipRecorder::push_au_locked(Job *job, GstBuffer *src)
{
    GstBuffer *copy = gst_buffer_copy(src);   // 메타만 복사, 데이터는 공유

    // 클립 타임라인은 0부터 — 첫 버퍼의 타임스탬프를 기준으로 전부 당긴다.
    // DTS 가 있으면 DTS 기준(더 이르다). 음수가 될 값은 0으로 눌러 언더플로
    // (unsigned) 를 막는다.
    const GstClockTime first = GST_BUFFER_DTS_IS_VALID(copy)
                                   ? GST_BUFFER_DTS(copy)
                                   : GST_BUFFER_PTS(copy);
    if (job->base_ts < 0 && GST_CLOCK_TIME_IS_VALID(first))
        job->base_ts = qint64(first);
    if (job->base_ts >= 0) {
        const GstClockTime base = GstClockTime(job->base_ts);
        if (GST_BUFFER_PTS_IS_VALID(copy))
            GST_BUFFER_PTS(copy) =
                GST_BUFFER_PTS(copy) > base ? GST_BUFFER_PTS(copy) - base : 0;
        if (GST_BUFFER_DTS_IS_VALID(copy))
            GST_BUFFER_DTS(copy) =
                GST_BUFFER_DTS(copy) > base ? GST_BUFFER_DTS(copy) - base : 0;
    }

    // 소유권은 push 가 가져간다. 실패 = 쓰기 파이프라인 사망 — 공급을 끊고
    // 다음 틱이 정리하게 한다.
    if (gst_app_src_push_buffer(GST_APP_SRC(job->appsrc), copy) != GST_FLOW_OK)
        job->accepting = false;
    else
        ++job->pushed;
}

void ClipRecorder::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker lock(&m_mutex);

    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        Job *job = m_jobs[i];

        // 버스 폴링 — GLib 메인루프가 없어 watch 콜백은 안 뜬다
        // (direct_sink_backend.h poll_bus 와 같은 이유)
        if (GstBus *bus = gst_element_get_bus(job->pipeline)) {
            while (GstMessage *msg = gst_bus_pop_filtered(
                       bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                    gst_message_unref(msg);
                    finalize_locked(job, true, QString());
                    job = nullptr;
                } else {
                    GError *err = nullptr;
                    gst_message_parse_error(msg, &err, nullptr);
                    const QString reason =
                        err ? QString::fromUtf8(err->message) : "?";
                    if (err)
                        g_error_free(err);
                    gst_message_unref(msg);
                    finalize_locked(job, false, reason);
                    job = nullptr;
                }
                break;
            }
            gst_object_unref(bus);
        }
        if (!job) {
            m_jobs.removeAt(i);
            continue;
        }

        if (!job->eos_sent
            && (now >= job->deadline_wall_ms || !job->accepting)) {
            job->accepting = false;
            job->eos_sent = true;
            job->eos_wall_ms = now;
            gst_app_src_end_of_stream(GST_APP_SRC(job->appsrc));
        }

        if (job->eos_sent && now - job->eos_wall_ms > EOS_TIMEOUT_MS) {
            // mp4mux 가 마무리를 못 했다 — 조각 MP4 라 그때까지 조각은 산다
            finalize_locked(job, false, QStringLiteral("EOS timeout"));
            m_jobs.removeAt(i);
        }
    }

    if (m_jobs.isEmpty())
        m_tick->stop();
}

void ClipRecorder::finalize_locked(Job *job, bool ok, const QString &reason)
{
    gst_element_set_state(job->pipeline, GST_STATE_NULL);   // 동기 — 파일 닫힘
    gst_object_unref(job->appsrc);
    gst_object_unref(job->pipeline);

    if (ok) {
        qInfo().noquote() << QString("[ClipRec] ch %1 클립 저장 (%2 AU): %3")
                                 .arg(job->channel).arg(job->pushed)
                                 .arg(job->path);
        emit_result(job->channel, true, job->path);
    } else {
        qWarning().noquote() << QString("[ClipRec] ch %1 클립 실패: %2 (%3)")
                                    .arg(job->channel).arg(reason)
                                    .arg(job->path);
        emit_result(job->channel, false, reason);
    }
    delete job;
}

#endif // HAVE_GSTREAMER
