#include "zone_display_link.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QtMath>

#include "fire_zone_map.h"
#include "mqtt_link.h"
#include "zone_sensor_store.h"

namespace {

// One topic per zone, exactly as matrix_link.h expects. They are split
// because they are retained: a shared topic would keep only the last zone
// and RPi C would never see the other three after a reconnect.
QString zone_topic(int zone_id)
{
    return QString("guardx/display/rpic/zones/%1").arg(zone_id);
}

// Display data is state. Losing an update leaves a stale reading on the
// wall until the next one, so QoS 1 + retain, same as RPi B.
const int QOS = 1;
const bool RETAIN = true;

// ZoneSensorStore channel keys (sensor_fields.h).
const QLatin1String KEY_TEMP("temperature");
const QLatin1String KEY_HUMI("humidity");

}  // namespace

ZoneDisplayLink *ZoneDisplayLink::instance()
{
    static ZoneDisplayLink link;
    return &link;
}

ZoneDisplayLink::ZoneDisplayLink(QObject *parent) : QObject(parent)
{
    m_enabled = QSettings("GuardX", "VMS")
                    .value("display/zone_dummy", true).toBool();
}

void ZoneDisplayLink::start()
{
    if (m_started || !m_enabled)
        return;
    m_started = true;

    // ZoneSensorStore emits updated() for real and dummy zones alike; the
    // dummy filter lives in on_zone_updated() so there is a single place
    // that decides what this class is allowed to publish.
    connect(ZoneSensorStore::instance(), &ZoneSensorStore::updated, this,
            &ZoneDisplayLink::on_zone_updated);
}

void ZoneDisplayLink::on_zone_updated(int zone_id)
{
    const FireZoneInfo *info = find_fire_zone(zone_id);
    if (!info || !info->dummy)
        return;   // real zone - RPi B owns this topic

    const ZoneSensorStore::Snapshot snap =
        ZoneSensorStore::instance()->snapshot(zone_id);

    const ZoneSensorStore::Sample temp = snap.channels.value(KEY_TEMP);
    const ZoneSensorStore::Sample humi = snap.channels.value(KEY_HUMI);
    if (!temp.valid || !humi.valid)
        return;

    // Send only what the registers can hold. An out-of-range frame is
    // rejected whole, which would lose the humidity along with the
    // temperature - clamping shows a wrong value but keeps the other one.
    int temp_x10 = int(qRound(temp.value * 10.0));
    int humidity = int(qRound(humi.value));
    temp_x10 = qBound(0, temp_x10, TEMP_X10_MAX);
    humidity = qBound(0, humidity, HUMIDITY_MAX);

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();

    Sent &sent = m_sent[zone_id];
    if (sent.valid && sent.temp_x10 == temp_x10 && sent.humidity == humidity &&
        now_ms - sent.last_ms < REFRESH_MS)
        return;

    QJsonObject msg;
    msg["node_id"] = "vms";
    msg["timestamp"] = now_ms;
    msg["seq"] = double(m_seq++);
    msg["zone_id"] = zone_id;
    msg["temp_x10"] = temp_x10;
    msg["humidity"] = humidity;

    // Record the send only after it succeeded. The other order would mark a
    // dropped value as sent, and the next tick would skip it as unchanged -
    // that update would be lost for good instead of being retried a second
    // later on the next store tick.
    if (!MqttLink::instance()->publish(
            zone_topic(zone_id),
            QJsonDocument(msg).toJson(QJsonDocument::Compact), QOS, RETAIN))
        return;

    sent.temp_x10 = temp_x10;
    sent.humidity = humidity;
    sent.last_ms = now_ms;
    sent.valid = true;
}
