#include "track_history.h"

#include <QDebug>
#include <QTimer>

#include <algorithm>
#include <cmath>

TrackHistory *TrackHistory::instance()
{
    static TrackHistory hist;
    return &hist;
}

TrackHistory::TrackHistory(QObject *parent)
    : QObject(parent)
{
    // add()마다 emit하면 폴링 1회당 수십 번이 되고 패널이 그만큼 재구성된다.
    // 변경 여부만 표시해두고 여기서 묶어 알린다.
    m_notify = new QTimer(this);
    connect(m_notify, &QTimer::timeout, this, [this] {
        if (!m_dirty)
            return;
        m_dirty = false;
        emit updated();
    });
    m_notify->start(NOTIFY_MS);
}

QString TrackHistory::label(const TrackId &id)
{
    return id.is_global() ? QString("G-%1").arg(id.global_id)
                          : QString("P-%1").arg(id.object_id);
}

void TrackHistory::add(int channel, const DetectionBox &box)
{
    if (!box.ts.isValid() || box.ex <= box.sx || box.ey <= box.sy)
        return;

    // 미래로 튄 시각 한 점이 저장소를 **영구 오염**시킨다. 두 갈래로 망가진다:
    //   ① m_clock 은 "본 적 있는 최대 시각"이라 되돌아오지 않는다 →
    //      prune()의 컷오프가 미래로 밀려 **나머지 동선이 전부 지워지고**,
    //      active()의 3초 창도 같이 밀려 살아있는 대상이 목록에서 사라진다.
    //   ② 그 트랙의 last().ts 가 미래가 되면 이후 정상 점이 아래
    //      `pt.ts <= last.ts` 에서 전부 기각된다 → 트랙 하나가 통째로 얼어붙는다.
    //
    // 입구(DetectionFeed)가 HTTP·ONVIF 양쪽을 이미 거르지만, 여기는 **모든**
    // 공급원이 통과하는 문이다. 새 입력 경로가 생길 때마다 같은 가드를 다시
    // 짜야 한다면 언젠가 빠뜨린다 — 실제로 08-10 리뷰 시점에 ONVIF 경로가
    // 빠져 있었다.
    //
    // 기준은 PC 시계다 — 여기 들어오는 점은 출처가 여럿이라(HTTP·ONVIF) 공통의
    // 카메라측 기준 시각이 없다. 그래서 허용치는 시계차를 통째로 흡수해야 하고
    // (08-10 실측: 이 현장 카메라가 PC보다 3.3초 앞선다),
    // DETECTION_PC_CLOCK_SKEW_MS 를 쓴다 — 근거는 box_source.h.
    // 여기서 걸린다면 그건 드리프트가 아니라 손상된 값이다.
    if (box.ts > QDateTime::currentDateTimeUtc().addMSecs(
                     DETECTION_PC_CLOCK_SKEW_MS)) {
        static qint64 s_warn_ms = 0;
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (now_ms - s_warn_ms > 10000) {
            s_warn_ms = now_ms;
            qWarning().noquote()
                << QString("[TrackHistory] ch%1 미래 시각 감지 (+%2ms) — 버림. "
                           "입구 가드를 지나온 공급원이 있다는 뜻이다")
                       .arg(channel)
                       .arg(box.ts.toMSecsSinceEpoch() - now_ms);
        }
        return;
    }

    // 아랫변이 프레임 바닥에 닿은 박스는 발밑을 모른다 — 근거는
    // FRAME_EDGE_MARGIN 주석. 좌우 잘림은 거르지 않는다: x 중앙만 흔들릴 뿐
    // ey 는 멀쩡하고, 채널을 넘나드는 순간이 정확히 좌우 경계라 여기서
    // 버리면 핸드오버 구간이 통째로 비어 버린다.
    if (box.ey >= FRAME_H - FRAME_EDGE_MARGIN)
        return;

    TrackId id;
    id.global_id = box.global_id;
    id.channel = channel;
    id.object_id = box.object_id;

    TrackPoint pt;
    pt.channel = channel;
    pt.object_id = box.object_id;
    pt.ts = box.ts;
    // 발밑(박스 하단 중앙)을 위치로 쓴다 — 평면도에 얹을 때 사람이 서 있는
    // 바닥 지점이 박스 중심보다 실제 위치에 가깝다.
    pt.cam = QPointF(double(box.sx + box.ex) / 2.0 / FRAME_W,
                     double(box.ey) / FRAME_H);

    QVector<TrackPoint> &path = m_tracks[id];
    if (!path.isEmpty()) {
        const TrackPoint &last = path.last();
        if (pt.ts <= last.ts)
            return;  // 같은 감지가 커서 겹침으로 다시 온 것
        const double dx = pt.cam.x() - last.cam.x();
        const double dy = pt.cam.y() - last.cam.y();
        const bool moved = std::hypot(dx, dy) >= SAMPLE_MIN_DIST;
        const bool aged = last.ts.msecsTo(pt.ts) >= SAMPLE_MIN_MS;
        const bool hopped = last.channel != pt.channel;
        if (!moved && !aged && !hopped) {
            // 점은 안 남기되 "아직 있다"는 사실은 갱신해야 active()에 남는다.
            // object_id도 함께 — global 트랙은 카메라가 재배정할 수 있다.
            path.last().ts = pt.ts;
            path.last().object_id = pt.object_id;
            m_dirty = true;
            return;
        }
    }

    path.append(pt);
    while (path.size() > MAX_POINTS)
        path.removeFirst();

    if (!m_clock.isValid() || pt.ts > m_clock)
        m_clock = pt.ts;

    m_dirty = true;
    prune();
}

void TrackHistory::clear()
{
    if (m_tracks.isEmpty())
        return;
    m_tracks.clear();
    emit updated();   // 패널이 즉시 빈 상태로 다시 그린다
}

void TrackHistory::prune()
{
    if (!m_clock.isValid())
        return;

    const QDateTime cutoff = m_clock.addMSecs(-PATH_TTL_MS);
    for (auto it = m_tracks.begin(); it != m_tracks.end();) {
        QVector<TrackPoint> &path = it.value();
        while (!path.isEmpty() && path.first().ts < cutoff)
            path.removeFirst();
        if (path.isEmpty())
            it = m_tracks.erase(it);
        else
            ++it;
    }
}

QVector<TrackPoint> TrackHistory::path(const TrackId &id) const
{
    return m_tracks.value(id);
}

QDateTime TrackHistory::first_seen(const TrackId &id) const
{
    const QVector<TrackPoint> path = m_tracks.value(id);
    return path.isEmpty() ? QDateTime() : path.first().ts;
}

QDateTime TrackHistory::last_seen(const TrackId &id) const
{
    const QVector<TrackPoint> path = m_tracks.value(id);
    return path.isEmpty() ? QDateTime() : path.last().ts;
}

int TrackHistory::current_channel(const TrackId &id) const
{
    const QVector<TrackPoint> path = m_tracks.value(id);
    return path.isEmpty() ? -1 : path.last().channel;
}

int TrackHistory::current_object_id(const TrackId &id) const
{
    const QVector<TrackPoint> path = m_tracks.value(id);
    return path.isEmpty() ? -1 : path.last().object_id;
}

QVector<TrackId> TrackHistory::active() const
{
    if (!m_clock.isValid())
        return {};

    const QDateTime cutoff = m_clock.addMSecs(-ACTIVE_TTL_MS);
    QVector<TrackId> ids;
    for (auto it = m_tracks.cbegin(); it != m_tracks.cend(); ++it)
        if (!it.value().isEmpty() && it.value().last().ts >= cutoff)
            ids.append(it.key());

    // 최근에 본 순 → 같으면 id 순 (QHash 순회 순서가 매번 달라 목록이 튄다)
    std::sort(ids.begin(), ids.end(), [this](const TrackId &a, const TrackId &b) {
        const QDateTime ta = m_tracks.value(a).last().ts;
        const QDateTime tb = m_tracks.value(b).last().ts;
        if (ta != tb)
            return ta > tb;
        if (a.channel != b.channel)
            return a.channel < b.channel;
        return a.object_id < b.object_id;
    });
    return ids;
}
