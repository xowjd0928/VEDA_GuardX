#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include "guardx_hal.h"
#include "device_registry.h"
#include "json_builder.h"
#include "mqtt_pub.h"

static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

int main(void) {
    sensor_snapshot_t snap;
    char json[GUARDX_JSON_MAX];
    int len;

    if (OPEN_ALL() != GUARDX_OK) { fprintf(stderr, "OPEN_ALL fail\n"); return 1; }
    if (mqtt_pub_init() != GUARDX_OK) { fprintf(stderr, "mqtt fail\n"); return 1; }

    READ_ALL(&snap);
    len = CREATE_JSON(json, sizeof(json), &snap, now_ms(), 0);
    printf("payload: %s\n", json);
    if (mqtt_pub_sensor(json, len) != GUARDX_OK) return 1;

    /* 버튼 이벤트 JSON도 QoS2로 1건 발행 */
    len = CREATE_BUTTON_JSON(json, sizeof(json), 1, now_ms(), 1);
    if (mqtt_pub_button(json, len) != GUARDX_OK) return 1;

    /* QoS2 핸드셰이크 완료 대기 */
    struct timespec d = {1, 0}; nanosleep(&d, NULL);
    mqtt_pub_cleanup();
    CLOSE_ALL();
    printf("integration test OK\n");
    return 0;
}
