/*
 * mqtt_bus.c - libmosquitto 기반 구독(센서) + 발행(액추에이터)
 *
 * QoS 정책 (프로토콜 규약 2-1):
 *  - 센서 구독:      QoS 0 (유실 허용 - 1초 뒤 다음 값이 옴)
 *  - 버튼 구독:      QoS 2 (로그 정확히 1회 - DB 중복 기록 금지)
 *  - 액추에이터 발행: QoS 1 (멱등 명령 - 유실 불가, 중복 허용)
 *
 * 구독은 on_connect 안에서 수행 - 재접속 시 자동 재구독 (RPi C
 * mqtt_sub.c와 동일 패턴).
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <mosquitto.h>

#include "mqtt_bus.h"

static struct mosquitto *mosq;
static mqtt_sensor_handler_t sensor_handler;
static mqtt_button_handler_t button_handler;
static mqtt_config_handler_t config_handler;
static mqtt_clear_fire_handler_t clear_fire_handler;

/* PHASE 6: 센서/버튼 토픽은 이제 와일드카드다 - 어느 zone(node_id)의
 * 메시지가 올지 미리 정해져 있지 않기 때문. GUARDX_TOPIC_SENSOR_FMT가
 * "guardx/sensor/%s"이므로 %s 자리에 MQTT 단일 레벨 와일드카드 '+'를
 * 넣으면 그대로 재사용된다 - 새 상수를 따로 정의할 필요가 없다.
 * config 토픽만 예전처럼 고정(RPi B 자신을 향한 신호라 여럿일 이유가
 * 없음). 액추에이터는 발행 시점에 대상 zone의 노드로 매번 새로 조립하므로
 * 여기 전역 버퍼가 없다(mqtt_bus_publish_actuator 참조). */
static char topic_sensor_wild[128];
static char topic_button_wild[128];
static char topic_config[128];
static char topic_clear_fire[128];

/* "guardx/sensor/" 다음, "/button" 앞의 조각(=node_id)을 뽑는다.
 * 성공하면 out에 NUL 종료로 채우고 true, 실패(프리픽스 불일치 등)면
 * false. sensor_topic이면 is_button=false로, button_topic이면 true로
 * 판정해서 리턴한다 - on_message가 두 경우를 한 번에 처리하려고 둠. */
static bool parse_node_id(const char *topic, char *out, size_t out_sz,
                          bool *is_button)
{
    static const char PREFIX[] = "guardx/sensor/";
    static const char SUFFIX[] = "/button";
    const size_t prefix_len = sizeof(PREFIX) - 1;
    const size_t suffix_len = sizeof(SUFFIX) - 1;
    size_t topic_len, node_len;
    const char *node_start;

    if (strncmp(topic, PREFIX, prefix_len) != 0)
        return false;

    node_start = topic + prefix_len;
    topic_len = strlen(node_start);

    if (topic_len > suffix_len &&
        strcmp(node_start + topic_len - suffix_len, SUFFIX) == 0) {
        node_len = topic_len - suffix_len;
        *is_button = true;
    } else {
        node_len = topic_len;
        *is_button = false;
    }

    /* node_id 안에 '/'가 남아있으면 우리가 아는 두 패턴 중 어느 것도
     * 아니다(예상 못한 하위토픽) - 무시. */
    if (node_len == 0 || node_len >= out_sz ||
        memchr(node_start, '/', node_len) != NULL)
        return false;

    memcpy(out, node_start, node_len);
    out[node_len] = '\0';
    return true;
}

static void on_connect(struct mosquitto *m, void *obj, int rc)
{
    (void)obj;

    if (rc != 0) {
        fprintf(stderr, "mqtt: connect refused (rc=%d)\n", rc);
        return;
    }

    rc = mosquitto_subscribe(m, NULL, topic_sensor_wild, GUARDX_QOS_SENSOR);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: subscribe(%s) failed: %s\n",
                topic_sensor_wild, mosquitto_strerror(rc));

    rc = mosquitto_subscribe(m, NULL, topic_button_wild, GUARDX_QOS_BUTTON);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: subscribe(%s) failed: %s\n",
                topic_button_wild, mosquitto_strerror(rc));

    rc = mosquitto_subscribe(m, NULL, topic_config, GUARDX_QOS_CONFIG);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: subscribe(%s) failed: %s\n",
                topic_config, mosquitto_strerror(rc));

    rc = mosquitto_subscribe(m, NULL, topic_clear_fire, GUARDX_QOS_CLEAR_FIRE);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: subscribe(%s) failed: %s\n",
                topic_clear_fire, mosquitto_strerror(rc));

    printf("mqtt: subscribed to %s (qos%d), %s (qos%d), %s (qos%d), %s (qos%d)\n",
           topic_sensor_wild, GUARDX_QOS_SENSOR,
           topic_button_wild, GUARDX_QOS_BUTTON,
           topic_config, GUARDX_QOS_CONFIG,
           topic_clear_fire, GUARDX_QOS_CLEAR_FIRE);
}

static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg)
{
    char node_id[32];
    bool is_button;

    (void)m;
    (void)obj;

    if (msg->payloadlen <= 0)
        return;

    /* config/clear_fire는 프리픽스가 완전히 달라 parse_node_id에 안 걸린다 -
     * 먼저 걸러서 나머지는 센서/버튼 경로 하나로 처리한다. */
    if (strcmp(msg->topic, topic_config) == 0) {
        if (config_handler)
            config_handler((const char *)msg->payload, msg->payloadlen);
        return;
    }

    if (strcmp(msg->topic, topic_clear_fire) == 0) {
        if (clear_fire_handler)
            clear_fire_handler((const char *)msg->payload, msg->payloadlen);
        return;
    }

    if (!parse_node_id(msg->topic, node_id, sizeof(node_id), &is_button)) {
        fprintf(stderr, "mqtt: 인식 못한 토픽 무시: %s\n", msg->topic);
        return;
    }

    if (is_button) {
        if (button_handler)
            button_handler(node_id, (const char *)msg->payload, msg->payloadlen);
    } else {
        if (sensor_handler)
            sensor_handler(node_id, (const char *)msg->payload, msg->payloadlen);
    }
}

guardx_err_t mqtt_bus_init(mqtt_sensor_handler_t on_sensor,
                           mqtt_button_handler_t on_button,
                           mqtt_config_handler_t on_config,
                           mqtt_clear_fire_handler_t on_clear_fire)
{
    int rc;

    if (on_sensor == NULL || on_button == NULL || on_config == NULL ||
        on_clear_fire == NULL)
        return GUARDX_ERR_INVALID;
    sensor_handler = on_sensor;
    button_handler = on_button;
    config_handler = on_config;
    clear_fire_handler = on_clear_fire;

    snprintf(topic_sensor_wild, sizeof(topic_sensor_wild),
             GUARDX_TOPIC_SENSOR_FMT, "+");
    snprintf(topic_button_wild, sizeof(topic_button_wild),
             GUARDX_TOPIC_BUTTON_FMT, "+");
    snprintf(topic_config, sizeof(topic_config),
             GUARDX_TOPIC_CONFIG_FMT, GUARDX_NODE_RPIB);
    snprintf(topic_clear_fire, sizeof(topic_clear_fire),
             GUARDX_TOPIC_CLEAR_FIRE_FMT, GUARDX_NODE_RPIB);

    mosquitto_lib_init();

    /* client id = 노드 ID (TLS 사용 시 인증서 CN과도 일치시킬 것) */
    mosq = mosquitto_new(GUARDX_NODE_RPIB, true, NULL);
    if (!mosq) {
        fprintf(stderr, "mqtt: mosquitto_new failed\n");
        return GUARDX_ERR_OPEN;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

#if MQTT_USE_TLS
    /* [REAL DEPLOYMENT] mTLS 설정 (포트 8883, CN=rpib) */
    rc = mosquitto_tls_set(mosq, MQTT_TLS_CA_PATH, NULL,
                           MQTT_TLS_CERT_PATH, MQTT_TLS_KEY_PATH, NULL);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: tls_set failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        return GUARDX_ERR_OPEN;
    }
#endif

    rc = mosquitto_connect(mosq, MQTT_BROKER_HOST, MQTT_BROKER_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: connect failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        return GUARDX_ERR_OPEN;
    }

    /* 재접속·keepalive·수신을 처리하는 내부 스레드 시작 */
    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: loop_start failed: %s\n", mosquitto_strerror(rc));
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        return GUARDX_ERR_OPEN;
    }

    printf("mqtt: connected to %s:%d (tls=%d)\n",
           MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USE_TLS);
    return GUARDX_OK;
}

guardx_err_t mqtt_bus_publish_actuator(const char *rpic_node_id,
                                       const char *json, int len)
{
    char topic_actuator[128];
    int rc;

    /* zone마다 다른 RPi C로 보내야 하므로 고정 버퍼가 아니라 호출마다
     * 조립한다(PHASE 6) - 발행 빈도가 낮아(화재 대응 시나리오뿐) 매번
     * snprintf 비용은 무시할 만하다. */
    snprintf(topic_actuator, sizeof(topic_actuator),
             GUARDX_TOPIC_ACTUATOR_FMT, rpic_node_id);

    rc = mosquitto_publish(mosq, NULL, topic_actuator, len, json,
                           GUARDX_QOS_ACTUATOR, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s, qos%d) failed: %s\n",
                topic_actuator, GUARDX_QOS_ACTUATOR, mosquitto_strerror(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

guardx_err_t mqtt_bus_publish_alert(const char *topic, const char *json, int len)
{
    /* QoS1 고정 - 액추에이터 발행과 같은 신뢰도(멱등 아님이라 유실은 곤란하나,
     * 중복 수신은 VMS가 timestamp/event_type으로 걸러도 무해하다). */
    int rc = mosquitto_publish(mosq, NULL, topic, len, json, 1, false);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s, qos1) failed: %s\n",
                topic, mosquitto_strerror(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

void mqtt_bus_cleanup(void)
{
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    sensor_handler = NULL;
    button_handler = NULL;
    config_handler = NULL;
    clear_fire_handler = NULL;
    mosquitto_lib_cleanup();
}
