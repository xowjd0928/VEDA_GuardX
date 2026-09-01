/*
 * mqtt_pub.c - libmosquitto 기반 MQTT 발행
 *
 * QoS 정책 (설계 문서):
 *  - 일반 센서 데이터: QoS 0 (유실 허용, 1초 뒤 다음 데이터가 옴)
 *  - 비상 버튼 로깅:   QoS 2 (정확히 1회 전달 보장)
 *
 * mosquitto_loop_start()로 내부 네트워크 스레드를 돌려
 * 발행이 이벤트 루프를 블로킹하지 않게 한다.
 */

#include <stdio.h>
#include <mosquitto.h>

#include "mqtt_pub.h"

static struct mosquitto *mosq;

/* guardx_protocol.h의 포맷 매크로 + 노드 ID로 조립한 토픽 (init 시 1회) */
static char topic_sensor[128];
static char topic_button[128];

guardx_err_t mqtt_pub_init(void)
{
    int rc;

    /* 공통 프로토콜 헤더의 포맷 매크로로 토픽 조립.
     * 토픽 구조가 바뀌면 guardx_protocol.h만 고치면 여기는 그대로. */
    snprintf(topic_sensor, sizeof(topic_sensor), GUARDX_TOPIC_SENSOR_FMT, GUARDX_NODE_RPIA);
    snprintf(topic_button, sizeof(topic_button), GUARDX_TOPIC_BUTTON_FMT, GUARDX_NODE_RPIA);

    mosquitto_lib_init();

    /* client id = 노드 ID (TLS 사용 시 인증서 CN과도 일치시킬 것) */
    mosq = mosquitto_new(GUARDX_NODE_RPIA, true, NULL);
    if (!mosq) {
        fprintf(stderr, "mqtt: mosquitto_new failed\n");
        return GUARDX_ERR_OPEN;
    }

#if MQTT_USE_TLS
    /* [REAL DEPLOYMENT] mTLS 설정 (포트 8883, CN=rpia) */
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

    /* 재접속·keepalive를 처리하는 내부 스레드 시작 */
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

static guardx_err_t publish(const char *topic, const char *json, int len, int qos)
{
    int rc = mosquitto_publish(mosq, NULL, topic, len, json, qos, false);

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: publish(%s, qos%d) failed: %s\n",
                topic, qos, mosquitto_strerror(rc));
        return GUARDX_ERR_READ;
    }
    return GUARDX_OK;
}

guardx_err_t mqtt_pub_sensor(const char *json, int len)
{
    return publish(topic_sensor, json, len, GUARDX_QOS_SENSOR);
}

guardx_err_t mqtt_pub_button(const char *json, int len)
{
    return publish(topic_button, json, len, GUARDX_QOS_BUTTON);
}

void mqtt_pub_cleanup(void)
{
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    mosquitto_lib_cleanup();
}
