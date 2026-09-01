#pragma once

#include <QObject>
#include <QString>

/**
 * @brief 구역 운영 설정 (zones + zone_thresholds) 캐시
 *
 * 수용 인원과 혼잡 임계는 schema.sql이 "단일 진실원천"으로 zone_thresholds에
 * 두기로 한 값이다. 소스에 상수로 박아두면 운영자가 정원을 바꿔도
 * VMS만 옛 숫자를 보여주게 된다.
 *
 * v2부터 DB를 직접 읽지 않는다. RPi B가 `guardx/db/zones` 토픽에 retained로
 * 발행한 값을 구독해서 캐시한다 (docs/DB_LINK_AND_MQTT_MIGRATION.md 4부).
 * 바뀐 것은 "캐시를 채우는 방법"뿐이고 아래 게터 3종은 그대로다 —
 * 그래서 이 값을 쓰는 channel_view.cpp는 수정할 필요가 없다.
 *
 * retained라 접속 즉시 현재 값이 오고, 이후 운영자가 정원을 바꾸면
 * VMS 재시작 없이 새 메시지가 날아온다 (구 방식은 기동 시 1회 SELECT라
 * 재시작 전까지 반영되지 않았다).
 *
 * 메시지가 아직 안 왔거나 브로커가 죽어 있으면 Theme의 디자인 기본값으로
 * 떨어지고 앱은 그대로 동작한다 (fail-soft — 정원 표시 하나 때문에
 * 감시를 막을 수 없다).
 *
 * 주의: zone_id와 channel은 1:1로 이어지지 않는다 (실측: zone1->ch1,
 * zone2->ch0). 발행측이 zones.channel을 실어 보내는 것을 전제로 한다.
 */
namespace ZoneConfig {

/**
 * @brief 값이 바뀌었다는 신호원
 *
 * ZoneConfig는 함수 묶음이라 시그널을 낼 수 없어 이것만 QObject다.
 * 정원은 매 프레임 다시 읽히므로 알림 없이도 반영됐지만, 구역 이름은
 * *이미 만들어진* 라벨의 글자라 누가 알려주지 않으면 안 바뀐다.
 */
class Notifier : public QObject
{
    Q_OBJECT

public:
    /** @brief 캐시를 채운 쪽이 부른다 (signals는 밖에서 emit할 수 없다) */
    void notify() { emit changed(); }

signals:
    void changed();
};

/** @brief connect(ZoneConfig::notifier(), &ZoneConfig::Notifier::changed, ...) */
Notifier *notifier();

/**
 * @brief 채널의 구역 이름 (zones.zone_name) — 아직 못 받았으면 빈 문자열
 *
 * "LOBBY EAST"처럼 구역명만 들어 있다. "CH1 · " 접두는 Theme::channel_name()이
 * 붙인다 — 채널 번호는 DB가 아니라 화면 배치의 문제다.
 */
QString name(int channel);

/**
 * @brief 구독 등록. 앱 시작 시 1회 호출
 *
 * 값은 여기서 오지 않는다 — 등록만 하고 즉시 반환한다. 실제 값은 브로커가
 * retained 메시지를 건네줄 때 콜백으로 채워진다. 그래서 반환값이 없다
 * (구 load()는 성공/실패를 즉시 알 수 있었지만 이제는 알 수 없다).
 */
void init();

/** @brief 발행된 값을 실제로 받았는지 (false면 기본값 사용 중) */
bool loaded();

/** @brief 채널의 수용 인원 (OCC n/cap의 cap) */
int capacity(int channel);

/** @brief 주의 단계 임계 비율 */
double warn_ratio(int channel);

/** @brief 위험 단계 임계 비율 */
double critical_ratio(int channel);

/**
 * @brief 채널에 매핑된 zone_id — 아직 못 받았으면 channel+1 (Z1=CH1 가정)
 *
 * 위 주석의 경고("zone_id와 channel은 1:1로 이어지지 않는다")가 이 게터의
 * 존재 이유다. 동선 분석(trajectory)은 zone_id로, 예측/점유는 channel로
 * 오므로 둘을 합쳐 보여주는 화면(ANALYTICS)은 이 매핑 없이는 엉뚱한 구역에
 * 동선을 붙이게 된다. 발행측이 zones.zone_id를 실어 보내면 그 값을 쓰고,
 * 없으면 관례적 가정(channel+1)으로 폴백한다 — 모르는 것을 표시하지 않는
 * 것보다 낫고, 틀리면 payload에 zone_id를 실으면 바로잡힌다.
 */
int zone_id(int channel);

/** @brief zone_id가 매핑된 채널 — 모르면 zone_id-1이 유효 범위일 때 그 값, 아니면 -1 */
int zone_channel(int zone_id);

} // namespace ZoneConfig
