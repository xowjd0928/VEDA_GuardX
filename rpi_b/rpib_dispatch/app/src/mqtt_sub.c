/*
 * mqtt_sub.c - rpib_dispatch MQTT (구독 1개, 발행 1개짜리라 원본
 * mqtt_bus.c보다 훨씬 단순하다)
 */

#include <stdio.h>
#include <string.h>
#include <mosquitto.h>

#include "mqtt_sub.h"
#include "guardx_protocol.h"   /* GUARDX_TOPIC_ACTUATOR_FMT */
#include "sensor_parser.h"     /* GUARDX_JSON_MAX */

static struct mosquitto *mosq;
static mqtt_fire_cmd_handler_t fire_cmd_handler;

static void on_connect(struct mosquitto *m, void *obj, int rc)
{
    (void)obj;

    if (rc != 0) {
        fprintf(stderr, "mqtt: connect refused (rc=%d)\n", rc);
        return;
    }
    rc = mosquitto_subscribe(m, NULL, GUARDX_TOPIC_FIRE_CMD, GUARDX_QOS_FIRE_CMD);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "mqtt: subscribe(%s) failed: %s\n",
                GUARDX_TOPIC_FIRE_CMD, mosquitto_strerror(rc));
    else
        printf("mqtt: subscribed to %s\n", GUARDX_TOPIC_FIRE_CMD);
}

static void on_message(struct mosquitto *m, void *obj,
                       const struct mosquitto_message *msg)
{
    char buf[GUARDX_JSON_MAX];
    fire_cmd_msg_t fm;

    (void)m;
    (void)obj;

    if (msg->payloadlen <= 0 || msg->payloadlen >= (int)sizeof(buf))
        return;
    memcpy(buf, msg->payload, (size_t)msg->payloadlen);
    buf[msg->payloadlen] = '\0';

    if (fire_cmd_parse_json(buf, &fm) != GUARDX_OK) {
        fprintf(stderr, "mqtt: malformed internal fire_command dropped: %s\n", buf);
        return;
    }
    if (fire_cmd_handler)
        fire_cmd_handler(&fm);
}

guardx_err_t mqtt_sub_init(mqtt_fire_cmd_handler_t on_fire_cmd)
{
    int rc;

    if (!on_fire_cmd)
        return GUARDX_ERR_INVALID;
    fire_cmd_handler = on_fire_cmd;

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

guardx_err_t mqtt_sub_publish_actuator(const char *rpic_node,
                                       const char *json, int len)
{
    char topic[128];
    int rc;

    snprintf(topic, sizeof(topic), GUARDX_TOPIC_ACTUATOR_FMT, rpic_node);

    rc = mosquitto_publish(mosq, NULL, topic, len, json, GUARDX_QOS_ACTUATOR, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s) failed: %s\n", topic, mosquitto_strerror(rc));
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
    fire_cmd_handler = NULL;
    mosquitto_lib_cleanup();
}
