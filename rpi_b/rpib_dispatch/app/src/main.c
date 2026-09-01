/*
 * main.c - rpib_dispatch: 내부 명령 수신 -> RPi C 실제 발행 + 감사 기록
 *
 * rpib_engine 3분할 중 B->C 구간 담당. 무상태 실행기 - rpib_decision이
 * 이미 "무엇을 누구에게" 결정해 보내주므로(fire_internal.h), 여기선
 * cmd_builder.c로 RPi C용 JSON을 조립해 발행하고, 성공한 것만
 * fire_event_command에 남긴다(원본 pub_action/pub_set과 같은 정책 -
 * 발행 실패를 기록하면 "실제로 나간 명령"이라는 사실과 어긋난다).
 */

#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <poll.h>
#include <time.h>

#include "cmd_builder.h"
#include "db_writer.h"
#include "mqtt_sub.h"

static volatile sig_atomic_t running = 1;
static uint32_t pub_seq;   /* 이 프로세스 기준 발행 seq - 원본과 같은 규약(프로세스별 단조증가) */

static void sig_handler(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void on_fire_cmd(const fire_cmd_msg_t *m)
{
    char json[GUARDX_JSON_MAX];
    uint32_t seq = pub_seq++;
    int len;

    if (m->has_value)
        len = CREATE_CMD_SET_JSON(json, sizeof(json), m->command, m->value,
                                  now_ms(), seq, m->cause, m->trigger_seq);
    else
        len = CREATE_CMD_ACTION_JSON(json, sizeof(json), m->command, m->action,
                                     now_ms(), seq, m->cause, m->trigger_seq);

    if (mqtt_sub_publish_actuator(m->rpic_node, json, len) != GUARDX_OK) {
        fprintf(stderr, "main: -> [%s] %s %s 발행 실패, 기록 안 함\n",
                m->rpic_node, m->command, m->action);
        return;
    }

    printf("main: -> [%s] %s %s\n", m->rpic_node, m->command, m->action);
    if (db_write_command(m->event_id, m->command, m->action, m->value,
                         m->has_value, seq) != GUARDX_OK)
        fprintf(stderr, "main: event %lld의 fire_event_command 기록 실패\n",
                m->event_id);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    if (db_writer_open() != GUARDX_OK) {
        fprintf(stderr, "main: db open failed, aborting\n");
        return 1;
    }
    if (mqtt_sub_init(on_fire_cmd) != GUARDX_OK) {
        fprintf(stderr, "main: mqtt init failed, aborting\n");
        db_writer_close();
        return 1;
    }

    printf("main: rpib_dispatch started\n");

    while (running)
        poll(NULL, 0, 1000);

    printf("main: shutting down\n");
    mqtt_sub_cleanup();
    db_writer_close();
    return 0;
}
