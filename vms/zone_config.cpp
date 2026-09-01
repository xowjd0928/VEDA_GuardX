#include "zone_config.h"
#include "mqtt_link.h"
#include "theme.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace {

const int NUM_CHANNELS = 4;

/**
 * @brief 구역 설정 토픽
 *
 * 이름은 규약의 guardx/{도메인}/{노드ID}/{하위} 형식을 따른다 —
 * guardx/sensor/rpia/button 과 같은 구조다. 발행자는 RPi B의 폴러다.
 *
 * QoS 1 + retained. 유실되면 정원이 영영 틀린 채로 남으므로 QoS 0은 안 되고
 * (센서 토픽과 달리 주기 재발행이 없다), apply()가 덮어쓰기라 중복 수신은
 * 무해하므로 QoS 2까지 갈 이유도 없다 — 규약 0절 원칙 2의 "멱등 동작".
 *
 * 실제 전달 QoS는 min(발행 QoS, 구독 QoS)이므로 구독도 1로 줘야 한다.
 */
const QString TOPIC = "guardx/db/rpib/zones";
const int QOS = 1;

struct Entry {
    int capacity = -1;          // -1 = 아직 못 받음 -> Theme 기본값
    double warn = 0.75;
    double critical = 0.90;
    QString name;               // 빈 문자열 = 아직 못 받음 -> "CHn"만 표시
    int zone_id = -1;           // -1 = 아직 못 받음 -> channel+1 가정
};

Entry g_zones[NUM_CHANNELS];
bool g_loaded = false;

bool valid(int ch)
{
    return ch >= 0 && ch < NUM_CHANNELS;
}

/**
 * @brief 수신한 payload를 캐시에 반영
 *
 * 구 load()의 while(q.next()) 루프와 같은 일을 한다. 다른 점은 이 함수가
 * 여러 번 불린다는 것이다 — 접속 시 retained 1회, 이후 값이 바뀔 때마다.
 */
void apply(const QByteArray &payload)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "[ZoneConfig] payload 파싱 실패:" << err.errorString();
        return;
    }

    const QJsonArray arr = doc.object().value("zones").toArray();
    if (arr.isEmpty()) {
        qWarning() << "[ZoneConfig] zones 배열이 비어 있음 — 기본 정원 유지";
        return;
    }

    int found = 0;
    bool display_changed = false;   // 이름 **또는 zone_id 매핑**이 바뀌었나
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const int ch = o.value("channel").toInt(-1);
        if (!valid(ch)) {
            qWarning() << "[ZoneConfig] 알 수 없는 channel" << ch << "— 건너뜀";
            continue;
        }

        // zone_name이 없는 옛 폴러와도 동작해야 한다 — 키가 없으면 기존 값 유지.
        // 바뀐 것이 있을 때만 표시를 갱신한다: 30초 주기 재발행마다 REPORT를
        // 다시 그리면 스크롤·선택 상태가 매번 튄다.
        if (const QJsonValue nv = o.value("zone_name"); nv.isString()) {
            const QString nm = nv.toString().trimmed();
            if (!nm.isEmpty() && nm != g_zones[ch].name) {
                g_zones[ch].name = nm;
                display_changed = true;
            }
        }

        // capacity_limit은 스키마상 NULL 가능. null이거나 키가 없으면
        // 기존 값을 유지한다 (isDouble()이 null·undefined·문자열을 한 번에 거른다)
        if (const QJsonValue cv = o.value("capacity_limit"); cv.isDouble())
            g_zones[ch].capacity = cv.toInt();
        if (const QJsonValue wv = o.value("warn_ratio"); wv.isDouble())
            g_zones[ch].warn = wv.toDouble();
        if (const QJsonValue cr = o.value("critical_ratio"); cr.isDouble())
            g_zones[ch].critical = cr.toDouble();
        // zone_id — 동선(zone_id 기준)과 예측(channel 기준)을 잇는 열쇠.
        // 옛 폴러는 안 실어 보낼 수 있다 — 키가 없으면 기존 값(가정) 유지.
        //
        // ⚠ 이름과 **같이** 알림을 걸어야 한다. 이 값은 ANALYTICS 카드 제목의
        //   "Z2" 태그로 구워지는데(AZoneCard::retitle), 이름이 그대로면서
        //   매핑만 바뀌는 payload가 실재한다(이름이 비어 있는 현장·zone_id를
        //   뒤늦게 싣기 시작한 폴러). 알림이 없으면 제목은 옛 태그로 남고
        //   그 아래 동선·KPI는 새 매핑으로 갱신돼, 한 화면이 같은 구역을
        //   두 이름으로 부른다 (08-20 리뷰 확인).
        if (const QJsonValue zv = o.value("zone_id"); zv.isDouble()) {
            const int zid = zv.toInt();
            if (zid != g_zones[ch].zone_id) {
                g_zones[ch].zone_id = zid;
                display_changed = true;
            }
        }
        ++found;
    }

    if (found == 0) {
        qWarning() << "[ZoneConfig] 쓸 수 있는 구역이 없음 — 기본 정원 유지";
        return;
    }

    g_loaded = true;
    qInfo() << "[ZoneConfig] 구역 설정" << found << "건 수신 —"
            << "정원" << ZoneConfig::capacity(0) << ZoneConfig::capacity(1)
            << ZoneConfig::capacity(2) << ZoneConfig::capacity(3);

    if (display_changed) {
        qInfo() << "[ZoneConfig] 구역 이름 —"
                << ZoneConfig::name(0) << ZoneConfig::name(1)
                << ZoneConfig::name(2) << ZoneConfig::name(3);
        qInfo() << "[ZoneConfig] zone_id 매핑 —"
                << ZoneConfig::zone_id(0) << ZoneConfig::zone_id(1)
                << ZoneConfig::zone_id(2) << ZoneConfig::zone_id(3);
        ZoneConfig::notifier()->notify();
    }
}

} // namespace

namespace ZoneConfig {

void init()
{
    MqttLink::instance()->subscribe(TOPIC, apply, QOS);

    // 값이 언제 올지는 모른다. 그때까지 게터는 Theme 기본값을 돌려준다.
    qInfo() << "[ZoneConfig]" << TOPIC << "구독 등록 — 수신 전까지 기본 정원 사용";
}

Notifier *notifier()
{
    static Notifier n;
    return &n;
}

QString name(int channel)
{
    return valid(channel) ? g_zones[channel].name : QString();
}

bool loaded()
{
    return g_loaded;
}

int capacity(int channel)
{
    if (valid(channel) && g_zones[channel].capacity > 0)
        return g_zones[channel].capacity;
    return Theme::channel_cap(channel);   // 디자인 기본값으로 폴백
}

double warn_ratio(int channel)
{
    return valid(channel) ? g_zones[channel].warn : 0.75;
}

double critical_ratio(int channel)
{
    return valid(channel) ? g_zones[channel].critical : 0.90;
}

int zone_id(int channel)
{
    if (valid(channel) && g_zones[channel].zone_id > 0)
        return g_zones[channel].zone_id;
    return channel + 1;   // 발행측이 zone_id를 안 실으면 Z1=CH1 가정
}

int zone_channel(int id)
{
    // 받은 매핑이 우선 — 실측(zone1->ch1, zone2->ch0)처럼 뒤섞일 수 있다
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        if (g_zones[ch].zone_id == id)
            return ch;
    // 매핑을 아직 못 받았으면 관례적 가정. 단, **일부라도 받았다면** 가정을
    // 쓰지 않는다 — 절반은 실측, 절반은 가정이면 두 채널이 같은 구역을
    // 주장하는 모순이 생긴다.
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        if (g_zones[ch].zone_id > 0)
            return -1;
    return (id >= 1 && id <= NUM_CHANNELS) ? id - 1 : -1;
}

} // namespace ZoneConfig
