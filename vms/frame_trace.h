#pragma once

#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

/**
 * @brief 프레임 한 장을 촬영부터 화면 제출까지 끝까지 따라간다
 *
 * PipelineStats(30초 분포)와 역할이 다르다. 분포는 "p50이 몇 ms"는 알려주지만
 * **한 장이 실제로 지나간 길**은 못 보여준다. 구간 합이 총합과 안 맞을 때,
 * 어떤 구간이 통째로 비어 있을 때, 프레임이 화면에 못 갔을 때 —
 * 범인을 찾으려면 한 장을 끝까지 따라가는 수밖에 없다.
 *
 * 한 번에 한 장만 따라간다. 30fps × 4채널을 전부 찍으면 초당 120줄이라
 * 로그가 로그를 못 읽게 만든다. 기본은 5초에 한 장(설정 `trace_interval_ms`,
 * 0이면 끔).
 *
 * 스레드: begin()은 스트리밍 스레드, mark_*()/discard()는 UI 스레드에서
 * 온다. 자체 뮤텍스로 보호하고, 절대 밖으로 콜백하지 않는다(락 순서 문제 없음).
 */
class FrameTrace
{
public:
    void configure(int channel, int interval_ms)
    {
        QMutexLocker lock(&m_mutex);
        m_channel = channel;
        m_interval_ms = interval_ms;
    }

    /**
     * @brief 이 프레임을 추적 대상으로 잡는다 (appsink 도착 시점)
     * @return 잡았으면 true — 이후 mark_*()가 같은 pts로 불려야 완성된다
     *
     * appsink 시점엔 앞 단계 시각이 이미 다 알려져 있다(촬영=RTCP NTP,
     * 디코더 입구=패드 프로브). 그래서 세 단계를 한 번에 채운다.
     * 모르는 값은 -1로 넘기면 로그에 "미계측"으로 남는다.
     *
     * @param capture_ms 촬영 시각(카메라 시계, 모르면 -1)
     * @param offset_ms  카메라 시계 오프셋 — 촬영 시각을 PC 시계로 옮길 때 씀
     */
    bool begin(qint64 pts, qint64 capture_ms, qint64 dec_in_ms,
               qint64 arrival_ms, qint64 offset_ms)
    {
        QMutexLocker lock(&m_mutex);
        if (m_interval_ms <= 0)
            return false;

        if (m_armed) {
            // 따라가던 프레임이 화면에도 못 가고 폐기 신호도 못 받았다.
            // (파이프라인이 죽었거나 렌더가 멈춘 경우) 그 사실 자체가 정보다.
            if (arrival_ms - m_t[Arrival] < STUCK_MS)
                return false;
            finish_locked("미완료 — 렌더까지 도달 못 함 (렌더 정지/스트림 끊김)");
        }
        if (m_last_ms != 0 && arrival_ms - m_last_ms < m_interval_ms)
            return false;

        m_pts = pts;
        m_offset_ms = offset_ms;
        m_t[Capture] = capture_ms;
        m_t[DecIn] = dec_in_ms;
        m_t[Arrival] = arrival_ms;
        m_t[Dequeue] = -1;
        m_t[Rendered] = -1;
        m_armed = true;
        m_last_ms = arrival_ms;
        return true;
    }

    /** @brief 표시용으로 큐에서 꺼내진 시각 */
    void mark_dequeue(qint64 pts, qint64 t)
    {
        QMutexLocker lock(&m_mutex);
        if (m_armed && pts == m_pts)
            m_t[Dequeue] = t;
    }

    /** @brief 렌더 커맨드 제출 완료 시각 — 여기서 추적이 끝난다 */
    void mark_rendered(qint64 pts, qint64 t)
    {
        QMutexLocker lock(&m_mutex);
        if (m_armed && pts == m_pts) {
            m_t[Rendered] = t;
            finish_locked({});
        }
    }

    /** @brief 화면에 못 가고 버려짐 — 왜 버려졌는지가 지연만큼 중요하다 */
    void discard(qint64 pts, const QString &why)
    {
        QMutexLocker lock(&m_mutex);
        if (m_armed && pts == m_pts)
            finish_locked(why);
    }

    /** @brief 완성된 리포트를 가져간다 (없으면 빈 문자열). 호출자가 찍는다 */
    QString take_report()
    {
        QMutexLocker lock(&m_mutex);
        QString out;
        out.swap(m_pending);
        return out;
    }

    /** @brief 재접속 — 따라가던 프레임을 놓아준다 */
    void reset()
    {
        QMutexLocker lock(&m_mutex);
        m_armed = false;
        m_last_ms = 0;
        m_pending.clear();
    }

private:
    enum Step { Capture, DecIn, Arrival, Dequeue, Rendered, StepCount };

    /// 추적 중인 프레임이 이 시간 안에 끝나지 않으면 미완료로 접는다
    static constexpr qint64 STUCK_MS = 2000;

    static QString clock(qint64 unix_ms)
    {
        return QDateTime::fromMSecsSinceEpoch(unix_ms).toString("HH:mm:ss.zzz");
    }

    /** @brief 앞 단계와의 간격. 둘 중 하나라도 없으면 "—" */
    static QString delta(qint64 from, qint64 to)
    {
        if (from < 0 || to < 0)
            return QStringLiteral("      —");
        return QString("%1 ms").arg(to - from, 4);
    }

    /** @brief 호출 전 잠금 필요. 리포트를 만들고 슬롯을 놓아준다 */
    void finish_locked(const QString &abort_reason)
    {
        // 촬영 시각은 카메라 시계다. 오프셋을 더해 PC 시계로 옮겨야 다른
        // 로그(도착·렌더 시각)와 같은 축에서 읽힌다.
        const qint64 cap_pc =
            m_t[Capture] >= 0 ? m_t[Capture] + m_offset_ms : -1;

        QString s = QString("[Trace] ch%1 · pts %2s · 프레임 한 장 전 구간")
                        .arg(m_channel)
                        .arg(m_pts / 1000.0, 0, 'f', 3);
        if (!abort_reason.isEmpty())
            s += QString("  ⚠ %1").arg(abort_reason);

        if (cap_pc >= 0) {
            s += QString("\n    %1  촬영 (카메라 NTP%2)")
                     .arg(clock(cap_pc),
                          m_offset_ms != 0
                              ? QString(", 시계보정 %1 ms").arg(m_offset_ms)
                              : QString());
        } else {
            s += "\n    ──:──:──.───  촬영 (RTCP SR 미수신 — 촬영 시각 모름)";
        }

        struct Row { Step step; Step prev; const char *name; const char *what; };
        static const Row rows[] = {
            { DecIn,    Capture, "디코더 입구", "net     전송·지터버퍼·depay" },
            { Arrival,  DecIn,   "appsink 도착", "decode  디코더 체류" },
            { Dequeue,  Arrival, "표시 선택",   "queue   표시 대기(mailbox)" },
            { Rendered, Dequeue, "렌더 제출",   "render  텍스처 업로드+드로" },
        };
        for (const Row &r : rows) {
            if (m_t[r.step] < 0) {
                s += QString("\n    ──:──:──.───  %1 (미도달/미계측)   %2")
                         .arg(QString::fromUtf8(r.name).leftJustified(12),
                              QString::fromUtf8(r.what));
                continue;
            }
            s += QString("\n    %1  %2 %3   %4")
                     .arg(clock(m_t[r.step]),
                          QString::fromUtf8(r.name).leftJustified(12),
                          delta(m_t[r.prev], m_t[r.step]),
                          QString::fromUtf8(r.what));
        }

        if (cap_pc >= 0 && m_t[Rendered] >= 0) {
            s += QString("\n    = 촬영→화면 %1 ms (시계보정 기준 — 상대값)")
                     .arg(m_t[Rendered] - cap_pc);
        } else if (m_t[Arrival] >= 0 && m_t[Rendered] >= 0) {
            s += QString("\n    = 도착→화면 %1 ms")
                     .arg(m_t[Rendered] - m_t[Arrival]);
        }

        m_pending = s;
        m_armed = false;
    }

    mutable QMutex m_mutex;
    int m_channel = -1;
    int m_interval_ms = 0;

    bool m_armed = false;
    qint64 m_pts = -1;
    qint64 m_offset_ms = 0;
    qint64 m_t[StepCount] = { -1, -1, -1, -1, -1 };
    qint64 m_last_ms = 0;     ///< 마지막으로 추적을 **시작**한 시각
    QString m_pending;
};
