#include "ffmpeg_backend.h"
#include "qmediaplayer_backend.h"  // SinkVideoWidget 재사용

#include <QDateTime>
#include <QDebug>
#include <QSettings>

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// 재접속 백오프
static const int RECONNECT_BASE_MS = 500;
static const int RECONNECT_MAX_MS = 8000;

// ---- drop 방식 지연 상한 (QSettings로 조정, 원본 코드는 VMS/backup/) ----
// TCP는 대역 부족 시 유실 대신 밀림이 쌓인다. 두 단계로 자른다:
//  - render_lag: 이보다 밀린 프레임은 변환·표시 생략 (디코드는 참조 때문에 계속)
//  - reset_lag:  이보다 밀리면 재접속 — 서버측 송신 backlog까지 플러시
static const qint64 RENDER_LAG_DEFAULT_MS = 300;   // ≈ 30fps 9프레임
static const qint64 RESET_LAG_DEFAULT_MS = 5000;   // GStreamer 경로와 동일

// 시작 워밍업: 접속 직후엔 카메라가 직전 IDR부터 버스트로 몰아 보내
// 프레임 페이스가 실시간이 아니다. 이 구간을 기준으로 삼으면 이후 정상
// 프레임이 전부 "늦은 것"으로 보여 시작하자마자 드롭/재접속이 난다 —
// 워밍업 동안은 기준을 매 프레임 갱신하고 지연 판정을 하지 않는다.
static const qint64 WARMUP_DEFAULT_MS = 2000;

// ---------------------------------------------------------------- FFmpegWorker

FFmpegWorker::FFmpegWorker(int channel, QUrl url, QString transport, QObject *parent)
    : QThread(parent), m_channel(channel), m_url(std::move(url)),
      m_transport(std::move(transport))
{
    static bool net_inited = false;
    if (!net_inited) {
        avformat_network_init();
        net_inited = true;
    }
}

int FFmpegWorker::s_interrupt(void *opaque)
{
    auto *self = static_cast<FFmpegWorker *>(opaque);
    return (self && self->m_stop.load()) ? 1 : 0;
}

bool FFmpegWorker::request_stop(int timeout_ms)
{
    m_stop = true;
    return wait(timeout_ms);
}

void FFmpegWorker::track_smoothness(qint64 now_ms)
{
    if (m_sm_prev_ms > 0) {
        const qint64 gap = now_ms - m_sm_prev_ms;
        ++m_sm_count;
        m_sm_sum += double(gap);
        m_sm_sum_sq += double(gap) * double(gap);
        m_sm_max_gap = qMax(m_sm_max_gap, gap);
    }
    m_sm_prev_ms = now_ms;
    if (m_sm_window_start_ms == 0)
        m_sm_window_start_ms = now_ms;

    const qint64 win = now_ms - m_sm_window_start_ms;
    if (win >= 5000 && m_sm_count > 1) {
        const double mean = m_sm_sum / m_sm_count;
        const double var = m_sm_sum_sq / m_sm_count - mean * mean;
        const double jitter = var > 0.0 ? std::sqrt(var) : 0.0;
        const double fps = m_sm_count * 1000.0 / double(win);
        qDebug().noquote()
            << QString("[FFmpegBackend] ch %1 smoothness: fps %2 jitter %3 ms "
                       "maxgap %4 ms | drop late %5 busy %6")
                   .arg(m_channel).arg(fps, 0, 'f', 1)
                   .arg(qRound(jitter)).arg(m_sm_max_gap)
                   .arg(m_drop_late).arg(m_drop_busy);
        m_sm_window_start_ms = now_ms;
        m_sm_count = 0;
        m_sm_sum = 0.0;
        m_sm_sum_sq = 0.0;
        m_sm_max_gap = 0;
        m_drop_late = 0;
        m_drop_busy = 0;
    }
}

void FFmpegWorker::run()
{
    int retry = 0;
    while (!m_stop) {
        emit status_changed(retry == 0 ? QStringLiteral("connecting...")
                                       : QString("stream lost - reconnecting (%1)").arg(retry));
        const bool ok = run_session();
        if (m_stop)
            break;
        // 세션 종료(오류/EOF) — 지수 백오프 후 재접속
        ++retry;
        const int delay = qMin(RECONNECT_BASE_MS << qMin(retry, 4), RECONNECT_MAX_MS);
        Q_UNUSED(ok);
        for (int slept = 0; slept < delay && !m_stop; slept += 50)
            msleep(50);
    }
}

bool FFmpegWorker::run_session()
{
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt)
        return false;

    // 블로킹 I/O 중단 경로 — **avformat_open_input 前에** 걸어야 열기 단계의
    // 블로킹까지 끊을 수 있다. 이게 있어야 stop 이 terminate() 없이 끝난다.
    fmt->interrupt_callback.callback = &FFmpegWorker::s_interrupt;
    fmt->interrupt_callback.opaque = this;

    // ---- 저지연 데뮉스 옵션 ----
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", m_transport.toUtf8().constData(), 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);      // 분석용 입력 버퍼링 금지
    av_dict_set(&opts, "flags", "low_delay", 0);
    av_dict_set(&opts, "max_delay", "0", 0);
    av_dict_set(&opts, "reorder_queue_size", "0", 0); // 재정렬 대기 없음(B프레임 없음)
    av_dict_set(&opts, "probesize", "100000", 0);
    av_dict_set(&opts, "analyzeduration", "0", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);     // 5s 소켓 타임아웃(us)

    const QByteArray url_utf8 = m_url.toString(QUrl::FullyEncoded).toUtf8();
    if (avformat_open_input(&fmt, url_utf8.constData(), nullptr, &opts) < 0) {
        av_dict_free(&opts);
        avformat_free_context(fmt);
        qWarning() << "[FFmpegBackend] ch" << m_channel << "RTSP 열기 실패";
        return false;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    const AVCodec *codec = nullptr;
    const int vstream =
        av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (vstream < 0 || !codec) {
        avformat_close_input(&fmt);
        return false;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {                       // 할당 실패 시 아래에서 널 역참조로 죽는다
        avformat_close_input(&fmt);
        return false;
    }
    avcodec_parameters_to_context(ctx, fmt->streams[vstream]->codecpar);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;   // 프레임을 지체 없이 출력
    ctx->thread_count = 1;                   // 프레임 스레딩 금지(스레드당 1프레임 지연)
    ctx->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    SwsContext *sws = nullptr;
    int sws_w = 0, sws_h = 0, sws_fmt = -1;
    QImage rgb;  // 재사용 버퍼(디코드 스레드 소유; emit 시 copy)

    // 지연 상한/워밍업 (레지스트리로 조정 가능)
    QSettings lag_settings("GuardX", "VMS");
    const qint64 render_lag_ms =
        lag_settings.value("ffmpeg_render_lag_ms", RENDER_LAG_DEFAULT_MS).toLongLong();
    const qint64 reset_lag_ms =
        lag_settings.value("ffmpeg_reset_lag_ms", RESET_LAG_DEFAULT_MS).toLongLong();
    const qint64 warmup_ms =
        lag_settings.value("ffmpeg_warmup_ms", WARMUP_DEFAULT_MS).toLongLong();
    qint64 warmup_until_ms = 0;  // 첫 프레임 도착 시점 + warmup_ms

    // 세션 시작마다 드롭 상태 리셋 (재접속 = 지연 기준·워밍업도 재설정)
    m_frame_pending = false;
    m_base_pts_ms = -1;
    m_base_wall_ms = 0;
    m_drop_late = 0;
    m_drop_busy = 0;

    bool got_any = false;
    bool flush_restart = false;
    while (!m_stop && !flush_restart) {
        const int rc = av_read_frame(fmt, pkt);
        if (rc < 0)
            break;  // EOF/오류 → 재접속

        if (pkt->stream_index != vstream) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(ctx, pkt) == 0) {
            while (avcodec_receive_frame(ctx, frame) == 0) {
                const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

                // ---- 지연 측정: PTS 진행 속도 vs 실제 시간 ----
                // 첫 프레임을 기준점으로 삼고, 이후 프레임이 기준 대비 얼마나
                // 늦게 도착하는지 본다. 카메라 시계와 무관한 상대 측정.
                qint64 lag = 0;
                if (frame->pts != AV_NOPTS_VALUE) {
                    const AVRational tb = fmt->streams[vstream]->time_base;
                    const qint64 pts_ms =
                        qint64(frame->pts * 1000 * tb.num / tb.den);
                    m_pts_ms = pts_ms;

                    if (m_base_pts_ms < 0 || pts_ms < m_base_pts_ms) {
                        m_base_pts_ms = pts_ms;   // 첫 프레임/PTS 불연속 → 재기준
                        m_base_wall_ms = now_ms;
                        if (warmup_until_ms == 0)
                            warmup_until_ms = now_ms + warmup_ms;
                    }

                    if (now_ms < warmup_until_ms) {
                        // 워밍업: 시작 버스트 구간 — 기준만 따라가고 판정 없음
                        m_base_pts_ms = pts_ms;
                        m_base_wall_ms = now_ms;
                        lag = 0;
                    } else {
                        lag = now_ms
                              - (m_base_wall_ms + (pts_ms - m_base_pts_ms));
                        if (lag < 0) {
                            // 기준보다 이르게 도착 = 기준이 늦었던 것 — 앞당긴다
                            m_base_wall_ms += lag;
                            lag = 0;
                        }
                    }
                }

                if (lag > reset_lag_ms) {
                    // 회복 불가 수준 — 재접속으로 서버측 backlog까지 버린다
                    qWarning().noquote()
                        << QString("[FFmpegBackend] ch %1 지연 누적 %2ms — "
                                   "재접속으로 플러시").arg(m_channel).arg(lag);
                    flush_restart = true;
                    av_frame_unref(frame);
                    break;
                }

                if (lag > render_lag_ms) {
                    // 이미 낡은 프레임 — 변환·표시 생략 (참조용 디코드만 유지)
                    ++m_drop_late;
                    av_frame_unref(frame);
                    continue;
                }
                if (m_frame_pending.load()) {
                    // UI가 직전 프레임을 아직 안 그렸다 — 이번 것은 버린다.
                    // (이 검사가 없으면 UI 이벤트 큐에 프레임이 쌓여
                    //  시간이 갈수록 지연·메모리가 자라는 병이 생긴다)
                    ++m_drop_busy;
                    av_frame_unref(frame);
                    continue;
                }

                // sws 컨텍스트 (해상도/포맷 변경 시 재생성)
                if (!sws || sws_w != frame->width || sws_h != frame->height
                    || sws_fmt != frame->format) {
                    if (sws)
                        sws_freeContext(sws);
                    sws = sws_getContext(frame->width, frame->height,
                                         AVPixelFormat(frame->format),
                                         frame->width, frame->height,
                                         AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
                    sws_w = frame->width;
                    sws_h = frame->height;
                    sws_fmt = frame->format;
                    rgb = QImage(frame->width, frame->height, QImage::Format_RGB32);
                }

                if (sws) {
                    uint8_t *dst[1] = { rgb.bits() };
                    int dst_stride[1] = { int(rgb.bytesPerLine()) };
                    sws_scale(sws, frame->data, frame->linesize, 0,
                              frame->height, dst, dst_stride);

                    if (frame->pts != AV_NOPTS_VALUE) {
                        const AVRational tb = fmt->streams[vstream]->time_base;
                        m_pts_ms = qint64(frame->pts * 1000 * tb.num / tb.den);
                    }

                    // UI가 mark_consumed() 할 때까지 다음 emit은 막힌다 (drop 방식)
                    m_frame_pending = true;
                    emit frame_ready(rgb.copy());

                    if (!got_any) {
                        got_any = true;
                        emit status_changed(QString());  // 정상화
                    }
                    track_smoothness(now_ms);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }

    if (sws)
        sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return got_any;
}

// --------------------------------------------------------------- FFmpegBackend

FFmpegBackend::FFmpegBackend(int channel, QObject *parent)
    : VideoBackend(parent), m_channel(channel)
{
}

FFmpegBackend::~FFmpegBackend()
{
    stop();
}

QWidget *FFmpegBackend::create_widget(QWidget *parent)
{
    m_widget = new SinkVideoWidget(parent);
    return m_widget;
}

void FFmpegBackend::set_mosaic_rects(const QVector<QRectF> &rects)
{
    if (m_widget)
        m_widget->set_mosaic_rects(rects);
}

void FFmpegBackend::play(const QUrl &url)
{
    stop();

    // 전송 기본값은 video_backend.h 의 rtsp_transport() 한 곳에만 있다
    m_worker = new FFmpegWorker(m_channel, url, ::rtsp_transport());

    connect(m_worker, &FFmpegWorker::frame_ready, this,
            [this](const QImage &img) {
                if (m_widget)
                    m_widget->set_image(img);
                if (m_worker)
                    m_worker->mark_consumed();  // 다음 프레임 전달 허용
            }, Qt::QueuedConnection);

    connect(m_worker, &FFmpegWorker::status_changed, this,
            [this](const QString &s) {
                if (m_status != s) {
                    m_status = s;
                    emit status_changed();
                }
            }, Qt::QueuedConnection);

    m_worker->start();
    emit session_started();   // 상위가 키프레임을 강제한다 (검은 화면 단축)
}

void FFmpegBackend::stop()
{
    if (m_worker) {
        // 죽어가는 워커의 늦은 frame_ready 가 새 세션 화면을 덮지 않게 먼저 끊는다
        m_worker->disconnect(this);

        // 인터럽트 콜백(s_interrupt) 덕에 보통 수십 ms 안에 끝난다.
        // 2초는 병적인 경우까지 덮는 상한 — GUI 스레드가 여기서 멈추므로
        // 예전의 3+1초처럼 길게 잡지 않는다.
        if (m_worker->request_stop(2000)) {
            delete m_worker;
        } else {
            // ⚠ 여기서 terminate() 하지 않는다. 강제 종료는 정리 코드를 통째로
            // 건너뛰어 libav 컨텍스트(fmt/ctx/pkt/frame/sws)를 전부 누수시키고,
            // 최악엔 CRT 힙 락을 쥔 채 죽어 **프로세스 전체**를 망가뜨린다.
            // 소유권을 스레드 자신에게 넘기고 물러난다 — 끝나면 스스로 지워진다.
            qWarning() << "[FFmpegBackend] ch" << m_channel
                       << "워커가 2초 안에 안 끝남 — 뒤에서 정리되게 두고 진행";
            connect(m_worker, &QThread::finished,
                    m_worker, &QObject::deleteLater);
        }
        m_worker = nullptr;
    }
    m_status.clear();
}

qint64 FFmpegBackend::current_frame_pts_ms() const
{
    return m_worker ? m_worker->last_pts_ms() : -1;
}
