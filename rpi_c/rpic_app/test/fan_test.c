/*
 * fan_test.c - GuardX RPi C 팬 전용 간단 테스트
 *
 * MQTT로 팬 명령만 하나 보낸다. RPi C에서 rpic_subscriber가 켜져 있어야 한다.
 *
 * 빌드:
 *   gcc -O2 -Wall fan_test.c -o fan_test -lmosquitto
 *
 * 사용:
 *   ./fan_test 60      # 팬 60%
 *   ./fan_test 0       # 팬 정지(0%)
 *   ./fan_test on      # 팬 켜기(기본 듀티)
 *   ./fan_test off     # 팬 끄기
 *
 * 브로커 IP: 아래 BROKER 수정, 또는  MQTT_HOST=192.168.0.10 ./fan_test 60
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mosquitto.h>

#define BROKER "172.20.33.251"          /* RPi B(브로커) IP - 필요시 이 줄만 수정 */
#define PORT   1883
#define TOPIC  "guardx/actuator/rpic"

static int pub_mid = -1;
static volatile int acked = 0;

static void on_publish(struct mosquitto *m, void *u, int mid)
{
    (void)m; (void)u;
    if (mid == pub_mid)
        acked = 1;
}

int main(int argc, char **argv)
{
    const char *broker = getenv("MQTT_HOST");
    struct mosquitto *mosq;
    char payload[256];
    struct timespec ts;
    long long now_ms;
    int i;

    if (argc < 2) {
        printf("사용법: %s <0~100 | on | off>\n", argv[0]);
        printf("예:   %s 60     # 팬 60%%\n", argv[0]);
        printf("      %s off    # 팬 끄기\n", argv[0]);
        return 1;
    }
    if (!broker || !broker[0])
        broker = BROKER;

    /* 명령 -> JSON payload */
    clock_gettime(CLOCK_REALTIME, &ts);
    now_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    if (strcmp(argv[1], "off") == 0)
        snprintf(payload, sizeof(payload),
            "{\"node_id\":\"rpic\",\"timestamp\":%lld,\"seq\":1,"
            "\"command\":\"fan\",\"action\":\"OFF\"}", now_ms);
    else if (strcmp(argv[1], "on") == 0)
        snprintf(payload, sizeof(payload),
            "{\"node_id\":\"rpic\",\"timestamp\":%lld,\"seq\":1,"
            "\"command\":\"fan\",\"action\":\"ON\"}", now_ms);
    else
        snprintf(payload, sizeof(payload),
            "{\"node_id\":\"rpic\",\"timestamp\":%lld,\"seq\":1,"
            "\"command\":\"fan\",\"action\":\"SET\",\"value\":%d}",
            now_ms, atoi(argv[1]));

    /* 연결 -> 발행 -> 전송 확인 -> 종료 */
    mosquitto_lib_init();
    mosq = mosquitto_new("fan_tester", true, NULL);
    if (!mosq) {
        fprintf(stderr, "mosquitto_new 실패\n");
        mosquitto_lib_cleanup();
        return 1;
    }
    mosquitto_publish_callback_set(mosq, on_publish);

    if (mosquitto_connect(mosq, broker, PORT, 60) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "브로커(%s:%d) 접속 실패 - IP/네트워크 확인\n", broker, PORT);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }
    mosquitto_loop_start(mosq);

    printf("팬 명령 전송: %s  (브로커 %s)\n", argv[1], broker);
    mosquitto_publish(mosq, &pub_mid, TOPIC, (int)strlen(payload), payload, 1, false);

    {
        struct timespec ten_ms = { 0, 10 * 1000 * 1000 };   /* 10ms */
        for (i = 0; i < 200 && !acked; i++)   /* 최대 2초 대기 */
            nanosleep(&ten_ms, NULL);
    }

    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, false);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    printf("완료\n");
    return 0;
}
