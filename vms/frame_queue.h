#pragma once

#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>

#include <gst/gst.h>

#include "frame_trace.h"
#include "pipeline_stats.h"

/**
 * @brief 스트리밍 스레드 -> 렌더 스레드 프레임 전달 큐
 *
 * 기본 동작은 "최신 1장만"(mailbox)이다. 라이브 영상에서 프레임을 줄 세우면
 * 지연이 그대로 누적되기 때문 — drop, don't queue.
 *
 * 채널 싱크가 켜지면 얘기가 달라진다. 빠른 채널을 느린 채널에 맞춰
 * 늦게 표시해야 하므로, 그 차이(최대 SYNC_MAX_HOLD_MS)만큼은 보관해야 한다.
 * 그래서 capacity를 런타임에 바꿀 수 있게 했다.
 */
class FrameQueue
{
public:
    struct Entry {
        GstSample *sample = nullptr;
        qint64 capture_unix_ms = -1;  ///< RTCP NTP 기반 촬영 시각 (모르면 -1)
        qint64 arrival_ms = 0;        ///< appsink 도착 시각 (큐 대기 측정용)
        qint64 pts = -1;              ///< 프레임 식별자 (추적용)
    };

    /**
     * @brief 구간별 지연 통계
     *
     * 여기 두는 이유: 스트리밍 스레드와 UI 스레드가 **둘 다 이 큐를 들고 있는
     * 유일한 객체**다. 별도로 만들어 양쪽에 넘기면 배선만 늘어난다.
     */
    PipelineStats &stats() { return m_stats; }

    /** @brief 프레임 한 장 전 구간 추적 (stats 와 같은 이유로 여기 산다) */
    FrameTrace &trace() { return m_trace; }

    ~FrameQueue() { clear(); }

    /** @brief 보관 가능 프레임 수 (1 = 순수 mailbox) */
    void set_capacity(int frames)
    {
        QMutexLocker lock(&m_mutex);
        m_capacity = qBound(1, frames, 64);
        trim_locked("큐 용량 축소로 폐기");
    }

    /** @brief 소유권 이전. 용량 초과분은 가장 오래된 것부터 버린다 */
    void put(GstSample *sample, qint64 capture_unix_ms, qint64 pts = -1)
    {
        QMutexLocker lock(&m_mutex);
        m_entries.append({ sample, capture_unix_ms,
                           QDateTime::currentMSecsSinceEpoch(), pts });
        trim_locked("용량 초과 — 더 새 프레임에 밀림");
    }

    /**
     * @brief 표시할 프레임을 고른다 (ref 붙여서 반환 — 잠금 밖에서 사용)
     *
     * @param deadline_unix_ms 이 시각까지 촬영된 프레임만 표시 대상.
     *                         -1이면 싱크 없이 그냥 최신 프레임.
     *
     * 선택된 프레임보다 오래된 것은 버린다(따라잡기). 아직 이른 프레임은
     * 큐에 남겨 다음 tick에서 쓴다.
     */
    GstSample *take_for_display(qint64 deadline_unix_ms)
    {
        QMutexLocker lock(&m_mutex);
        if (m_entries.isEmpty())
            return m_last ? gst_sample_ref(m_last) : nullptr;

        int chosen = -1;
        if (deadline_unix_ms < 0) {
            chosen = m_entries.size() - 1;  // mailbox 모드: 무조건 최신
        } else {
            for (int i = 0; i < m_entries.size(); ++i) {
                const qint64 ts = m_entries[i].capture_unix_ms;
                // 촬영 시각을 모르는 프레임은 싱크 대상에서 제외하고 즉시 표시
                if (ts < 0 || ts <= deadline_unix_ms)
                    chosen = i;
                else
                    break;
            }
        }

        if (chosen < 0)
            return m_last ? gst_sample_ref(m_last) : nullptr;  // 아직 표시할 때가 아님

        // 선택분까지 소비: 이전 것들은 이미 늦었으므로 폐기
        for (int i = 0; i < chosen; ++i) {
            m_trace.discard(m_entries[i].pts,
                            "큐에서 폐기 — 더 새 프레임이 있어 따라잡기");
            gst_sample_unref(m_entries[i].sample);
        }
        GstSample *sample = m_entries[chosen].sample;

        // 큐 대기 = 도착 → 표시 선택. mailbox(용량 1)에서는 거의 0이고,
        // 채널 싱크를 켜면 느린 채널에 맞추느라 여기서 시간을 쓴다.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (const qint64 arrived = m_entries[chosen].arrival_ms; arrived > 0)
            m_stats.add(PipelineStats::Queue, now - arrived);
        m_trace.mark_dequeue(m_entries[chosen].pts, now);

        m_entries.remove(0, chosen + 1);

        if (m_last)
            gst_sample_unref(m_last);
        m_last = sample;  // 새 프레임이 없을 때 다시 그릴 수 있게 보관
        return gst_sample_ref(m_last);
    }

    /**
     * @brief 지금 표시할 프레임이 실제로 있는지 (소비하지 않고 확인)
     *
     * 싱크 표시 시계는 8ms마다 도는데, 매번 무조건 다시 그리면 15fps 스트림에
     * 대해 초당 125번을 그리게 된다. 실제로 표시할 프레임이 생겼을 때만
     * 그리도록 이 함수로 걸러낸다.
     */
    bool has_due(qint64 deadline_unix_ms)
    {
        QMutexLocker lock(&m_mutex);
        if (m_entries.isEmpty())
            return false;
        if (deadline_unix_ms < 0)
            return true;
        const qint64 ts = m_entries.first().capture_unix_ms;
        return ts < 0 || ts <= deadline_unix_ms;
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        for (Entry &e : m_entries)
            gst_sample_unref(e.sample);
        m_entries.clear();
        if (m_last) {
            gst_sample_unref(m_last);
            m_last = nullptr;
        }
    }

private:
    void trim_locked(const QString &why)
    {
        while (m_entries.size() > m_capacity) {
            m_trace.discard(m_entries.first().pts, why);
            gst_sample_unref(m_entries.first().sample);
            m_entries.removeFirst();
        }
    }

    QMutex m_mutex;
    QList<Entry> m_entries;
    GstSample *m_last = nullptr;  ///< 마지막 표시 프레임 (재도색용)
    int m_capacity = 1;
    PipelineStats m_stats;
    FrameTrace m_trace;
};
