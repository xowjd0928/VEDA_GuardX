#pragma once
// topics.hpp — 프로세스 간 내부 신호 토픽.
//
// VMS와 주고받는 토픽(guardx/db/rpib/zones·query/*·cmd/*)은 대외 규약이라
// vms/docs/DB_LINK_AND_MQTT_MIGRATION.md 가 진실원천이고 각 모듈이 자기
// 상수로 들고 있다. 여기 있는 것은 그와 성격이 다르다 — **RPi B 안의 두
// 프로세스만 아는 신호**라 양쪽이 같은 문자열을 봐야 하고, 어느 한쪽
// 모듈에 두면 반대쪽이 그 모듈을 include 하게 되어 의존이 거꾸로 생긴다.
//
// 그래서 양쪽이 다 의존하는 Mqtt(전송 계층)에 둔다.

/**
 * @brief 임계 변경 신호 — guardx_mqttd(발행) → guardx_poller(구독)
 *
 * payload: {"node_id":"rpib","zone_id":N}
 *
 * set_zone 은 mqttd 가 처리하지만 재판정(pollAlert)은 카메라 폴러의 틱에서만
 * 돈다. 한 프로세스일 때는 메모리 큐로 넘기던 것을 프로세스가 갈리면서
 * 브로커로 넘긴다. retain 하지 않는다 — 지나간 신호를 재기동 때 다시
 * 실행하면 안 되는 성격이다.
 */
inline constexpr const char* TOPIC_ZONE_CHANGED =
    "guardx/db/rpib/evt/zone_changed";
