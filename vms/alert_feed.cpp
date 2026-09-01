#include "alert_feed.h"
#include "mqtt_link.h"
#include "alert_time.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace {

const QString TOPIC_LIVE = "guardx/alert/rpib";
const QString TOPIC_SNAPSHOT = "guardx/db/rpib/incidents";
const QString TOPIC_QUERY = "guardx/db/rpib/query/incidents";
const QString TOPIC_AUDIO = "guardx/alert/rpic";   // 오디오 감지(비명/총성) — 독립 경로
const int QOS = 1;   // 상태성 토픽 — 전달 QoS는 min(발행, 구독)이라 1로 받는다

const int NUM_CHANNELS = 4;

AlertFeed::Severity parse_severity(const QString &s)
{
    if (s == "critical") return AlertFeed::Critical;
    if (s == "warn")     return AlertFeed::Warn;
    return AlertFeed::None;   // "clear" 포함
}

bool valid(int ch) { return ch >= 0 && ch < NUM_CHANNELS; }

} // namespace

AlertFeed *AlertFeed::instance()
{
    static AlertFeed *feed = new AlertFeed;
    return feed;
}

AlertFeed::AlertFeed(QObject *parent) : QObject(parent)
{
    MqttLink *link = MqttLink::instance();

    link->subscribe(TOPIC_LIVE,
                    [this](const QByteArray &p) { on_live_alert(p); }, QOS);
    link->subscribe(TOPIC_SNAPSHOT,
                    [this](const QByteArray &p) { on_incidents_snapshot(p); }, QOS);
    // 오디오 경보(비명/총성) — 혼잡과 독립. 상태머신을 안 거치고 신호만 낸다.
    link->subscribe(TOPIC_AUDIO,
                    [this](const QByteArray &p) { on_audio_alert(p); }, QOS);
    // 이력 조회는 MqttLink::request()가 응답 토픽·req_id·타임아웃을 맡는다

    qInfo() << "[AlertFeed] 구독 등록 —" << TOPIC_LIVE << "+" << TOPIC_SNAPSHOT
            << "+" << TOPIC_AUDIO;
}

AlertFeed::State AlertFeed::state(int ch) const
{
    return valid(ch) ? m_state[ch] : State();
}

AlertFeed::Severity AlertFeed::severity(int ch) const
{
    return valid(ch) ? m_state[ch].severity : None;
}

AlertFeed::Severity AlertFeed::worst() const
{
    Severity w = None;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        if (m_state[ch].severity > w)
            w = m_state[ch].severity;
    for (Severity s : m_device)
        if (s > w)
            w = s;
    return w;
}

int AlertFeed::critical_count() const
{
    int n = 0;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        n += (m_state[ch].severity == Critical);
    n += device_count(Critical);
    return n;
}

// -------------------------------------------------- 카메라/앱 경보 (§4c-2)

void AlertFeed::raise_device_alert(int key, Severity sev, const QString &message)
{
    if (m_device.value(key, None) == sev && m_device_msg.value(key) == message)
        return;  // 전이 없음 — 스팸 금지
    m_device.insert(key, sev);
    m_device_msg.insert(key, message);
    qInfo().noquote() << "[AlertFeed] 장비 경보" << key << "→"
                      << (sev == Critical ? "critical" : sev == Warn ? "warn"
                                                                     : "clear")
                      << message;
    emit state_changed();
    emit device_alert(key, int(sev), message);
}

int AlertFeed::device_count(Severity sev) const
{
    int n = 0;
    for (Severity s : m_device)
        n += (s == sev);
    return n;
}

QStringList AlertFeed::device_messages(Severity min_sev) const
{
    QStringList out;
    for (auto it = m_device.begin(); it != m_device.end(); ++it)
        if (it.value() >= min_sev && it.value() != None)
            out << m_device_msg.value(it.key());
    return out;
}

// ------------------------------------------------------------------ 라이브

void AlertFeed::on_live_alert(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    if (o.value("event").toString() != "congestion")
        return;   // 다른 종류의 경보 (fire/gas 등)는 아직 이 화면 대상이 아니다

    const int ch = o.value("channel").toInt(-1);
    if (!valid(ch)) {
        qWarning() << "[AlertFeed] 알 수 없는 channel" << ch << "— 무시";
        return;
    }

    const Severity sev = parse_severity(o.value("severity").toString());
    const qint64 ts = o.value("timestamp").toVariant().toLongLong();

    State &st = m_state[ch];
    const bool changed = (st.severity != sev);
    st.severity = sev;
    st.predicted = (o.value("source").toString() == "prediction");
    st.count = o.value("count").toInt(-1);
    st.capacity = o.value("capacity").toInt(-1);
    // 수신 시각이 아니라 **사건 시각**이다. 링크가 밀리면 둘이 벌어지고,
    // 그 차이가 그대로 "언제부터 critical인가" 오표시가 된다.
    //
    // ⚠ 낡았다고 버리지는 않는다 — 이건 점 사건이 아니라 **상태**다.
    // 1분 전에 시작된 혼잡은 지금도 혼잡이다. 순서 문제(낡은 스냅샷이
    // 최신을 덮는 것)는 바로 아래 m_last_live_ms 비교가 이미 막고 있다.
    st.since = alert_event_time(ts);

    // 스냅샷이 이 시각보다 낡았으면 이 채널에 대해서는 무시한다
    m_last_live_ms[ch] = qMax(m_last_live_ms[ch], ts);

    qInfo() << "[AlertFeed] ch" << ch << "→"
            << (sev == Critical ? "critical" : sev == Warn ? "warn" : "clear")
            << st.count << "/" << st.capacity;

    if (changed)
        emit state_changed();
    emit alert_raised(ch, int(sev));

    // DB가 이력의 진실원천 — 표는 다시 당겨서 맞춘다 (라이브분을 끼워 넣지
    // 않는다: 중복 판정 로직이 필요해지고, 경보는 드물어 재조회가 싸다)
    request_history(m_hours);
}

void AlertFeed::on_incidents_snapshot(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    if (!o.contains("incidents"))
        return;

    const qint64 ts = o.value("timestamp").toVariant().toLongLong();

    // 스냅샷은 "열린 것 전부"라 없는 채널은 평상시라는 뜻이다. 단, 이 스냅샷보다
    // 최신 라이브 경보를 이미 받은 채널은 건드리지 않는다 (30초 낡음 대비).
    Severity fresh[NUM_CHANNELS] = { None, None, None, None };
    bool predicted[NUM_CHANNELS] = { false, false, false, false };
    int count[NUM_CHANNELS] = { -1, -1, -1, -1 };
    int capacity[NUM_CHANNELS] = { -1, -1, -1, -1 };

    for (const QJsonValue &v : o.value("incidents").toArray()) {
        const QJsonObject inc = v.toObject();
        const int ch = inc.value("channel").toInt(-1);
        if (!valid(ch))
            continue;
        const Severity sev = parse_severity(inc.value("severity").toString());
        if (sev > fresh[ch]) {           // 한 채널에 여러 건이면 나쁜 쪽
            fresh[ch] = sev;
            predicted[ch] = (inc.value("source_type").toString() == "prediction");
            // 07-31: 폴러가 실어주는 인원/정원 — 재시작 복원 시 칩에
            // "CRITICAL 2/1" 숫자가 나오게 한다 (없으면 -1 = 표시 생략)
            count[ch] = inc.value("count").toInt(-1);
            capacity[ch] = inc.value("capacity").toInt(-1);
        }
    }

    bool changed = false;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        if (ts <= m_last_live_ms[ch]) {
            qDebug() << "[AlertFeed] ch" << ch << "스냅샷이 라이브보다 낡음 — 유지";
            continue;
        }
        if (m_state[ch].severity == fresh[ch]
            && m_state[ch].count == count[ch])
            continue;
        if (m_state[ch].severity != fresh[ch])   // since = 단계가 바뀐 시각
            m_state[ch].since = QDateTime::currentDateTime();
        m_state[ch].severity = fresh[ch];
        m_state[ch].predicted = predicted[ch];
        m_state[ch].count = count[ch];
        m_state[ch].capacity = capacity[ch];
        changed = true;
    }

    if (changed) {
        qInfo() << "[AlertFeed] 스냅샷 반영 — critical" << critical_count() << "채널";
        emit state_changed();
    }
}

// -------------------------------------------------------------------- 오디오

void AlertFeed::on_audio_alert(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    const QString event = o.value("event").toString();
    if (event != "scream" && event != "gunshot")
        return;   // 오디오 토픽의 다른 이벤트는 무시

    const int ch = o.value("channel").toInt(-1);
    const double conf = o.value("confidence").toDouble(0.0);

    // ⚠ 여기가 timestamp 를 **읽지도 않았다** (08-10 리뷰). 팝업이
    // QTime::currentTime() 을 찍어서, 3분 늦게 도착한 총성이 "방금 총성"으로
    // 표시됐다. 비명·총성은 상태가 아니라 **점 사건**이라 이쪽은 나이를 보고
    // 실제로 버린다 — 지나간 사건으로 사람을 뛰게 하면 안 된다.
    const qint64 ts_ms = o.value("timestamp").toVariant().toLongLong();
    if (!alert_momentary_is_fresh(ts_ms, QStringLiteral("오디오 경보 %1").arg(event)))
        return;
    const QDateTime ts = alert_event_time(ts_ms);

    qInfo() << "[AlertFeed] 오디오 경보:" << event << "ch" << ch
            << "conf" << conf << "사건시각" << ts.toString("HH:mm:ss");

    // 혼잡 상태머신을 건드리지 않는다 — 독립 신호만 발생
    emit audio_alert(ch, event, conf, ts);
}

// -------------------------------------------------------------------- 이력

void AlertFeed::request_history(int hours)
{
    m_hours = hours;

    // 직전 요청이 아직 살아 있으면 버린다 — 늦게 온 응답이 최신을 덮지 않게
    MqttLink::instance()->cancel(m_req_id);

    QJsonObject params;
    params["query"] = "incidents";
    params["hours"] = hours;
    params["limit"] = 200;

    m_req_id = MqttLink::instance()->request(
        TOPIC_QUERY, params,
        [this](const QJsonObject &reply) {
            m_req_id.clear();
            apply_history(reply);
        },
        [this](const QString &reason) {
            // 실패해도 표는 직전 값을 유지한다 (fail-soft)
            m_req_id.clear();
            qWarning() << "[AlertFeed] 이력 조회 실패:" << reason;
            emit history_changed();
        });
}

void AlertFeed::apply_history(const QJsonObject &o)
{
    m_history.clear();
    // events: [[ts, ch, severity, source, message, incident_id, status], …]
    for (const QJsonValue &v : o.value("events").toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() < 7)
            continue;
        Event e;
        // 폴러가 "…Z"로 보내므로 ISODate 파서가 이미 UTC로 잡는다
        e.ts = QDateTime::fromString(a.at(0).toString(), Qt::ISODate).toLocalTime();
        e.channel = a.at(1).toInt(-1);
        e.severity = parse_severity(a.at(2).toString());
        e.predicted = (a.at(3).toString() == "prediction");
        e.message = a.at(4).toString();
        e.incident_id = a.at(5).toInt();
        e.resolved = (a.at(6).toString() == "resolved");
        m_history.append(e);
    }

    qInfo() << "[AlertFeed] 이력" << m_history.size() << "건 수신";
    emit history_changed();
}
