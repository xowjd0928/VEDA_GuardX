/*
 * main.c - GuardX RPi C 액추에이터 서브스크라이버 메인
 *
 * 구조 (RPi A 퍼블리셔와의 대칭):
 *
 *   RPi A: poll 루프가 주기적으로 "읽어서 발행" (능동, 1Hz)
 *   RPi C: mosquitto 콜백이 명령 올 때만 "수신해서 구동" (수동, 이벤트)
 *
 * 명령 수신/실행은 전부 mosquitto 내부 스레드의 on_command()에서
 * 일어나고, main 스레드는 초기화 후 종료 시그널만 기다린다.
 * 콜백 스레드가 하나뿐이라 명령 처리는 자연히 직렬화되며,
 * main 스레드와 HAL이 겹치는 구간은 시작(OPEN_ALL)/종료(CLOSE_ALL)
 * 뿐이다. 종료 시 mqtt_sub_cleanup()을 먼저 불러 콜백 스레드를
 * 세운 뒤 CLOSE_ALL() 하므로 락 없이 안전하다 - 이 순서를 바꾸면
 * 닫힌 fd에 콜백이 write하는 경합이 생기니 주의.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <time.h>

#include "guardx_hal.h"
#include "actuator_registry.h"
#include "audio_arbiter.h"
#include "audio_ref.h"
#include "fan_control.h"
#include "cmd_parser.h"
#include "matrix_link.h"
#include "mqtt_sub.h"

static volatile sig_atomic_t running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

#if RPIC_ENABLE_ACK
/* epoch 기준 밀리초 타임스탬프 — RPi A main.c와 동일 패턴(json timestamp 필드) */
static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* 액추에이터 명령 수신 핸들러 (mosquitto 콜백 스레드에서 실행).
 * 잘못된 payload 한 건이 프로세스를 세우면 안 되므로, 파싱/실행
 * 실패는 로그만 남기고 리턴한다 (RPi A READ_ALL의 부분 실패 격리와
 * 같은 정책). */
static void on_command(const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    actuator_cmd_t cmd;
    guardx_err_t ret;

    /* mosquitto payload는 NUL 종료 보장이 없어 복사 후 종료 처리 */
    if (len >= (int)sizeof(buf)) {
        fprintf(stderr, "main: payload too large (%d), dropped\n", len);
        return;
    }
    memcpy(buf, payload, len);
    buf[len] = '\0';

    if (PARSE_CMD_JSON(buf, &cmd) != GUARDX_OK) {
        fprintf(stderr, "main: malformed command dropped: %s\n", buf);
        return;
    }

    ret = DISPATCH_CMD(&cmd);
    if (ret == GUARDX_OK) {
        if (cmd.has_value)
            printf("main: %s %s %d ok\n", cmd.command, cmd.action, cmd.value);
        else
            printf("main: %s %s ok\n", cmd.command, cmd.action);
    } else {
        fprintf(stderr, "main: %s %s failed (%d)\n",
                cmd.command, cmd.action, ret);
    }

#if RPIC_ENABLE_ACK
    /* 프로토콜 4-4 ACK. task_vms.cpp가 보낸 seq를 그대로 echo해 VMS가
     * 요청-응답을 짝짓게 하고, action/value까지 실어서 "이 명령이 뭘
     * 했는지"를 VMS가 재구성할 수 있게 한다 — command만으로는 상태
     * 라벨을 못 채운다(ON/OFF/OPEN/CLOSE/STOP 중 뭐였는지 모른다). */
    {
        char ack[GUARDX_JSON_MAX];
        char seq_part[32] = "";
        char value_part[32] = "";

        if (cmd.has_seq)
            snprintf(seq_part, sizeof(seq_part), ",\"seq\":%ld", cmd.seq);
        if (cmd.has_value)
            snprintf(value_part, sizeof(value_part), ",\"value\":%d", cmd.value);

        int n = snprintf(ack, sizeof(ack),
            "{\"node_id\":\"" GUARDX_NODE_RPIC "\",\"timestamp\":%llu%s,"
            "\"command\":\"%s\",\"action\":\"%s\"%s,\"result\":\"%s\"}",
            (unsigned long long)now_ms(), seq_part,
            cmd.command, cmd.action, value_part,
            (ret == GUARDX_OK) ? "ok" : "fail");
        mqtt_sub_publish_ack(ack, n);
    }
#endif
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 1) 액추에이터 초기화 - 하나라도 실패하면 기동 실패 */
    if (OPEN_ALL() != GUARDX_OK) {
        fprintf(stderr, "main: OPEN_ALL failed, aborting\n");
        return 1;
    }

    /* 팬 자동 제어 장부를 안전 상태(0%)에 맞춘다. OPEN_ALL 이 이미 팬을
     * 꺼두었으므로 여기서는 그 사실을 기록하고 상태만 발행한다. */
    (void)fan_control_init();

    /* LED 매트릭스는 부가기능이라 실패가 액추에이터 기동을 막지 않는다.
     * 시리얼이 없는 개발 PC나 modbus_test로 포트를 쓰는 중에도 구독자는
     * 정상 기동해야 한다. */
    if (matrix_link_init() != GUARDX_OK)
        fprintf(stderr, "main: matrix link disabled (serial unavailable)\n");

    /* 스피커를 방송과 사이렌이 나눠 쓰는 규칙. 여기서 실패하면 사이렌 "반복"만
     * 안 되고 단발 상황음은 그대로 나므로 기동은 막지 않는다. */
    if (audio_arbiter_init() != GUARDX_OK)
        fprintf(stderr, "main: audio arbiter disabled (worker init failed)\n");

    /* 스피커로 내보내는 소리의 사본을 TOIMIC 에 넘기는 경로. 감지기가
     * 방송·사이렌 중에도 비명과 총성을 잡으려면 이 신호가 필요하다.
     * 실패해도 재생에는 영향이 없다 - 감지기가 예전처럼 재생 중에만
     * 감지를 멈추는 상태로 돌아갈 뿐이다. */
    if (audio_ref_init() != GUARDX_OK)
        fprintf(stderr, "main: speaker reference stream disabled\n");

    /* 2) MQTT 연결 + 구독 (수신 콜백 등록) */
    if (mqtt_sub_init(on_command) != GUARDX_OK) {
        fprintf(stderr, "main: mqtt init failed, aborting\n");
        audio_arbiter_cleanup();
        matrix_link_cleanup();
        CLOSE_ALL();
        return 1;
    }

    printf("main: rpic subscriber started\n");

    /* main 스레드는 종료 시그널 대기만. poll(NULL, 0, ...)은 시그널에
     * EINTR로 즉시 깨어나므로 종료 지연이 최대 1초를 넘지 않는다. */
    while (running)
        poll(NULL, 0, 1000);

    printf("main: shutting down\n");
    mqtt_sub_cleanup();   /* 콜백 스레드 먼저 정지 (상단 주석 참조) */
    audio_arbiter_cleanup();   /* 사이렌 반복을 멈춘 뒤 오디오 워커를 내린다 */
    audio_ref_cleanup();
    matrix_link_cleanup();
    CLOSE_ALL();          /* 안전 상태 적용 후 해제 */
    return 0;
}
