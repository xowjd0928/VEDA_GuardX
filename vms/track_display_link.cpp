#include "track_display_link.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "mqtt_link.h"

namespace {

// RPi B 수신 토픽. 기존 cmd/* 규약을 따른다 (set_zone·set_actuator와 같은 줄).
// 다만 이쪽은 요청-응답이 아니다 — 성공/실패를 되받지 않고, 실제로 점이
// 찍혔는지는 현장 LED를 보는 것이 확인 수단이다.
const QString TRACK_TOPIC = "guardx/db/rpib/cmd/track_display";

// 지목은 상태성이라 유실되면 안 된다(놓치면 다음 재전송까지 5초 공백).
const int QOS = 1;

}  // namespace

TrackDisplayLink *TrackDisplayLink::instance()
{
    static TrackDisplayLink link;
    return &link;
}

TrackDisplayLink::TrackDisplayLink(QObject *parent) : QObject(parent)
{
    m_keepalive = new QTimer(this);
    m_keepalive->setInterval(KEEPALIVE_MS);
    connect(m_keepalive, &QTimer::timeout, this,
            &TrackDisplayLink::publish_start);
}

void TrackDisplayLink::start(int global_id, int db_channel, int object_id,
                             const QString &label)
{
    m_global_id = global_id;
    m_channel = db_channel;
    m_object_id = object_id;
    m_label = label;

    // 대상만 바뀌는 경우(추적 중에 다른 사람을 고름)에도 여기로 온다.
    // 그때는 STOP 없이 새 START를 덮어쓴다 — 사이에 STOP이 끼면 LED에서
    // 점이 한 번 사라졌다 다시 나타난다.
    publish_start();

    if (!m_active) {
        m_active = true;
        emit active_changed(true);
    }
    // 재시작한다 — 방금 한 건 보냈으니 다음 재전송은 온전한 주기 뒤여야 한다.
    m_keepalive->start();
}

void TrackDisplayLink::stop()
{
    m_keepalive->stop();

    QJsonObject req;
    req["node_id"] = "vms";
    req["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    req["seq"] = double(m_seq++);
    req["action"] = "STOP";
    MqttLink::instance()->publish(
        TRACK_TOPIC, QJsonDocument(req).toJson(QJsonDocument::Compact), QOS);

    if (m_active) {
        m_active = false;
        emit active_changed(false);
    }
}

void TrackDisplayLink::publish_start()
{
    QJsonObject req;
    req["node_id"] = "vms";
    req["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    req["seq"] = double(m_seq++);
    req["action"] = "START";
    req["global_id"] = m_global_id;
    req["channel"] = m_channel;
    req["object_id"] = m_object_id;
    req["label"] = m_label;

    MqttLink::instance()->publish(
        TRACK_TOPIC, QJsonDocument(req).toJson(QJsonDocument::Compact), QOS);
}
