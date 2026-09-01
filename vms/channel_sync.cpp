#include "channel_sync.h"

#include <QDateTime>
#include <QDebug>
#include <QSettings>

#include <utility>

// 이 시간 동안 보고가 없는 채널은 계산에서 제외 (끊긴 채널이 목표를 붙잡지 않게)
static const qint64 REPORT_STALE_MS = 4000;

// 목표 지연에 더하는 여유 — 지터로 프레임이 살짝 늦어도 빈 tick이 안 생기게
static const qint64 SYNC_MARGIN_MS = 40;

ChannelSync *ChannelSync::instance()
{
    static ChannelSync sync;
    return &sync;
}

ChannelSync::ChannelSync(QObject *parent) : QObject(parent)
{
    QSettings settings("GuardX", "VMS");
    m_enabled = settings.value("sync_channels", true).toBool();
    // 무리 이탈 판정 폭 — 가장 빠른 채널 대비 이만큼 뒤지면 정렬에서 뺀다.
    // 150ms ≈ 4~5프레임: 정상 지터·홀드로는 안 넘고, 병든 채널만 걸린다.
    m_outlier_band_ms = settings.value("sync_outlier_ms", 150).toLongLong();
}

qint64 ChannelSync::target_latency_ms() const
{
    QMutexLocker lock(&m_mutex);
    return m_target_ms;
}

bool ChannelSync::outlier(int channel) const
{
    QMutexLocker lock(&m_mutex);
    return m_outliers.contains(channel);
}

void ChannelSync::report_latency(int channel, qint64 latency_ms)
{
    if (!m_enabled)
        return;

    QStringList notes;
    {
        QMutexLocker lock(&m_mutex);
        // 음수 지연 = 시계 어긋남. 정렬 기준으로 쓰면 목표가 망가지므로 0으로 본다.
        Report &report = m_reports[channel];
        report.latency_ms = qMax<qint64>(0, latency_ms);
        report.updated_ms = QDateTime::currentMSecsSinceEpoch();
        recompute_locked(&notes);
    }
    for (const QString &note : std::as_const(notes))
        qInfo().noquote() << note;
}

void ChannelSync::unregister_channel(int channel)
{
    QStringList notes;
    {
        QMutexLocker lock(&m_mutex);
        m_reports.remove(channel);
        m_outliers.remove(channel);
        recompute_locked(&notes);
    }
    for (const QString &note : std::as_const(notes))
        qInfo().noquote() << note;
}

void ChannelSync::recompute_locked(QStringList *notes)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 1) 무리의 기준점 = 살아있는 채널 중 최속 (최속은 정의상 이탈일 수 없다)
    qint64 best = -1;
    for (auto it = m_reports.cbegin(); it != m_reports.cend(); ++it) {
        if (now - it.value().updated_ms > REPORT_STALE_MS)
            continue;
        const qint64 latency = it.value().latency_ms;
        best = (best < 0) ? latency : qMin(best, latency);
    }

    if (best < 0) {
        m_target_ms = -1;
        return;
    }

    // 2) 무리 이탈 판정 — 병든 채널 하나가 벽 전체를 끌어내리지 않게.
    //    이전의 MAX_HOLD_MS(700) 상한은 스프레드 700ms까지 전원을 기다리게
    //    했다: 구 ch2(+500ms) 같은 채널이 다시 나타나면 모두가 반초 느려진다.
    //    이제 "최속 + band" 밖 채널은 정렬에서 빼고(자기 최신 프레임 표시),
    //    건강한 무리만 낮은 목표로 정렬한다. 복귀는 band의 70% 아래로 내려와야
    //    한다(히스테리시스) — 경계에 걸친 채널이 매 프레임 들락거리지 않게.
    qint64 worst_inlier = -1;
    for (auto it = m_reports.cbegin(); it != m_reports.cend(); ++it) {
        if (now - it.value().updated_ms > REPORT_STALE_MS)
            continue;
        const qint64 latency = it.value().latency_ms;
        const bool was_out = m_outliers.contains(it.key());
        const qint64 limit =
            best + (was_out ? m_outlier_band_ms * 7 / 10 : m_outlier_band_ms);
        if (latency > limit) {
            if (!was_out) {
                m_outliers.insert(it.key());
                *notes << QString("[ChannelSync] ch %1 정렬 이탈 — 지연 %2 ms가 "
                                  "무리(최속 %3 + %4 ms)를 벗어남. 이 채널만 "
                                  "비정렬(최신 프레임) 표시.")
                              .arg(it.key()).arg(latency).arg(best)
                              .arg(m_outlier_band_ms);
            }
        } else {
            if (was_out) {
                m_outliers.remove(it.key());
                *notes << QString("[ChannelSync] ch %1 정렬 복귀 — 지연 %2 ms.")
                              .arg(it.key()).arg(latency);
            }
            worst_inlier = qMax(worst_inlier, latency);
        }
    }

    // 최속 채널은 항상 무리 안이므로 worst_inlier ≥ best 가 보장된다
    const qint64 wanted = worst_inlier + SYNC_MARGIN_MS;
    if (m_target_ms < 0 || wanted > m_target_ms) {
        m_target_ms = wanted;  // 상승은 즉시 — 안 그러면 느린 채널이 계속 끊긴다
    } else {
        // 하강은 완만히. 순간적으로 좋아졌다고 목표를 확 낮추면
        // 다음 지터에서 다시 올라가며 화면이 출렁인다.
        m_target_ms = m_target_ms - (m_target_ms - wanted) / 8;
    }
}
