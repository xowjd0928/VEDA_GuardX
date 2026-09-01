#pragma once

#include <QObject>
#include <QString>

#ifdef HAVE_GSTREAMER
#include <QMutex>
#include <QVector>
#include <gst/gst.h>
class QTimer;
#endif

/**
 * @brief 이벤트 클립 저장기 (싱글턴) — 화재·혼잡 critical 전후 15초 녹화
 *
 * "이벤트 **전** 15초"가 이 클래스의 존재 이유다: 사건이 난 뒤에 녹화를
 * 시작하면 이미 늦었으므로, 각 채널의 **압축 H.264 스트림을 상시 링버퍼**
 * (PRE_MS+여유)로 들고 있다가 트리거 순간 [T−15s, T+15s]를 MP4 로 굳힌다.
 * 재인코딩이 없다 — DirectSinkBackend 파이프라인의 h264parse 출력을 pad
 * probe 로 참조(ref)만 하므로 표시 경로에 지연·부하를 더하지 않는다
 * (압축 상태 20초 ≈ 채널당 수 MB).
 *
 * 트리거 (생성자에서 스스로 구독 — ChannelView 의 AlertFeed 패턴):
 *  - AlertFeed::alert_raised(ch, Critical) → 그 채널만
 *  - FireAlertFeed::state_changed(활성 전이) → 전 채널 (화재는 사이트 전역)
 *  - FireAlertFeed::button_pressed(비상 버튼) → 전 채널 (사람이 움직인다)
 *
 * 쓰기 파이프라인은 appsrc ! h264parse ! mp4mux(fragmented) ! filesink —
 * 조각 MP4 라 저장 중에 앱이 죽어도 그때까지 조각은 재생된다.
 *
 * 저장 위치는 QSettings "storage_dir" (SETTINGS ▸ Event recording 카드),
 * 기본은 Videos/GuardX.
 *
 * ⚠ 링 공급원은 direct 백엔드뿐이다 — video_backend 를 gstreamer(appsink)
 *   등으로 바꾼 실험 구성에서는 "no video buffered" 로 조용히 빠진다.
 * ⚠ 재접속은 PTS 축이 새로 시작하므로 링을 비운다(on_session_reset) —
 *   재접속 직후의 이벤트는 pre-roll 이 그만큼 짧다.
 */
class ClipRecorder : public QObject
{
    Q_OBJECT

public:
    static ClipRecorder *instance();

    static const int PRE_MS = 15000;    ///< 이벤트 앞쪽 녹화 구간
    static const int POST_MS = 15000;   ///< 이벤트 뒤쪽 녹화 구간

    /** @brief 클립 저장 폴더 — QSettings "storage_dir", 기본 Videos/GuardX */
    static QString storage_dir();
    static void set_storage_dir(const QString &dir);

    /** @brief 전 채널 클립 (화재·테스트) — label 은 파일명에 들어간다 */
    void trigger_all(const QString &label);

    /** @brief 한 채널 클립 (혼잡 critical). 진행 중이면 마감만 +POST_MS 연장 */
    void trigger_channel(int channel, const QString &label);

#ifdef HAVE_GSTREAMER
    /**
     * @brief 압축 AU 공급 — DirectSinkBackend 의 delayq 입구 probe 가 부른다
     *
     * ⚠ 스트리밍 스레드. 버퍼는 ref 만 하고(제로카피) 링에 쌓는다.
     *   caps 는 키프레임마다 pad 에서 갱신한다(쓰기 파이프라인의 appsrc 계약).
     */
    void ingest(int channel, GstPad *pad, GstBuffer *buf);

    /**
     * @brief 세션 재시작 — 링을 비운다 (GUI 스레드, start_pipeline 에서)
     *
     * 새 세션은 PTS 축이 새로 시작한다. 옛 축의 버퍼가 섞이면 MP4 타임라인이
     * 깨지므로 통째로 버리는 것이 맞다. 진행 중 클립은 그때까지 분량으로 마감.
     */
    void on_session_reset(int channel);
#endif

signals:
    /** @brief 클립 저장 완료 — path 는 최종 파일 경로 */
    void clip_saved(int channel, const QString &path);
    /** @brief 클립 실패 (채널 오프라인·디스크 등) — 이유는 사람이 읽는 문구 */
    void clip_failed(int channel, const QString &reason);

private:
    explicit ClipRecorder(QObject *parent = nullptr);

    /** @brief 결과 통보를 락 밖(GUI 큐)으로 미룬다 — 구현부 주석 참고 */
    void emit_result(int channel, bool ok, const QString &text);

    static const int NUM_CH = 4;

    bool m_fire_prev = false;   ///< 화재 활성 전이 검출용 (기동 중 화재는 제외)

#ifdef HAVE_GSTREAMER
    /** @brief 링 항목 하나 — 압축 AU (ref 보유) */
    struct Au {
        GstBuffer *buf;
        bool key;          ///< IDR (DELTA_UNIT 플래그 없음)
        qint64 wall_ms;    ///< 도착 벽시계 — 15초 창 선택은 이 축
    };
    struct Ring {
        QVector<Au> aus;
        GstCaps *caps = nullptr;   ///< 최근 협상 caps (appsrc 에 그대로)
    };
    /** @brief 진행 중 쓰기 작업 하나 (채널당 최대 1개) */
    struct Job {
        int channel = -1;
        QString path;
        GstElement *pipeline = nullptr;
        GstElement *appsrc = nullptr;
        qint64 base_ts = -1;        ///< 재기준 타임스탬프 (ns) — 클립은 0부터
        qint64 started_wall_ms = 0;
        qint64 deadline_wall_ms = 0;
        qint64 eos_wall_ms = 0;
        bool accepting = true;      ///< ingest 공급 여부
        bool eos_sent = false;
        int pushed = 0;
    };

    /** @brief 링 스냅샷으로 쓰기 작업 생성 (m_mutex 잡힌 채로 호출) */
    void start_job_locked(int channel, const QString &label, qint64 now_ms);
    /** @brief 버퍼 1개를 작업에 복사·재기준·push (m_mutex 잡힌 채로) */
    void push_au_locked(Job *job, GstBuffer *src);
    /** @brief 500ms 틱 — 마감 EOS·버스 폴링·타임아웃 (GUI 스레드) */
    void tick();
    void finalize_locked(Job *job, bool ok, const QString &reason);

    QMutex m_mutex;                 ///< 링·작업 목록 (스트리밍 스레드 대 GUI)
    Ring m_ring[NUM_CH];
    QVector<Job *> m_jobs;
    QTimer *m_tick = nullptr;       ///< 작업이 있는 동안만 돈다
#endif
};
