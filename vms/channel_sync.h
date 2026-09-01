#pragma once

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QSet>
#include <QStringList>

/**
 * @brief 채널 간 표시 시각 정렬기 (공유 프레젠테이션 시계)
 *
 * 4채널은 같은 카메라(=같은 시계)에서 나오지만 경로별 지연이 다르다.
 * 그대로 그리면 타일마다 다른 순간이 보인다. 여기서 전 채널의 실측 지연 중
 * 최댓값을 목표 지연 T로 잡고, 각 채널이 "촬영시각 + T"에 표시하도록 맞춘다.
 *
 * 대가: 가장 빠른 채널이 (T - 자기지연)만큼 느려진다. 정렬과 최저지연은
 * 양립하지 않으므로 sync_channels 설정으로 끌 수 있게 했다.
 *
 * 무리 이탈(outlier): 한 채널이 병적으로 느려지면(구 ch2 +500ms) 전원을
 * 거기 맞추는 대신 그 채널만 정렬에서 빼고 자기 최신 프레임을 표시한다.
 * 판정 기준은 "가장 빠른 채널 + sync_outlier_ms(기본 150)" — 히스테리시스로
 * 경계에서의 왕복을 막는다. 건강한 무리는 낮은 지연으로 정렬을 유지한다.
 *
 * **스레드:** 이 클래스는 여러 스레드가 동시에 두드린다.
 *  - report_latency() — 채널마다 **다른** GStreamer 스트리밍 스레드(appsink
 *    콜백)에서 채널당 초당 30회 가량. 4채널이면 쓰기 스레드가 4개다.
 *  - outlier() / target_latency_ms() — GUI 스레드가 표시 시계 tick마다.
 *  - unregister_channel() — GUI 스레드(백엔드 소멸·재연결).
 *
 * 공유 상태(m_reports/m_outliers/m_target_ms)는 전부 m_mutex 가 지킨다.
 * 락 유지 구간은 채널 수(4)에 비례하는 산술뿐이고 로그 출력은 락 밖으로
 * 빼놨으므로, 표시 경로가 이 락에서 기다리는 시간은 무시할 수 있다.
 * m_enabled·m_outlier_band_ms 는 생성자에서만 쓰고 이후 불변이라 락 없이 읽는다.
 */
class ChannelSync : public QObject
{
    Q_OBJECT

public:
    static ChannelSync *instance();

    /** @brief 정렬 사용 여부 (QSettings "sync_channels", 기본 true) */
    bool enabled() const { return m_enabled; }  // 생성 후 불변 — 락 불필요

    /** @brief 한 채널이 자기 실측 지연을 보고한다 (촬영→도착, ms) */
    void report_latency(int channel, qint64 latency_ms);

    /** @brief 채널이 사라질 때 — 죽은 채널이 목표치를 붙잡지 않게 */
    void unregister_channel(int channel);

    /**
     * @brief 공통 목표 지연 (ms). 정렬 꺼짐/데이터 없음이면 -1.
     *
     * 최근 보고된 채널들의 지연 중 최댓값 + 여유. 한 채널이 튀어도
     * 전체가 출렁이지 않도록 상승은 즉시, 하강은 완만하게 따라간다.
     */
    qint64 target_latency_ms() const;

    /**
     * @brief 이 채널이 무리 이탈 상태인가 — 이탈 채널은 정렬 없이 최신 표시
     *
     * 판정·해제는 recompute_locked()가 하고 여기서는 조회만 한다. 표시 시계
     * tick마다 불리므로 조회는 O(1)이어야 한다(락 획득 + 해시 조회).
     */
    bool outlier(int channel) const;

    /** @brief 정렬을 위해 보관해야 하는 최대 시간 (큐 용량 산정용) */
    static const int MAX_HOLD_MS = 700;

private:
    explicit ChannelSync(QObject *parent = nullptr);

    /**
     * @brief 목표 지연·이탈 판정 재계산. **호출자가 m_mutex 를 쥐고 있어야 한다.**
     *
     * 이탈/복귀 로그는 여기서 출력하지 않고 notes 에 모아 둔다. 락 안에서
     * qInfo() 를 때리면 로그 I/O 가 끝날 때까지 표시 스레드가 이 락에
     * 걸려 멈춘다 — 지연을 재는 물건이 지연을 만들면 곤란하다.
     */
    void recompute_locked(QStringList *notes);

    struct Report {
        qint64 latency_ms = 0;
        qint64 updated_ms = 0;
    };

    mutable QMutex m_mutex;
    QHash<int, Report> m_reports;   // m_mutex
    QSet<int> m_outliers;           // m_mutex
    qint64 m_target_ms = -1;        // m_mutex
    qint64 m_outlier_band_ms = 150; // 생성 후 불변
    bool m_enabled = true;          // 생성 후 불변
};
