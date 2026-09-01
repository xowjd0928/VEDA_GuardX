#ifndef MQTT_BUS_H
#define MQTT_BUS_H

#include "guardx_err.h"
#include "guardx_protocol.h"
#include "fire_internal.h"

/*
 * mqtt_bus.h - rpib_decision MQTT
 *
 * 구독: 센서(판정 입력), 설정 리로드, 수동 화재 해제.
 * 발행: guardx/alert/fire(VMS 경보), 내부 fire_command(rpib_dispatch로).
 * 원본 mqtt_bus.c에서 버튼 구독과 액추에이터 직접 발행을 뺐다 - 버튼은
 * rpib_ingest 몫, 액추에이터 실발행은 rpib_dispatch 몫.
 */

#define MQTT_USE_TLS        0
#define MQTT_BROKER_HOST    "localhost"
#define MQTT_BROKER_PORT    (MQTT_USE_TLS ? 8883 : 1883)
#define MQTT_TLS_CA_PATH    "/etc/guardx/certs/ca.crt"
#define MQTT_TLS_CERT_PATH  "/etc/guardx/certs/rpib-decision.crt"
#define MQTT_TLS_KEY_PATH   "/etc/guardx/certs/rpib-decision.key"
#define MQTT_CLIENT_ID      "rpib-decision"

typedef void (*mqtt_sensor_handler_t)(const char *node_id, const char *payload, int len);
typedef void (*mqtt_config_handler_t)(const char *payload, int len);
typedef void (*mqtt_clear_fire_handler_t)(const char *payload, int len);

guardx_err_t mqtt_bus_init(mqtt_sensor_handler_t on_sensor,
                           mqtt_config_handler_t on_config,
                           mqtt_clear_fire_handler_t on_clear_fire);

guardx_err_t mqtt_bus_publish_alert(const char *topic, const char *json, int len);

/* LED 매트릭스 표시 데이터 발행 (guardx/display/rpic/*). QoS1·retain=true.
 *
 * alert와 갈라놓은 것은 retain 하나 때문이다. 경보는 "전이 순간"이라
 * 보관하면 안 되지만(재접속할 때마다 옛 경보가 되살아난다), 표시 데이터는
 * "지금 화면이 어때야 하는가"라 보관해야 한다 - RPi C가 재접속하면 브로커가
 * 마지막 값을 바로 물려줘야 LED가 빈 화면으로 돌아가지 않는다.
 *
 * 액추에이터 명령과 달리 rpib_dispatch를 거치지 않고 여기서 바로 쏜다 -
 * dispatch를 경유하는 이유가 fire_event_command 감사 기록을 붙이기
 * 위함인데, 표시 데이터는 남길 기록이 없다. guardx/alert/fire를 여기서
 * 직접 발행하는 것과 같은 이유다.
 *
 * 토픽 문자열은 rpi_c/rpic_app/app/include/matrix_link.h와 같아야 한다. */
guardx_err_t mqtt_bus_publish_display(const char *topic, const char *json, int len);

/* fire_scenario()/recover_scenario()가 명령 하나당 한 번씩 부른다 -
 * fire_cmd_build_json()으로 만든 JSON을 내부 토픽에 발행한다. */
guardx_err_t mqtt_bus_publish_fire_cmd(const fire_cmd_msg_t *m);

void         mqtt_bus_cleanup(void);

#endif /* MQTT_BUS_H */
