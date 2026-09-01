#pragma once

#include "video_backend.h"

#include <QImage>
#include <QThread>
#include <QUrl>

#include <atomic>

class SinkVideoWidget;  // qmediaplayer_backend.h — 범용 QImage 위젯 재사용

/**
 * @brief FFmpeg(libav) 디코드 스레드
 *
 * QMediaPlayer/GStreamer를 거치지 않고 libavformat(RTSP) + libavcodec으로
 * 직접 디코드한다. 버퍼링을 전부 우리가 통제해 지연을 최소화한다:
 *  - fflags=nobuffer, flags=low_delay, max_delay=0, reorder_queue_size=0
 *  - AV_CODEC_FLAG_LOW_DELAY, 프레임 스레딩 없음
 *  - "최신 프레임만" 표시 (drop, don't queue)
 */
class FFmpegWorker : public QThread
{
    Q_OBJECT

public:
    FFmpegWorker(int channel, QUrl url, QString transport, QObject *parent = nullptr);

    /**
     * @brief 중지를 요청하고 스레드가 끝날 때까지 최대 timeout_ms 기다린다
     * @return true = 스레드가 실제로 끝났다 (delete 해도 안전)
     *
     * 블로킹 중인 libav 호출은 AVIOInterruptCB 가 끊는다(s_interrupt) —
     * 소켓이 조용해도 다음 콜백 확인 시점에 av_read_frame 이 오류로 빠져나온다.
     * 그래서 정상 경로에선 거의 즉시 돌아온다. **timeout 을 넘겨도 terminate()
     * 는 하지 않는다** — 호출자가 소유권을 넘기고 물러나는 쪽을 택한다.
     */
    bool request_stop(int timeout_ms);

    qint64 last_pts_ms() const { return m_pts_ms.load(); }

    /** @brief UI가 직전 프레임을 소비함 — 다음 프레임 전달 허용 */
    void mark_consumed() { m_frame_pending = false; }

signals:
    /** @brief 새 프레임 (UI 스레드로 큐잉). QImage는 COW라 복사 저렴 */
    void frame_ready(const QImage &image);

    /** @brief 상태 문구 변경 (연결 중/재접속 등) */
    void status_changed(const QString &text);

protected:
    void run() override;

private:
    /** @brief RTSP 열기→디코드 1세션. 성공 반환 시 정상 종료, 실패 시 재접속 */
    bool run_session();

    /**
     * @brief libav 블로킹 I/O 중단 콜백 — m_stop 이 서면 1을 돌려 끊는다
     *
     * 이게 없으면 av_read_frame/avformat_open_input 안에서 소켓을 기다리는
     * 동안 m_stop 을 볼 방법이 없다. 카메라가 조용해지면 스레드가 몇 초씩
     * 안 끝나고, 그걸 terminate() 로 죽이면 libav 컨텍스트가 통째로 새고
     * (fmt/ctx/pkt/frame/sws) 최악엔 CRT 힙 락을 쥔 채 죽어 프로세스 전체가
     * 망가진다. 콜백은 libav 가 블로킹 중 주기적으로 부른다.
     */
    static int s_interrupt(void *opaque);

    /** @brief 프레임 도착 간격 통계 (부드러움) — GStreamer 경로와 동일 포맷 로그 */
    void track_smoothness(qint64 now_ms);

    int m_channel;
    QUrl m_url;
    QString m_transport;
    std::atomic<bool> m_stop{false};
    std::atomic<qint64> m_pts_ms{-1};

    // ---- drop 방식 상태 (원본 코드는 VMS/backup/ 참조) ----
    std::atomic<bool> m_frame_pending{false}; ///< UI가 아직 안 그린 프레임 존재
    qint64 m_base_pts_ms = -1;                ///< 지연 측정 기준 PTS
    qint64 m_base_wall_ms = 0;                ///< 기준 PTS가 도착한 실제 시각
    qint64 m_drop_late = 0;                   ///< 창 내 드롭 수 (지연 초과)
    qint64 m_drop_busy = 0;                   ///< 창 내 드롭 수 (UI 미소비)

    // 부드러움 측정
    qint64 m_sm_window_start_ms = 0;
    qint64 m_sm_prev_ms = 0;
    qint64 m_sm_max_gap = 0;
    int m_sm_count = 0;
    double m_sm_sum = 0.0;
    double m_sm_sum_sq = 0.0;
};

/**
 * @brief FFmpeg 디코드 백엔드 (QMediaPlayer 대체)
 *
 * VideoBackend 인터페이스를 그대로 구현하므로 ChannelView는 어느 백엔드인지
 * 몰라도 된다. QSettings video_backend = "ffmpeg" 로 선택.
 *
 * 참고: 렌더는 소프트웨어(sws_scale→QImage→QPainter)다. GStreamer 경로의
 * GPU 렌더보다 CPU를 더 쓰지만, 디코드 지연 통제가 목적인 첫 구현이다.
 * (HW 디코드 + 제로카피는 후속 과제)
 */
class FFmpegBackend : public VideoBackend
{
    Q_OBJECT

public:
    explicit FFmpegBackend(int channel, QObject *parent = nullptr);
    ~FFmpegBackend() override;

    QWidget *create_widget(QWidget *parent) override;
    void play(const QUrl &url) override;
    void stop() override;
    qint64 current_frame_pts_ms() const override;
    QString status_text() const override { return m_status; }
    bool supports_pixel_mosaic() const override { return true; }
    void set_mosaic_rects(const QVector<QRectF> &rects) override;

private:
    int m_channel = -1;
    SinkVideoWidget *m_widget = nullptr;  ///< 소유권은 부모 위젯
    FFmpegWorker *m_worker = nullptr;
    QString m_status;
};
