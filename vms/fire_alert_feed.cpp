#include "fire_alert_feed.h"
#include "mqtt_link.h"
#include "alert_time.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

const QString TOPIC_LIVE = "guardx/alert/fire";
const QString TOPIC_SNAPSHOT = "guardx/db/rpib/fire_incident";
const QString TOPIC_BUTTON = "guardx/alert/button";
const int QOS = 1;   // 상태성 토픽 — 전달 QoS는 min(발행, 구독)이라 1로 받는다

} // namespace

FireAlertFeed *FireAlertFeed::instance()
{
    static FireAlertFeed *feed = new FireAlertFeed;
    return feed;
}

FireAlertFeed::FireAlertFeed(QObject *parent) : QObject(parent)
{
    MqttLink *link = MqttLink::instance();

    link->subscribe(TOPIC_LIVE,
                    [this](const QByteArray &p) { on_live_alert(p); }, QOS);
    link->subscribe(TOPIC_SNAPSHOT,
                    [this](const QByteArray &p) { on_incident_snapshot(p); }, QOS);
    link->subscribe(TOPIC_BUTTON,
                    [this](const QByteArray &p) { on_button(p); }, QOS);

    qInfo() << "[FireAlertFeed] 구독 등록 —" << TOPIC_LIVE << "+" << TOPIC_SNAPSHOT
            << "+" << TOPIC_BUTTON;
}

// ------------------------------------------------------------------ 화재

void FireAlertFeed::on_live_alert(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    const QString event_type = o.value("event_type").toString();
    if (event_type != "fire_confirmed" && event_type != "recovered")
        return;

    const bool active = (event_type == "fire_confirmed");
    const qint64 ts = o.value("timestamp").toVariant().toLongLong();

    const bool changed = (m_state.active != active);
    m_state.active = active;
    m_state.cause = active ? o.value("cause").toString() : QString();
    // 팝업의 시각 표시(m_time)가 이 값이다 — 수신 시각을 쓰면 "지금 막
    // 났다"고 말하게 된다. 같은 파일의 on_button 은 원래부터 payload 시각을
    // 쓰고 있었다: 같은 판정이 함수마다 달랐던 자리다.
    //
    // ⚠ 낡았다고 버리지 않는다. 1분 전에 확정된 화재는 지금도 화재다 —
    // 상태 경보를 신선도로 버리면 진행 중인 사건을 화면에서 지운다.
    m_state.since = alert_event_time(ts);
    // zone_id는 판정과 무관 — main.c가 어느 zone에서 왔는지 표시용으로만
    // 싣는 필드다(FIRE_ZONE_HANDOFF 참조). 0이면 구버전 payload로 간주.
    m_state.zone_id = o.value("zone_id").toInt();

    // 스냅샷이 이 시각보다 낡으면 무시한다 (AlertFeed와 같은 방어)
    m_last_live_ms = qMax(m_last_live_ms, ts);

    qInfo() << "[FireAlertFeed] fire →" << (active ? "confirmed" : "recovered")
            << m_state.cause;

    if (changed)
        emit state_changed();
}

void FireAlertFeed::on_incident_snapshot(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    if (!o.contains("active"))
        return;

    const qint64 ts = o.value("timestamp").toVariant().toLongLong();
    if (ts <= m_last_live_ms) {
        qDebug() << "[FireAlertFeed] 스냅샷이 라이브보다 낡음 — 유지";
        return;
    }

    const bool active = o.value("active").toBool();
    const QString cause = active ? o.value("cause").toString() : QString();
    const int zone_id = o.value("zone_id").toInt();
    if (m_state.active == active && m_state.cause == cause && m_state.zone_id == zone_id)
        return;

    if (m_state.active != active)   // since = 상태가 바뀐 시각
        m_state.since = QDateTime::currentDateTime();
    m_state.active = active;
    m_state.cause = cause;
    m_state.zone_id = zone_id;

    qInfo() << "[FireAlertFeed] 스냅샷 반영 — active" << active;
    emit state_changed();
}

// ------------------------------------------------------------------ 버튼

void FireAlertFeed::on_button(const QByteArray &payload)
{
    const QJsonObject o = QJsonDocument::fromJson(payload).object();
    const qint64 ts_ms = o.value("timestamp").toVariant().toLongLong();
    const int zone_id = o.value("zone_id").toInt();

    // 비상 버튼은 **점 사건**이다 — 지나간 눌림을 지금 일처럼 띄우면 운영자가
    // 아무 일도 없는 현장으로 달려간다. (원래 이 함수만 payload 시각을 제대로
    // 쓰고 있었는데, 그마저 나이는 안 봤다)
    if (!alert_momentary_is_fresh(ts_ms, QStringLiteral("비상 버튼")))
        return;
    const QDateTime ts = alert_event_time(ts_ms);

    // payload의 press_count는 안 쓴다 — "직전 읽기 이후 새 눌림 수"라 정상
    // 단일 누름이면 항상 1이다(button_pressed 시그널 문서 참조). 대신 이
    // 메시지를 몇 번째로 받았는지를 직접 센다.
    ++m_button_total;

    qInfo() << "[FireAlertFeed] 비상 버튼 눌림 — 누적" << m_button_total << "회 (세션 기준), zone" << zone_id;
    emit button_pressed(m_button_total, zone_id, ts);
}
