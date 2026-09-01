/*
 * main.c - GuardX RPi A 센서 퍼블리셔 메인
 *
 * 이벤트 루프 구조 (설계 문서):
 *
 *   poll(fds, 1, 1000)  // 버튼 fd 감시, 최대 1초 대기
 *   ├─ 버튼 이벤트(POLLIN) → 즉시 read + QoS2 발행 (로깅 경로)
 *   └─ 타임아웃(1초)      → READ_ALL → CREATE_JSON → QoS0 발행
 *
 * 주기 센서 폴링(1Hz)과 비상 버튼 이벤트를 하나의 poll() 루프로
 * 통합해 스레드 없이 처리한다.
 *
 * NOTE: 버튼 이벤트가 연달아 오면 1초 타임아웃이 계속 리셋되어
 * 센서 발행이 지연될 수 있다. 실제 상황에서 버튼이 초당 수회씩
 * 계속 눌리는 시나리오는 없다고 보고 단순한 구조를 택했다.
 * 문제가 되면 clock 기반으로 남은 시간을 계산해 timeout에 넣는
 * 방식으로 개선할 것.
 */

#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <stdbool.h>

#include "guardx_hal.h"
#include "device_registry.h"
#include "json_builder.h"
#include "mqtt_pub.h"

#define POLL_TIMEOUT_MS 1000   /* 센서 보고 주기 1Hz */

static volatile sig_atomic_t running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* epoch 기준 밀리초 타임스탬프 (JSON timestamp 필드) */
static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    struct pollfd fds[1];
    sensor_snapshot_t snap;
    char json[GUARDX_JSON_MAX];
    int len, ret;
    uint32_t seq = 0;   /* 프로세스 시작 시 0, 매 발행 +1 (재시작 시 리셋) */

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1) 주기 폴링 센서 초기화 - 하나라도 실패하면 기동 실패 */
    if (OPEN_ALL() != GUARDX_OK) {
        fprintf(stderr, "main: OPEN_ALL failed, aborting\n");
        return 1;
    }

    /* 2) 비상 버튼 (registry 미포함, 이벤트 기반이라 별도 open) */
    if (rpia_button_open() != GUARDX_OK) {
        fprintf(stderr, "main: button open failed, aborting\n");
        CLOSE_ALL();
        return 1;
    }

    /* 3) MQTT 연결 */
    if (mqtt_pub_init() != GUARDX_OK) {
        fprintf(stderr, "main: mqtt init failed, aborting\n");
        rpia_button_close();
        CLOSE_ALL();
        return 1;
    }

    fds[0].fd     = rpia_button_get_fd();
    fds[0].events = POLLIN;

    printf("main: rpia publisher started\n");

    while (running) {
        ret = poll(fds, 1, POLL_TIMEOUT_MS);

        if (ret < 0) {
            /* 시그널로 인한 EINTR이면 running 플래그 확인 후 계속 */
            continue;
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            /* --- 버튼 이벤트: 즉시 QoS2 발행 (로깅 경로) --- */
            button_data_t presses;

            if (rpia_button_read(&presses) == GUARDX_OK && presses > 0) {
                len = CREATE_BUTTON_JSON(json, sizeof(json), presses,
                                          now_ms(), seq++);
                if (mqtt_pub_button(json, len) == GUARDX_OK)
                    printf("main: button event published (count=%u)\n", presses);
            }
        } else {
            /* --- 타임아웃(1초): 주기 센서 폴링 → QoS0 발행 --- */
            READ_ALL(&snap);
            len = CREATE_JSON(json, sizeof(json), &snap, now_ms(), seq++);
            mqtt_pub_sensor(json, len);
        }
    }

    printf("main: shutting down\n");
    mqtt_pub_cleanup();
    rpia_button_close();
    CLOSE_ALL();
    return 0;
}
