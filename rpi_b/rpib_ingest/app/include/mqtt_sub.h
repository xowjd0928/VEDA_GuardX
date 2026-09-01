#ifndef MQTT_SUB_H
#define MQTT_SUB_H

#include "guardx_err.h"
#include "guardx_protocol.h"

/*
 * mqtt_sub.h - rpib_ingest MQTT (구독: 센서+버튼 / 발행: 버튼 경보)
 *
 * rpib_engine의 mqtt_bus.c에서 판정/액추에이터 관련(설정 리로드, 화재
 * 해제, 액추에이터 발행)을 뺀 부분만 남긴 것 - client_id만 다르다
 * (rpib-decision과 브로커에서 충돌하면 안 되므로, 2026-08-10 client_id/
 * CN 체계 결정).
 */

#define MQTT_USE_TLS        0
#define MQTT_BROKER_HOST    "localhost"
#define MQTT_BROKER_PORT    (MQTT_USE_TLS ? 8883 : 1883)
#define MQTT_TLS_CA_PATH    "/etc/guardx/certs/ca.crt"
#define MQTT_TLS_CERT_PATH  "/etc/guardx/certs/rpib-ingest.crt"
#define MQTT_TLS_KEY_PATH   "/etc/guardx/certs/rpib-ingest.key"
#define MQTT_CLIENT_ID      "rpib-ingest"

/* node_id: 도착한 토픽에서 뽑은 발신자(RPi A) node_id - 와일드카드
 * 구독이라 main.c가 이 값으로 zone_loader의 fire_zone_t를 찾는다. */
typedef void (*mqtt_sensor_handler_t)(const char *node_id, const char *payload, int len);
typedef void (*mqtt_button_handler_t)(const char *node_id, const char *payload, int len);

guardx_err_t mqtt_sub_init(mqtt_sensor_handler_t on_sensor,
                           mqtt_button_handler_t on_button);

guardx_err_t mqtt_sub_publish_alert(const char *topic, const char *json, int len);

void         mqtt_sub_cleanup(void);

#endif /* MQTT_SUB_H */
