#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

#include "box_source.h"

class QTimer;

/**
 * @brief 추적 대상 하나의 식별자
 *
 * 동선은 global_id 기준이다 — 카메라를 넘어 같은 사람을 하나로 묶는 값은
 * 그것뿐이다. 다만 수신부가 아직 실어 보내지 않으므로, global_id == 0인
 * 동안은 (channel, object_id)로 대신 묶는다. object_id는 채널 안에서만
 * 고유하니 이 시기의 동선은 "한 채널 안의 궤적"이고, FLOOR MAP도 그 채널
 * 칸 하나에서만 그려진다.
 *
 * global_id가 실리기 시작하면 키가 저절로 global_id로 바뀌어 채널 간 동선이
 * 이어진다 — TrackingPanel은 손댈 필요가 없다.
 */
struct TrackId {
    int global_id = 0;   ///< 0 = 아직 배정 안 됨
    int channel = -1;    ///< global_id == 0일 때만 의미 있음
    int object_id = -1;

    bool is_global() const { return global_id != 0; }
    bool is_valid() const { return is_global() || object_id >= 0; }

    bool operator==(const TrackId &o) const
    {
        // 한쪽만 global이면 남남이다 (0 == global_id는 성립하지 않는다)
        if (is_global() || o.is_global())
            return global_id == o.global_id;
        return channel == o.channel && object_id == o.object_id;
    }
    bool operator!=(const TrackId &o) const { return !(*this == o); }
};

/** @brief QHash 키 사용 — operator==와 같은 분기를 타야 한다 */
inline size_t qHash(const TrackId &id, size_t seed = 0)
{
    return id.is_global() ? qHash(id.global_id, seed)
                          : qHashMulti(seed, id.channel, id.object_id);
}

/**
 * @brief 동선의 점 하나
 */
struct TrackPoint {
    int channel = -1;
    /**
     * 이 점을 만든 감지의 object_id.
     *
     * TrackId에도 있지만 그것은 QHash 키라 처음 삽입된 값에 고정된다.
     * global_id로 묶이는 트랙은 카메라가 object_id를 재배정해도 같은 키에
     * 들어오므로, 영상 타일의 어느 박스를 강조할지는 *마지막* 점의
     * object_id로 판단해야 한다.
     */
    int object_id = -1;
    QDateTime ts;      ///< 카메라 시계 기준 촬영 시각
    QPointF cam;       ///< 프레임 대비 0..1 (x = 박스 중앙, y = 박스 하단 = 발밑)
};

/**
 * @brief 대상별 이동 이력 — 동선 패널의 데이터 원천
 *
 * ChannelView가 이미 object_id별 샘플(m_tracks)을 들고 있지만 그것은
 * 재생 싱크용 버퍼라 4~5초면 잘라낸다. 동선은 분 단위로 봐야 하므로
 * 별도 저장소가 필요하다.
 *
 * DetectionFeed와 같은 싱글턴이다 — 감지는 전 채널 공용 엔드포인트 하나에서
 * 오고, 동선도 채널을 넘어 하나로 봐야 하기 때문이다.
 */
class TrackHistory : public QObject
{
    Q_OBJECT

public:
    static TrackHistory *instance();

    /**
     * @brief 감지 한 건 적재
     * @param channel 화면 채널 번호 (0..3) — 소비자가 전부 UI(평면도 칸,
     *                ChannelView 강조)라서 DB 채널이 아니라 화면 채널로 받는다
     */
    void add(int channel, const DetectionBox &box);

    /** @brief 시각 오름차순 동선 (없으면 빈 벡터) */
    QVector<TrackPoint> path(const TrackId &id) const;

    /** @brief 지금 화면에 보이는 대상들 — 최근에 본 순 */
    QVector<TrackId> active() const;

    QDateTime first_seen(const TrackId &id) const;
    QDateTime last_seen(const TrackId &id) const;

    /** @brief 마지막으로 잡힌 채널 (없으면 -1) */
    int current_channel(const TrackId &id) const;

    /** @brief 마지막으로 잡힌 object_id — 타일 박스 강조 대상 (없으면 -1) */
    int current_object_id(const TrackId &id) const;

    bool contains(const TrackId &id) const { return m_tracks.contains(id); }

    /** @brief 화면 표기용 — global은 "G-12", 아니면 "P-6461" */
    static QString label(const TrackId &id);

    /**
     * @brief 카메라 시계 (지금까지 본 가장 최신 ts)
     *
     * 단조 증가한다 — 뒤로 가지 않는다. 그래서 add()가 미래로 튄 점을
     * 걸러내야만 이 값이 신뢰할 수 있다(prune·active의 기준시각이다).
     */
    QDateTime clock() const { return m_clock; }

    /**
     * @brief 쌓인 궤적을 전부 버린다 (운영자의 "지우기")
     *
     * 화면에 남은 선과 Path Log 는 전부 이 저장소에서 나온다 — 선택만
     * 풀면 선은 사라져도 **다음에 같은 사람을 고르면 옛 궤적이 되살아난다.**
     * "지웠다"는 말이 그런 뜻이면 안 되므로 저장소를 비운다. 새 감지는
     * 곧바로 다시 쌓이기 시작한다(1초 안에 첫 점이 들어온다).
     */
    void clear();

signals:
    /** @brief 이력이 바뀜. add마다가 아니라 NOTIFY_MS로 묶어서 한 번 */
    void updated();

private:
    explicit TrackHistory(QObject *parent = nullptr);

    void prune();

    QHash<TrackId, QVector<TrackPoint>> m_tracks;
    QDateTime m_clock;
    bool m_dirty = false;
    QTimer *m_notify = nullptr;

    // 카메라 원본 해상도 — ChannelView::FRAME_W/H와 같아야 한다
    static const int FRAME_W = 2592;
    static const int FRAME_H = 1520;

    /// 200ms 폴링을 그대로 쌓으면 제자리 점이 수천 개가 된다. 시간이 벌어졌거나
    /// 눈에 보일 만큼 움직였을 때만 점을 남긴다.
    static const int SAMPLE_MIN_MS = 600;
    static constexpr double SAMPLE_MIN_DIST = 0.015;  ///< 프레임 폭의 1.5%

    /**
     * @brief 박스 아랫변이 프레임 바닥에 닿으면 그 점은 발밑이 아니다.
     *
     * 사람이 화면 아래로 걸어 나가면 박스가 경계에서 잘려 `ey` 가 실제 발이
     * 아니라 **잘린 자리**가 된다. 그 값은 실제보다 위(=멀리)를 가리키고,
     * 호모그래피는 지평선에 가까울수록 폭발하므로 평면도에서 몇 미터를 튄다.
     *
     * 캘리브레이션 도구가 이미 같은 판정을 한다
     * (`vms/tools/calib_auto_pedestrian.py`: `ok = fy < frame_h - 4`).
     * 런타임에도 같은 규칙을 적용한다 — 점 몇 개를 잃는 것이
     * 틀린 자리에 찍는 것보다 낫다.
     */
    static const int FRAME_EDGE_MARGIN = 4;

    static const int PATH_TTL_MS = 10 * 60 * 1000;  ///< 동선 보존
    static const int ACTIVE_TTL_MS = 3000;          ///< "화면에 있음" 판정
    static const int MAX_POINTS = 240;              ///< 대상당 점 상한
    static const int NOTIFY_MS = 250;
};
