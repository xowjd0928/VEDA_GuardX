/*
 * fan_test.c - RPi B에서 RPi C로 팬 제어 명령을 보내는 MQTT 테스트
 *
 * 사용:
 *   ./fan_test on
 *   ./fan_test off
 *   ./fan_test 60
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mosquitto.h>

#define DEFAULT_BROKER_HOST "172.20.33.251"
#define BROKER_PORT         1883
#define MQTT_TOPIC          "guardx/actuator/rpic/fan"
#define MQTT_CLIENT_ID      "rpib_fan_test"
#define MQTT_QOS            1
#define PUBLISH_TIMEOUT_MS  2000

static int publish_done;

static void on_publish(struct mosquitto *mosq, void *userdata, int mid)
{
    (void)mosq;
    (void)userdata;
    (void)mid;
    publish_done = 1;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "사용법: %s <on | off | 0~100>\n", program);
}

static int parse_duty(const char *text, int *duty)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value < 0 || value > 100)
        return -1;

    *duty = (int)value;
    return 0;
}

static long long current_time_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return 0;

    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

int main(int argc, char **argv)
{
    const char *broker_host;
    struct mosquitto *mosq = NULL;
    char payload[256];
    int duty;
    int elapsed_ms = 0;
    int rc;
    int exit_code = EXIT_FAILURE;

    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "on") == 0) {
        snprintf(payload, sizeof(payload),
                 "{\"node_id\":\"rpic\",\"timestamp\":%lld,\"seq\":1,"
                 "\"command\":\"fan\",\"action\":\"ON\"}",
                 current_time_ms());
    } else if (strcmp(argv[1], "off") == 0) {
        snprintf(payload, sizeof(payload),
                 "{\"node_id\":\"rpic\",\"timestamp\":%lld,\"seq\":1,"
                 "\"command\":\"fan\",\"action\":\"OFF\"}",
                 current_time_ms());
    } else if (parse_duty(argv[1], &duty) == 0) {
        snprintf(payload, sizeof(payload),
                 "{\"node_id\":\"rpic\",\"timestamp\":%lld,\"seq\":1,"
                 "\"command\":\"fan\",\"action\":\"SET\",\"value\":%d}",
                 current_time_ms(), duty);
    } else {
        fprintf(stderr, "잘못된 팬 명령입니다: %s\n", argv[1]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    broker_host = getenv("MQTT_HOST");
    if (broker_host == NULL || broker_host[0] == '\0')
        broker_host = DEFAULT_BROKER_HOST;

    rc = mosquitto_lib_init();
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Mosquitto 초기화 실패: %s\n", mosquitto_strerror(rc));
        return EXIT_FAILURE;
    }

    mosq = mosquitto_new(MQTT_CLIENT_ID, true, NULL);
    if (mosq == NULL) {
        fprintf(stderr, "MQTT 클라이언트 생성 실패\n");
        goto cleanup;
    }

    mosquitto_publish_callback_set(mosq, on_publish);

    rc = mosquitto_connect(mosq, broker_host, BROKER_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "브로커 연결 실패(%s:%d): %s\n",
                broker_host, BROKER_PORT, mosquitto_strerror(rc));
        goto cleanup;
    }

    rc = mosquitto_publish(mosq, NULL, MQTT_TOPIC,
                           (int)strlen(payload), payload, MQTT_QOS, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "팬 명령 발행 실패: %s\n", mosquitto_strerror(rc));
        goto disconnect;
    }

    while (!publish_done && elapsed_ms < PUBLISH_TIMEOUT_MS) {
        rc = mosquitto_loop(mosq, 100, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "MQTT 전송 처리 실패: %s\n",
                    mosquitto_strerror(rc));
            goto disconnect;
        }
        elapsed_ms += 100;
    }

    if (!publish_done) {
        fprintf(stderr, "팬 명령 전송 확인 시간 초과\n");
        goto disconnect;
    }

    printf("팬 명령 전송 완료: %s\n", payload);
    exit_code = EXIT_SUCCESS;

disconnect:
    mosquitto_disconnect(mosq);
cleanup:
    if (mosq != NULL)
        mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return exit_code;
}
