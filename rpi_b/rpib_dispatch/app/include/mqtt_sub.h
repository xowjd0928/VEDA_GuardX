#ifndef MQTT_SUB_H
#define MQTT_SUB_H

#include "guardx_err.h"
#include "fire_internal.h"

/*
 * mqtt_sub.h - rpib_dispatch MQTT
 *
 * 구독: 내부 fire_command 토픽 하나뿐(fire_internal.h). 발행:
 * guardx/actuator/<rpic_node> - RPi C가 받는 실제 명령. rpib_decision이
 * 무엇을 보낼지 이미 결정해 보내주므로, 이 프로세스는 무상태 실행기다.
 */

#define MQTT_USE_TLS        0
#define MQTT_BROKER_HOST    "localhost"
#define MQTT_BROKER_PORT    (MQTT_USE_TLS ? 8883 : 1883)
#define MQTT_TLS_CA_PATH    "/etc/guardx/certs/ca.crt"
#define MQTT_TLS_CERT_PATH  "/etc/guardx/certs/rpib-dispatch.crt"
#define MQTT_TLS_KEY_PATH   "/etc/guardx/certs/rpib-dispatch.key"
#define MQTT_CLIENT_ID      "rpib-dispatch"

typedef void (*mqtt_fire_cmd_handler_t)(const fire_cmd_msg_t *m);

guardx_err_t mqtt_sub_init(mqtt_fire_cmd_handler_t on_fire_cmd);

/* rpic_node: 발행 대상 RPi C (fire_cmd_msg_t.rpic_node) */
guardx_err_t mqtt_sub_publish_actuator(const char *rpic_node,
                                       const char *json, int len);

void         mqtt_sub_cleanup(void);

#endif /* MQTT_SUB_H */
