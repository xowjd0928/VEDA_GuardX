#ifndef MQTT_BUS_H
#define MQTT_BUS_H

#include "guardx_err.h"
#include "guardx_protocol.h"   /* 공통 토픽/QoS 정의 (common/include/) */

/*
 * MQTT 양방향 wrapper (libmosquitto 기반).
 *
 * RPi B의 엔진은 두 독립 구간의 접점이다:
 *   입력 구간: guardx/sensor/+(/button) 구독        <- A->B 구간의 종점
 *   출력 구간: guardx/actuator/{node} 발행          <- B->C 구간의 시발점
 * 하나의 클라이언트(client id "rpib")가 둘 다 담당한다.
 *
 * PHASE 6: zone(RPi A/C 물리 쌍)이 여러 개일 수 있게 되면서, 구독이
 * "rpia" 하나로 고정된 토픽이 아니라 MQTT 와일드카드(+)로 바뀌었다.
 * 즉 어느 zone의 메시지인지는 토픽 문자열 자체(도착한 실제 node_id)로
 * 알 수 있을 뿐 미리 정해져 있지 않다 - 그래서 수신 콜백이 이제 node_id를
 * 함께 받는다. 발행(액추에이터)도 마찬가지로 대상 노드를 호출자가
 * 매번 지정해야 한다(어느 zone의 RPi C로 보낼지는 main.c가 안다).
 */

/* 브로커 접속 정보.
 * 브로커가 같은 기기(RPi B)에서 돌므로 localhost 고정이 정상이다 -
 * A/C처럼 IP를 바꿀 일이 없다. mTLS 전환 후에도 로컬 루프백은 평문
 * 1883을 유지하는 구성이 가능(brokers conf에서 리스너 분리)하므로
 * TLS 기본값 0. 외부 리스너만 mTLS로 잠그는 걸 권장. */
#define MQTT_USE_TLS        0
#define MQTT_BROKER_HOST    "localhost"
#define MQTT_BROKER_PORT    (MQTT_USE_TLS ? 8883 : 1883)
#define MQTT_TLS_CA_PATH    "/etc/guardx/certs/ca.crt"
#define MQTT_TLS_CERT_PATH  "/etc/guardx/certs/rpib.crt"
#define MQTT_TLS_KEY_PATH   "/etc/guardx/certs/rpib.key"

/* 수신 콜백 3종. payload는 NUL 종료 보장 안 됨 - len 기준으로 다룰 것.
 * mosquitto 내부 네트워크 스레드에서 호출된다(전부 같은 스레드 - 서로
 * 동시에 불리지 않는다).
 *
 * node_id: 실제 도착한 토픽에서 뽑아낸 발신자 노드 ID (예: "rpia",
 * "rpia-2"). 와일드카드 구독이라 호출측(main.c)이 이 값으로 fire_zone
 * 매핑을 찾아 어느 zone의 메시지인지 판단해야 한다. config 콜백에는
 * node_id가 없다 - 그 토픽은 RPi B 자신(고정 1개)을 향한 신호라
 * 애초에 여럿일 이유가 없다. */
typedef void (*mqtt_sensor_handler_t)(const char *node_id, const char *payload, int len);
typedef void (*mqtt_button_handler_t)(const char *node_id, const char *payload, int len);
typedef void (*mqtt_config_handler_t)(const char *payload, int len);
/* PHASE 7: 수동 화재 해제 (VMS -> B). config와 마찬가지로 RPi B 자신을
 * 향한 명령이라 node_id가 없다 - 대신 payload 안의 zone_id로 대상을
 * 고른다(guardx_protocol.h GUARDX_TOPIC_CLEAR_FIRE_FMT 참조). */
typedef void (*mqtt_clear_fire_handler_t)(const char *payload, int len);

guardx_err_t mqtt_bus_init(mqtt_sensor_handler_t on_sensor,
                           mqtt_button_handler_t on_button,
                           mqtt_config_handler_t on_config,
                           mqtt_clear_fire_handler_t on_clear_fire);

/* rpic_node_id: 이 명령을 받을 RPi C의 노드 ID (fire_zone.rpic_node_id).
 * guardx/actuator/{rpic_node_id} 로 발행한다. */
guardx_err_t mqtt_bus_publish_actuator(const char *rpic_node_id,
                                       const char *json, int len); /* GUARDX_QOS_ACTUATOR */

/* VMS 경보 발행 (guardx/alert/fire, guardx/alert/button 등). guardx_protocol.h
 * 범위 밖(A/B/C 노드 간 규약이 아니라 B->VMS 전용 규약)이라 토픽을 호출자가
 * 넘긴다. QoS1·retain=false — "전이 순간"만 의미 있는 edge 신호라 액추에이터
 * 발행과 같은 QoS를 쓰되 상태 보존은 하지 않는다(재접속 시 복원은 guardx_mqttd
 * 의 retained 스냅샷 토픽 몫). */
guardx_err_t mqtt_bus_publish_alert(const char *topic, const char *json, int len);

void         mqtt_bus_cleanup(void);

#endif /* MQTT_BUS_H */
