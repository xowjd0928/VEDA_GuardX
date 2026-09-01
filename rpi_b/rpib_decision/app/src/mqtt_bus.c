/*
 * mqtt_bus.c - rpib_decision MQTT (mqtt_bus.c 원본에서 버튼/액추에이터
 * 발행을 빼고, 내부 fire_command 발행을 더한 버전)
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <mosquitto.h>

#include "mqtt_bus.h"
#include "sensor_parser.h"   /* GUARDX_JSON_MAX */

static struct mosquitto *mosq;
static mqtt_sensor_handler_t sensor_handler;
static mqtt_config_handler_t config_handler;
static mqtt_clear_fire_handler_t clear_fire_handler;

static char topic_sensor_wild[128];
static char topic_config[128];
static char topic_clear_fire[128];

/* "guardx/sensor/" 다음 조각(=node_id)만 뽑는다 - 버튼 서픽스는 여기선
 * 안 걸러도 된다(이 프로세스는 버튼을 구독 안 하므로 도착할 일이 없다,
 * 만에 하나 와도 아래에서 그냥 무시됨). */
static bool parse_node_id(const char *topic, char *out, size_t out_sz)
{
    static const char PREFIX[] = "guardx/sensor/";
    const size_t prefix_len = sizeof(PREFIX) - 1;
    const char *node_start;
    size_t node_len;

    if (strncmp(topic, PREFIX, prefix_len) != 0)
        return false;

    node_start = topic + prefix_len;
    node_len = strlen(node_start);

    if (node_len == 0 || node_len >= out_sz ||
        memchr(node_start, '/', node_len) != NULL)
        return false;   /* "/button" 등 하위토픽 - 이 프로세스 관심사 아님 */

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

    mosquitto_subscribe(m, NULL, topic_sensor_wild, GUARDX_QOS_SENSOR);
    mosquitto_subscribe(m, NULL, topic_config, GUARDX_QOS_CONFIG);
    mosquitto_subscribe(m, NULL, topic_clear_fire, GUARDX_QOS_CLEAR_FIRE);

    printf("mqtt: subscribed to %s, %s, %s\n",
           topic_sensor_wild, topic_config, topic_clear_fire);
}

static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg)
{
    char node_id[32];

    (void)m;
    (void)obj;

    if (msg->payloadlen <= 0)
        return;

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

    if (parse_node_id(msg->topic, node_id, sizeof(node_id))) {
        if (sensor_handler)
            sensor_handler(node_id, (const char *)msg->payload, msg->payloadlen);
    }
}

guardx_err_t mqtt_bus_init(mqtt_sensor_handler_t on_sensor,
                           mqtt_config_handler_t on_config,
                           mqtt_clear_fire_handler_t on_clear_fire)
{
    int rc;

    if (!on_sensor || !on_config || !on_clear_fire)
        return GUARDX_ERR_INVALID;
    sensor_handler = on_sensor;
    config_handler = on_config;
    clear_fire_handler = on_clear_fire;

    snprintf(topic_sensor_wild, sizeof(topic_sensor_wild),
             GUARDX_TOPIC_SENSOR_FMT, "+");
    snprintf(topic_config, sizeof(topic_config),
             GUARDX_TOPIC_CONFIG_FMT, GUARDX_NODE_RPIB);
    snprintf(topic_clear_fire, sizeof(topic_clear_fire),
             GUARDX_TOPIC_CLEAR_FIRE_FMT, GUARDX_NODE_RPIB);

    mosquitto_lib_init();

    mosq = mosquitto_new(MQTT_CLIENT_ID, true, NULL);
    if (!mosq) {
        fprintf(stderr, "mqtt: mosquitto_new failed\n");
        return GUARDX_ERR_OPEN;
    }

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

#if MQTT_USE_TLS
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

    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: loop_start failed: %s\n", mosquitto_strerror(rc));
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        return GUARDX_ERR_OPEN;
    }

    printf("mqtt: %s connected to %s:%d (tls=%d)\n",
           MQTT_CLIENT_ID, MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USE_TLS);
    return GUARDX_OK;
}

guardx_err_t mqtt_bus_publish_alert(const char *topic, const char *json, int len)
{
    int rc = mosquitto_publish(mosq, NULL, topic, len, json, 1, false);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s, qos1) failed: %s\n",
                topic, mosquitto_strerror(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

guardx_err_t mqtt_bus_publish_display(const char *topic, const char *json, int len)
{
    /* retain=true - 이유는 mqtt_bus.h 선언부 주석 참조. */
    int rc = mosquitto_publish(mosq, NULL, topic, len, json, 1, true);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s, qos1, retain) failed: %s\n",
                topic, mosquitto_strerror(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

guardx_err_t mqtt_bus_publish_fire_cmd(const fire_cmd_msg_t *m)
{
    char json[GUARDX_JSON_MAX];
    int len = fire_cmd_build_json(json, sizeof(json), m);
    int rc;

    if (len <= 0 || len >= (int)sizeof(json))
        return GUARDX_ERR_INVALID;

    rc = mosquitto_publish(mosq, NULL, GUARDX_TOPIC_FIRE_CMD, len, json,
                           GUARDX_QOS_FIRE_CMD, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s) failed: %s\n",
                GUARDX_TOPIC_FIRE_CMD, mosquitto_strerror(rc));
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
    config_handler = NULL;
    clear_fire_handler = NULL;
    mosquitto_lib_cleanup();
}
