#include "zone_sensor_store.h"
#include "fire_zone_map.h"
#include "mqtt_link.h"
#include "sensor_fields.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

namespace {

const QString SENSORS_TOPIC = "guardx/db/rpib/sensors";
const QString SENSOR_HISTORY_TOPIC = "guardx/db/rpib/query/sensor_history";
const QString FIRE_TOPIC = "guardx/db/rpib/fire_threshold";
const int QOS = 1;
const int HISTORY_BACKFILL_MIN = 5;   ///< 롤링 창(HISTORY_WINDOW_MS)과 맞춤

/// spark_raw 그래프 평활화 계수. 작을수록 매끄럽고 느리게 반응
constexpr double EMA_ALPHA = 0.25;

} // namespace

ZoneSensorStore *ZoneSensorStore::instance()
{
    static ZoneSensorStore *store = new ZoneSensorStore;
    return store;
}

ZoneSensorStore::ZoneSensorStore(QObject *parent) : QObject(parent)
{
    // zone 목록은 fire_zone_map.h가 출처 — 실데이터든 더미든 여기 다 들어온다.
    // 미리 만들어 두는 이유: 아직 한 번도 값이 안 온 zone도 화면에 "수신 대기"로
    // 보여야 하기 때문이다(존재하지 않는 zone과 구분되어야 한다).
    int n = 0;
    const FireZoneInfo *zones = fire_zone_table(&n);
    bool any_dummy = false;
    for (int i = 0; i < n; ++i) {
        Snapshot s;
        s.zone_id = zones[i].zone_id;
        s.zone_name = QString("Zone %1").arg(zones[i].zone_id);   // DB 값이 오면 덮임
        m_zones.insert(s.zone_id, s);
        any_dummy = any_dummy || zones[i].dummy;
    }

    MqttLink *link = MqttLink::instance();
    link->subscribe(SENSORS_TOPIC,
                    [this](const QByteArray &p) { on_sensors(p); }, QOS);
    // SETTINGS 화면과 동일 토픽 — retained라 구독만으로 현재 THRESHOLD를 받고,
    // SETTINGS에서 값이 바뀌면 자동으로 다시 온다.
    link->subscribe(FIRE_TOPIC,
                    [this](const QByteArray &p) { on_fire_threshold(p); }, QOS);

    // 그래프 초기 백필 — **브로커가 붙은 뒤에** 요청한다.
    //
    // 예전엔 여기(생성자)에서 바로 request() 를 했다. 그런데 이 객체는
    // MqttLink::start() 보다 먼저 만들어지므로 그 시점엔 브로커가 **확정적으로**
    // 오프라인이고, 요청은 매 실행 "브로커 미연결"로 실패했다. 재시도 경로도
    // 없어서 on_sensor_history 는 사실상 죽은 코드였고 그래프는 항상 빈 채로
    // 시작했다 (2026-08-10 수정).
    connect(link, &MqttLink::online_changed, this, [this](bool online) {
        if (online)
            request_backfill();
    });
    if (link->online())        // 이미 붙어 있는 경우(재생성 등)
        request_backfill();

    if (any_dummy) {
        m_dummy_timer = new QTimer(this);
        connect(m_dummy_timer, &QTimer::timeout, this, &ZoneSensorStore::tick_dummy);
        m_dummy_timer->start(1000);   // 실제 센서 발행 주기(1Hz)와 동일하게
        tick_dummy();                 // 첫 화면이 비어 보이지 않게 즉시 1회
    }

    qInfo() << "[ZoneSensorStore] 구독 등록 —" << SENSORS_TOPIC << "+" << FIRE_TOPIC;
}

ZoneSensorStore::Snapshot ZoneSensorStore::snapshot(int zone_id) const
{
    return m_zones.value(zone_id);
}

ZoneSensorStore::NodeAHealth ZoneSensorStore::node_a_health() const
{
    NodeAHealth h;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // ⚠ **더미 zone 은 세지 않는다** — tick_dummy 가 가짜 값의 last_ms 를 매초
    //   현재시각으로 찍기 때문에, 그것까지 세면 하드웨어가 죽어도 영원히
    //   초록이다 (2026-08-10 실사고, 두 지시기가 서로 다른 답을 냈던 원인).
    int n = 0;
    const FireZoneInfo *zones = fire_zone_table(&n);
    for (int i = 0; i < n; ++i) {
        if (zones[i].dummy)
            continue;
        const Snapshot &s = m_zones[zones[i].zone_id];
        if (s.last_ms == 0)
            continue;   // 아직 한 번도 안 옴 = "무응답"이 아니라 "대기 중"
        h.seen = true;
        const qint64 age = now - s.last_ms;
        if (h.age_ms < 0 || age < h.age_ms)
            h.age_ms = age;         // 가장 최근에 온 zone 기준
        h.streak = qMax(h.streak, s.fresh_streak);
    }

    // 초록 = 최근 값이 신선하고(≤STALE_MS) **연속으로** 들어오는 중.
    // 두 조건 다 필요하다: 신선도만 보면 재발행 한 방에 5초 초록이 되고,
    // 연속 수만 보면 잘 오다가 죽은 직후가 초록으로 남는다.
    h.streaming =
        h.seen && h.age_ms >= 0 && h.age_ms <= STALE_MS && h.streak >= STREAM_STREAK;
    return h;
}

QVector<ZoneSensorStore::Point> ZoneSensorStore::history(
    int zone_id, const QString &channel_key) const
{
    return m_history.value(zone_id).value(channel_key);
}

// ---------------------------------------------------------------- 실데이터

/*
 * payload (PHASE 7):
 *   {"node_id":"rpib","timestamp":<ms>,
 *    "zones":[{"zone_id":1,"zone_name":"1구역","sensor_seq":812,
 *              "composite_score":12.4,
 *              "channels":{"gas_raw":{"value":530,"is_valid":true}, ...}}]}
 *
 * 값이 아직 없는 zone도 행은 온다(전부 null) — has_data=false로 남겨서
 * 화면이 "존재하지만 수신 대기"를 표시할 수 있게 한다.
 */
void ZoneSensorStore::on_sensors(const QByteArray &payload)
{
    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    const QJsonArray zones = root.value("zones").toArray();
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

    for (const QJsonValue &zv : zones) {
        const QJsonObject zo = zv.toObject();
        const int zone_id = zo.value("zone_id").toInt();
        if (zone_id <= 0)
            continue;

        // fire_zone_map에 없는 zone이 DB에 생겼을 수 있다(먼저 INSERT하고
        // VMS를 아직 안 고친 상태). 버리지 않고 받아 둔다 — 최소한 값은
        // 보이고, 화면 목록에 없을 뿐이다.
        Snapshot &s = m_zones[zone_id];
        s.zone_id = zone_id;

        const QString name = zo.value("zone_name").toString();
        if (!name.isEmpty())
            s.zone_name = name;   // 구역명의 진실원천은 DB

        const QJsonValue chans = zo.value("channels");
        if (!chans.isObject() || chans.toObject().isEmpty()) {
            emit updated(zone_id);   // 이름만 갱신됐을 수 있다
            continue;                // has_data는 그대로 — 값이 없으면 없는 것
        }

        const QJsonObject channels = chans.toObject();
        s.has_data = true;

        // ---- 신선도: "도착"이 아니라 "새 사이클"만 센다 (2026-08-20) ----
        // ⚠ 이 payload 의 발행자는 RPi A 가 아니라 **RPi B** 다. A 가 꺼져도
        //   B 는 DB 의 마지막 스냅샷을 계속 재발행하므로, 도착 시각으로 신선도를
        //   찍으면 **A 하드웨어가 죽어도 상단바·Device 점이 영원히 초록**이 된다
        //   (사용자 신고 2026-08-20 — 더미 zone 가드와 같은 병리의 다른 입구).
        //   sensor_seq 가 전진한 수신에만 시각을 찍는다.
        //
        // ⚠ **sensor_seq 가 늘어난 것만으로는 부족하다** (2026-08-20 실측):
        //   RPi A 가 없어도 폴러는 행을 계속 쓰고 seq 도 계속 전진한다 —
        //   그때 실린 값이 전부 0 / is_valid=false 였다. 그래서 세 조건을
        //   모두 요구한다:
        //     ① seq 전진 (있을 때만 — 없는 폴러도 있다)
        //     ② 유효 채널이 하나라도 있음 (전부 invalid = 센서가 말이 없다)
        //     ③ 값이 실제로 움직였음 — 살아있는 A 는 원시 ADC 잡음 때문에
        //        매 사이클 최소 한 채널이 흔들린다 (실측: gas_raw 653~657,
        //        25초 표본 전부 직전과 다름). 값이 얼어붙었으면 그건 새
        //        측정이 아니라 마지막 행의 재발행이다.
        const QJsonValue seq_v = zo.value("sensor_seq");
        const qint64 seq = seq_v.isDouble() ? qint64(seq_v.toDouble(-1)) : -1;
        const bool seq_ok = (seq < 0) || (s.last_seq < 0) || (seq != s.last_seq);
        if (seq >= 0)
            s.last_seq = seq;

        // 값 지문 — 채널 순서에 무관하도록 정렬해 만든다. ⚠ seq·timestamp 는
        // 넣지 않는다(넣으면 항상 달라져 ③이 무의미해진다).
        QStringList parts;
        bool any_valid = false;
        for (auto it = channels.constBegin(); it != channels.constEnd(); ++it) {
            const QJsonObject c = it.value().toObject();
            any_valid = any_valid || c.value("is_valid").toBool();
            parts << it.key() + '='
                         + QString::number(c.value("value").toDouble(), 'g', 12);
        }
        parts.sort();
        const QString print = parts.join(';');
        const bool moved = s.value_print.isEmpty() || print != s.value_print;
        s.value_print = print;

        const bool fresh = seq_ok && any_valid && moved;
        if (fresh) {
            // 연속 조건: 직전 새 사이클과의 간격이 STALE_MS 를 넘었으면 그
            // 사이에 끊긴 것이다 — 이어 세지 않고 1부터 다시 센다.
            const bool continuous =
                s.last_ms > 0 && (now_ms - s.last_ms) <= STALE_MS;
            s.fresh_streak = continuous ? s.fresh_streak + 1 : 1;
            s.last_ms = now_ms;
        }

        s.composite_score = zo.value("composite_score").isNull()
                                ? -1.0
                                : zo.value("composite_score").toDouble(-1.0);

        for (const SensorField &f : SENSOR_FIELDS) {
            const QString key = QString::fromLatin1(f.key);
            const QJsonObject ch = channels.value(key).toObject();
            Sample sm;
            if (ch.isEmpty()) {
                sm.present = false;
            } else {
                sm.present = true;
                sm.value = ch.value("value").toDouble();
                sm.valid = ch.value("is_valid").toBool();
                push_point(zone_id, key, now_ms, sm.value, sm.valid);
            }
            s.channels.insert(key, sm);
        }

        emit updated(zone_id);
    }
}

/*
 * 백필 응답. 각 점은 [zone, ch, s, v, ok] — s는 "몇 초 전"(DESC = 과거→최신).
 * 전 zone이 한 응답에 섞여 오지만 push_point가 (zone, ch)로 나눠 담고,
 * 전체가 s DESC이므로 각 부분수열의 시간순도 유지된다.
 */
void ZoneSensorStore::on_sensor_history(const QJsonObject &reply)
{
    const QJsonArray points = reply.value("points").toArray();
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

    for (const QJsonValue &pv : points) {
        const QJsonArray a = pv.toArray();
        if (a.size() < 5)
            continue;   // PHASE 7 이전 형식(zone 없음) — 섞이면 그래프가 틀린다
        const int zone_id = a[0].toInt();
        const QString channel = a[1].toString();
        const qint64 s = qint64(a[2].toDouble());
        const double v = a[3].toDouble();
        const bool ok = a[4].toBool();
        push_point(zone_id, channel, now_ms - s * 1000, v, ok);
    }

    // ⚠ 백필 점들은 **과거** 시각인데, 요청 왕복(브로커→RPi B→DB) 동안 실시간
    // 구독이 이미 **현재** 시각 점을 버퍼에 넣어놨을 수 있다. push_point 는
    // append 라 그 경우 순서가 뒤집힌다 — 앞쪽 트림(while buf.first() < cutoff)
    // 과 폴리라인 렌더러가 둘 다 오름차순을 전제하므로 그래프가 튀거나 트림이
    // 엉뚱한 점을 지운다.
    //
    // 정렬은 **여기서만** 한다(실행당 1회). push_point 는 1Hz × zone × 채널로
    // 도는 뜨거운 경로이고 실시간 점은 원래 오름차순이라, 거기에 정렬삽입을
    // 넣으면 상시 비용만 늘고 얻는 게 없다. (2026-08-10 수정)
    const qint64 cutoff = now_ms - HISTORY_WINDOW_MS;
    for (auto zit = m_history.begin(); zit != m_history.end(); ++zit) {
        for (auto cit = zit->begin(); cit != zit->end(); ++cit) {
            QVector<Point> &buf = *cit;
            std::sort(buf.begin(), buf.end(),
                      [](const Point &a, const Point &b) { return a.t_ms < b.t_ms; });
            while (!buf.isEmpty() && buf.first().t_ms < cutoff)
                buf.removeFirst();
        }
    }

    qInfo() << "[ZoneSensorStore] 이력" << points.size() << "포인트 백필됨";
    for (auto it = m_zones.constBegin(); it != m_zones.constEnd(); ++it)
        emit updated(it.key());
}

void ZoneSensorStore::request_backfill()
{
    if (m_backfill_asked)
        return;
    m_backfill_asked = true;

    QJsonObject hist_req;
    hist_req["cmd"] = "sensor_history";
    hist_req["minutes"] = HISTORY_BACKFILL_MIN;
    MqttLink::instance()->request(
        SENSOR_HISTORY_TOPIC, hist_req,
        [this](const QJsonObject &reply) { on_sensor_history(reply); },
        [this](const QString &reason) {
            // 실패해도 치명적이지 않다 — 실시간 구독이 곧 새 점을 채운다.
            // 다만 다음 재접속에서 한 번 더 시도할 수 있게 빗장을 풀어 둔다.
            m_backfill_asked = false;
            qWarning() << "[ZoneSensorStore] 이력 백필 실패 —" << reason;
        });
}

void ZoneSensorStore::on_fire_threshold(const QByteArray &payload)
{
    // queryFireThreshold()(RPi B)가 활성 임계가 없으면 "threshold": null을
    // 보낸다 — toObject()가 그 경우 빈 오브젝트를 주므로 화면이 자동으로
    // fallback 범위로 돌아간다 (별도 분기 불필요).
    const QJsonObject root = QJsonDocument::fromJson(payload).object();
    m_threshold = root.value("threshold").toObject();
    emit threshold_changed();
}

// ------------------------------------------------------------------- 더미

/*
 * 실 하드웨어가 없는 zone을 실데이터와 **같은 경로**로 채운다 — push_point와
 * Snapshot을 그대로 쓰므로 화면은 더미인지 알 필요가 없고, 알 수도 없다.
 *
 * 값 생성 방식: 채널별 실측 평시값(dummy_base)을 중심으로 한 **경계 있는
 * 랜덤워크**. 이전에는 삼각파를 썼는데 두 가지가 문제였다:
 *   1) 범위(fallback_min~max)의 25% 지점을 기준으로 잡아서 실제와 동떨어졌다.
 *      spark_raw가 특히 심했다 — 평시 실측은 900~1000대인데 200~350이 나왔다.
 *   2) 톱니처럼 규칙적으로 오르내려 그래프가 기계적으로 보였다. 실제 센서는
 *      한 자리에서 미세하게 떠도는 모양이라 비교 화면에서 눈에 띄게 달랐다.
 *
 * 랜덤워크는 직전 값에서 조금씩만 움직이므로 매 틱 난수를 새로 뽑는 것과
 * 달리 선이 튀지 않고, 그러면서도 주기가 없어 불규칙해 보인다. base ± jitter
 * 로 가둬서 더미가 화재 색으로 물드는 일은 없다(경보와 헷갈리면 안 된다).
 */
void ZoneSensorStore::tick_dummy()
{
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

    int n = 0;
    const FireZoneInfo *zones = fire_zone_table(&n);

    for (int zi = 0; zi < n; ++zi) {
        if (!zones[zi].dummy)
            continue;
        const int zone_id = zones[zi].zone_id;
        Snapshot &s = m_zones[zone_id];
        s.zone_id = zone_id;
        s.has_data = true;
        s.last_ms = now_ms;

        QHash<QString, double> &cur = m_dummy_val[zone_id];

        for (const SensorField &f : SENSOR_FIELDS) {
            const QString key = QString::fromLatin1(f.key);

            auto it = cur.find(key);
            if (it == cur.end()) {
                // 시작점을 zone마다 조금 어긋나게 — 전 구역이 같은 값에서
                // 출발하면 비교 화면에서 선이 겹쳐 하나처럼 보인다.
                const double off = (QRandomGenerator::global()->generateDouble() - 0.5)
                                   * f.dummy_jitter * 1.2;
                it = cur.insert(key, f.dummy_base + off);
            }

            // 한 틱에 흔들림 폭의 최대 ±15%씩만 이동 — 이보다 크면 톱니처럼
            // 보이고, 작으면 멈춘 것처럼 보인다.
            const double step = (QRandomGenerator::global()->generateDouble() - 0.5)
                                * f.dummy_jitter * 0.30;
            *it = qBound(f.dummy_base - f.dummy_jitter,
                         *it + step,
                         f.dummy_base + f.dummy_jitter);

            Sample sm;
            sm.present = true;
            sm.valid = true;
            sm.value = *it;
            s.channels.insert(key, sm);
            push_point(zone_id, key, now_ms, sm.value, true);
        }

        // 종합 위험도도 평상시 zone 1과 같은 그림으로 — 실제로 0으로 찍히고
        // 있으므로 더미도 0 근처에서만 논다. 여기서 큰 값이 나오면 "저 구역
        // 위험한가?" 하는 오해를 만든다.
        double &score = cur[QStringLiteral("__score")];
        score = qBound(0.0,
                       score + (QRandomGenerator::global()->generateDouble() - 0.5) * 0.6,
                       3.0);
        s.composite_score = score;

        emit updated(zone_id);
    }
}

// ------------------------------------------------------------------ 공통

void ZoneSensorStore::push_point(int zone_id, const QString &channel_key,
                                 qint64 t_ms, double value, bool valid)
{
    if (!valid)
        return;   // 이상치는 그래프에 안 그린다 — 값·상태 표시는 Snapshot이 따로 함

    // spark_raw만 그래프용 값에 EMA를 걸어 부드럽게 만든다 — 포토트랜지스터
    // raw 특성상 매 사이클 값이 크게 튀어 그대로 이으면 0/1 이진값처럼 보인다.
    // Snapshot의 숫자·게이지는 이 함수를 거치지 않으므로 원시값 그대로 남는다.
    double graph_value = value;
    if (channel_key == QLatin1String("spark_raw")) {
        QHash<QString, double> &ema = m_ema[zone_id];
        auto it = ema.find(channel_key);
        if (it == ema.end())
            ema.insert(channel_key, value);
        else
            *it = EMA_ALPHA * value + (1.0 - EMA_ALPHA) * *it;
        graph_value = ema.value(channel_key);
    }

    QVector<Point> &buf = m_history[zone_id][channel_key];
    buf.append({ t_ms, graph_value });

    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - HISTORY_WINDOW_MS;
    while (!buf.isEmpty() && buf.first().t_ms < cutoff)
        buf.removeFirst();
}
