/*
 * mqtt_sub.c - rpib_ingest MQTT (mqtt_bus.c의 구독 절반만 남긴 버전)
 * parse_node_id()는 원본과 동일 로직 - 재사용할 정도로 크지 않아 복사.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <mosquitto.h>

#include "mqtt_sub.h"

static struct mosquitto *mosq;
static mqtt_sensor_handler_t sensor_handler;
static mqtt_button_handler_t button_handler;

static char topic_sensor_wild[128];
static char topic_button_wild[128];

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

    printf("mqtt: subscribed to %s (qos%d), %s (qos%d)\n",
           topic_sensor_wild, GUARDX_QOS_SENSOR,
           topic_button_wild, GUARDX_QOS_BUTTON);
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

guardx_err_t mqtt_sub_init(mqtt_sensor_handler_t on_sensor,
                           mqtt_button_handler_t on_button)
{
    int rc;

    if (!on_sensor || !on_button)
        return GUARDX_ERR_INVALID;
    sensor_handler = on_sensor;
    button_handler = on_button;

    snprintf(topic_sensor_wild, sizeof(topic_sensor_wild),
             GUARDX_TOPIC_SENSOR_FMT, "+");
    snprintf(topic_button_wild, sizeof(topic_button_wild),
             GUARDX_TOPIC_BUTTON_FMT, "+");

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

guardx_err_t mqtt_sub_publish_alert(const char *topic, const char *json, int len)
{
    int rc = mosquitto_publish(mosq, NULL, topic, len, json, 1, false);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s, qos1) failed: %s\n",
                topic, mosquitto_strerror(rc));
        return GUARDX_ERR_WRITE;
    }
    return GUARDX_OK;
}

void mqtt_sub_cleanup(void)
{
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    sensor_handler = NULL;
    button_handler = NULL;
    mosquitto_lib_cleanup();
}
