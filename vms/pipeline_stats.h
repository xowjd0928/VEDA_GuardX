#pragma once

#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QVector>

#include <algorithm>
#include <limits>

/**
 * @brief 파이프라인 구간별 지연 통계 — 30초 창으로 집계해 한 번에 찍는다
 *
 * **왜 구간별인가**: 기존 로그는 프레임 도착 간격(maxgap/jitter)과
 * glass-to-display **총합**뿐이었다. 총 지연이 300ms라는 건 알아도 그게
 * 전송 때문인지 디코더 때문인지 우리 큐 때문인지 구분할 수 없어서,
 * 튜닝할 때마다 A/B를 새로 돌려야 했다. 구간을 쪼개 놓으면 로그 한 줄로
 * 범인이 드러난다.
 *
 * 구간 정의 (한 프레임이 지나는 순서):
 * ```
 *   촬영 ─Net─> 디코더입구 ─Decode─> appsink ─Queue─> 표시선택 ─Render─> 제출
 *   └────────────────────────── Total ──────────────────────────────────┘
 * ```
 *
 * 스레드: 스트리밍 스레드(Net·Decode)와 UI 스레드(Queue·Render)가 함께
 * 쓰므로 뮤텍스로 보호한다. 30fps × 5구간이면 창당 4,500 샘플 — 정렬
 * 비용은 30초에 한 번이라 무시할 수준이다.
 */
class PipelineStats
{
public:
    PipelineStats()
    {
        for (int i = 0; i < StageCount; ++i) {
            m_rej_min[i] = std::numeric_limits<qint64>::max();
            m_rej_max[i] = std::numeric_limits<qint64>::min();
        }
    }

    enum Stage {
        Net,      ///< 촬영 → 디코더 입구 (전송 + 지터버퍼 + depay)
        Decode,   ///< 디코더 입구 → appsink 도착
        Queue,    ///< appsink 도착 → 표시용으로 꺼내짐 (mailbox 대기)
        Render,   ///< 꺼냄 → 텍스처 업로드 + 드로 제출 (CPU 측)
        Total,    ///< 촬영 → 화면 제출
        StageCount
    };

    /**
     * @brief 표본 1개 추가. 범위 밖 값은 버리되 **버렸다는 사실을 남긴다**
     *
     * 예전엔 음수를 조용히 버렸다. 그 결과 net·TOTAL이 "표본 없음"으로만 찍혀
     * 계측이 아예 안 붙은 것처럼 보였고, 실제로는 카메라 시계가 PC보다
     * 1초 앞서서 전 표본이 음수였을 뿐이다(2026-07-31 실측). 이틀을 transport
     * 탓으로 헤맸다 — 버린 개수와 범위를 반드시 리포트에 남긴다.
     */
    void add(Stage s, qint64 ms)
    {
        if (s < 0 || s >= StageCount)
            return;
        QMutexLocker lock(&m_mutex);
        if (ms < 0 || ms > MAX_SANE_MS) {
            ++m_rejected[s];
            m_rej_min[s] = qMin(m_rej_min[s], ms);
            m_rej_max[s] = qMax(m_rej_max[s], ms);
            return;
        }
        m_samples[s].append(quint16(ms));
    }

    /**
     * @brief 카메라 시계 오프셋을 관측하고 보정된 전송 지연을 돌려준다
     *
     * 카메라와 PC의 NTP가 안 맞으면 (도착 − 촬영)이 음수가 된다. 이 현장
     * 카메라는 PC보다 약 1.04초 앞서 있어 모든 표본이 −1040ms대였다.
     *
     * 관측된 값의 **최솟값**을 오프셋으로 본다. 최솟값 = (시계차 + 그 창에서
     * 가장 빠른 프레임의 실제 전송지연)이므로, 이걸 빼고 남는 값은
     * "가장 빠른 프레임 대비 초과 지연"이다. 즉 보정 후 net·TOTAL은
     * **상대값**이다 — 절대 지연은 NTP를 맞춰야만 잴 수 있다. 그래도 어느
     * 구간이 튀는지, 시간에 따라 나빠지는지는 상대값으로 충분히 보인다.
     *
     * 기준을 net(전송 구간)에서 잡는 이유: 여기서 최솟값이 0이 되므로
     * net·decode·queue·render 의 합이 보정된 TOTAL 과 그대로 맞는다.
     * 총합(도착−촬영)에서 잡으면 가장 빠른 프레임의 net이 −decode 가 된다.
     *
     * @param raw_net_ms (appsink 도착 − 촬영) − 디코드시간, 시계 오프셋 포함
     */
    qint64 observe_net(qint64 raw_net_ms)
    {
        QMutexLocker lock(&m_mutex);
        m_off_win_min = qMin(m_off_win_min, raw_net_ms);
        // 하한이 내려가면 즉시 반영한다 — 안 그러면 그 프레임이 음수가 되어
        // 또 버려진다(고치려던 바로 그 증상).
        if (!m_off_valid || m_off_win_min < m_off) {
            m_off = m_off_win_min;
            m_off_valid = true;
        }
        return raw_net_ms - m_off;
    }

    /** @brief 이미 잡힌 기준으로 보정만 (TOTAL 처럼 다른 스레드가 재는 값) */
    qint64 apply_offset(qint64 raw_ms) const
    {
        QMutexLocker lock(&m_mutex);
        return m_off_valid ? raw_ms - m_off : raw_ms;
    }

    bool offset_known() const
    {
        QMutexLocker lock(&m_mutex);
        return m_off_valid;
    }

    /** @brief 현재 시계 오프셋 추정 (모르면 0) — 음수면 카메라가 앞선 것 */
    qint64 offset_ms() const
    {
        QMutexLocker lock(&m_mutex);
        return m_off_valid ? m_off : 0;
    }

    /**
     * @brief 창이 찼으면 리포트 문자열, 아니면 빈 문자열
     *
     * 첫 호출은 창 시작만 잡고 빈 값을 돌려준다 — 접속 직후 몇 프레임으로
     * 낸 통계는 워밍업이 섞여 오해를 부른다.
     */
    QString take_report(int channel, qint64 now_ms)
    {
        QMutexLocker lock(&m_mutex);
        if (m_window_start_ms == 0) {
            m_window_start_ms = now_ms;
            return {};
        }
        const qint64 span = now_ms - m_window_start_ms;
        if (span < WINDOW_MS)
            return {};

        const QString out = format_locked(channel, span);
        for (int i = 0; i < StageCount; ++i) {
            m_samples[i].clear();
            m_rejected[i] = 0;
            m_rej_min[i] = std::numeric_limits<qint64>::max();
            m_rej_max[i] = std::numeric_limits<qint64>::min();
        }
        // 창마다 시계 기준을 다시 잡아 드리프트를 따라간다. 표본이 없었으면
        // (스트림이 죽은 창) 직전 기준을 그대로 유지한다.
        if (m_off_win_min != std::numeric_limits<qint64>::max())
            m_off = m_off_win_min;
        m_off_win_min = std::numeric_limits<qint64>::max();
        m_window_start_ms = now_ms;
        return out;
    }

    /** @brief 재접속 시 초기화 — 끊긴 구간의 표본이 다음 창에 섞이지 않게 */
    void reset()
    {
        QMutexLocker lock(&m_mutex);
        for (int i = 0; i < StageCount; ++i) {
            m_samples[i].clear();
            m_rejected[i] = 0;
            m_rej_min[i] = std::numeric_limits<qint64>::max();
            m_rej_max[i] = std::numeric_limits<qint64>::min();
        }
        m_window_start_ms = 0;
        // 시계 오프셋은 유지한다 — 카메라 시계는 재접속으로 변하지 않고,
        // 버리면 재접속 직후 창이 또 통째로 음수가 되어 비어 버린다.
    }

    static constexpr qint64 WINDOW_MS = 30000;

private:
    static constexpr qint64 MAX_SANE_MS = 60000;

    struct Summary { int n = 0; int p50 = 0, p95 = 0, max = 0; };

    /** @brief 호출 전 잠금 필요 */
    Summary summarize_locked(Stage s) const
    {
        Summary r;
        QVector<quint16> v = m_samples[s];
        r.n = v.size();
        if (v.isEmpty())
            return r;
        std::sort(v.begin(), v.end());
        r.p50 = v[v.size() / 2];
        r.p95 = v[qMin(v.size() - 1, int(v.size() * 95 / 100))];
        r.max = v.last();
        return r;
    }

    QString format_locked(int channel, qint64 span_ms) const
    {
        static const char *names[StageCount] = {
            "net   ", "decode", "queue ", "render", "TOTAL "
        };

        Summary sum[StageCount];
        for (int i = 0; i < StageCount; ++i)
            sum[i] = summarize_locked(Stage(i));

        // 병목 = Total 을 뺀 구간 중 p50 이 가장 큰 것. p50 으로 고르는 이유는
        // max 는 한 번의 튐에도 흔들려 "평소 어디서 시간을 쓰는가"를 못 알려준다.
        int worst = -1;
        for (int i = 0; i < Total; ++i)
            if (sum[i].n > 0 && (worst < 0 || sum[i].p50 > sum[worst].p50))
                worst = i;

        // 비율의 분모: Total 이 있으면 그것, 없으면 잰 구간의 합.
        int denom = sum[Total].p50;
        if (denom <= 0) {
            denom = 0;
            for (int i = 0; i < Total; ++i)
                denom += sum[i].p50;
        }

        // 프레임 수는 표본이 있는 구간 중 최대치로 센다. Total 기준으로 세면
        // 한 구간만 비어도 "0프레임"이라는 거짓말이 된다.
        int frames = 0;
        for (int i = 0; i < StageCount; ++i)
            frames = qMax(frames, sum[i].n);
        const double fps = frames > 0 ? frames * 1000.0 / double(span_ms) : 0.0;

        QString s = QString("[Pipeline] ch%1 · %2초 · %3프레임 (%4 fps)")
                        .arg(channel)
                        .arg(span_ms / 1000)
                        .arg(frames)
                        .arg(fps, 0, 'f', 1);

        for (int i = 0; i < StageCount; ++i) {
            const QString name = QString::fromLatin1(names[i]);
            if (sum[i].n == 0) {
                s += QString("\n    %1  —  (표본 없음%2)")
                         .arg(name, reject_note_locked(Stage(i)));
                continue;
            }
            s += QString("\n    %1  p50 %2 ms   p95 %3   max %4")
                     .arg(name)
                     .arg(sum[i].p50, 4).arg(sum[i].p95, 4).arg(sum[i].max, 4);
            if (m_rejected[i] > 0)
                s += QString("   (%1개 버림)").arg(m_rejected[i]);
            if (i == worst && denom > 0) {
                s += QString("   <== 병목 (측정 구간의 %1%)")
                         .arg(qRound(100.0 * sum[i].p50 / denom));
            }
        }

        // 시계가 안 맞으면 net·TOTAL의 의미가 달라진다. 이걸 안 적으면
        // "지연 4ms"를 절대 지연으로 오해한다.
        if (m_off_valid && qAbs(m_off) > CLOCK_WARN_MS) {
            s += QString("\n    ※ 카메라 시계가 PC보다 %1 ms %2 — net·TOTAL은 "
                         "시계보정 후 **상대값**(가장 빠른 프레임 대비 초과분).")
                     .arg(qAbs(m_off))
                     .arg(m_off < 0 ? "앞섬" : "뒤짐");
            s += "\n      절대 지연이 필요하면 카메라·PC NTP 동기화가 선행 조건.";
        }
        if (!m_off_valid) {
            s += "\n    ! 촬영 시각(RTCP SR NTP) 미수신 — net·TOTAL 측정 불가.";
            s += "\n      확인: rtspsrc add-reference-timestamp-meta=true · "
                 "카메라 RTCP 송신 · 뷰어 PC Tailscale(UDP RTCP 바인딩)";
        }
        return s;
    }

    /** @brief "표본 없음" 뒤에 붙일 이유 — 왜 비었는지 추측하게 두지 않는다 */
    QString reject_note_locked(Stage s) const
    {
        if (m_rejected[s] == 0) {
            if (s == Decode)
                return QStringLiteral(", 디코더 프로브 미부착");
            return {};
        }
        return QString(", 범위 밖 %1개 버림 (%2 ~ %3 ms)")
            .arg(m_rejected[s]).arg(m_rej_min[s]).arg(m_rej_max[s]);
    }

    mutable QMutex m_mutex;
    QVector<quint16> m_samples[StageCount];
    qint64 m_window_start_ms = 0;

    // 버려진 표본 — 개수와 실제 범위 (원인 추적용)
    int m_rejected[StageCount] = {};
    qint64 m_rej_min[StageCount] = {};
    qint64 m_rej_max[StageCount] = {};

    // 카메라 시계 오프셋 추정 (관측 최솟값)
    static constexpr qint64 CLOCK_WARN_MS = 100;
    qint64 m_off = 0;
    qint64 m_off_win_min = std::numeric_limits<qint64>::max();
    bool m_off_valid = false;
};
