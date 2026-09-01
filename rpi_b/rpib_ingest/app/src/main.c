/*
 * main.c - rpib_ingest: guardx/sensor/+(/button) 원시값 수집·기록
 *
 * rpib_engine 3분할 중 A->B 구간 담당. 판정은 안 한다(rpib_decision
 * 몫) - 받은 그대로 sensor_reading/sensor_value/button_event에 UPSERT.
 * composite_score는 안 채운다(NULL로 시작, rpib_decision이 나중에
 * UPDATE) - db_writer.h 주석 참조.
 *
 * 화재 판정과 무관하므로 decision.c/threshold_loader.c를 안 쓴다 -
 * 이 프로세스가 죽어도(또는 느려져도) rpib_decision의 화재 대응은
 * 안 막힌다(반대도 마찬가지).
 */

#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <poll.h>

#include "db_writer.h"
#include "mqtt_sub.h"
#include "sensor_parser.h"
#include "zone_loader.h"

static volatile sig_atomic_t running = 1;

static fire_zone_t g_zones[MAX_FIRE_ZONES];
static int g_zone_count;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static const fire_zone_t *find_zone_by_rpia(const char *node_id)
{
    int i;
    for (i = 0; i < g_zone_count; i++)
        if (strcmp(g_zones[i].rpia_node_id, node_id) == 0)
            return &g_zones[i];
    return NULL;
}

static void on_sensor(const char *node_id, const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    sensor_msg_t msg;
    const fire_zone_t *z = find_zone_by_rpia(node_id);

    if (!z) {
        fprintf(stderr, "main: 미등록 노드 '%s' 센서 메시지 무시\n", node_id);
        return;
    }
    if (len >= (int)sizeof(buf)) {
        fprintf(stderr, "main: sensor payload too large (%d), dropped\n", len);
        return;
    }
    memcpy(buf, payload, (size_t)len);
    buf[len] = '\0';

    if (PARSE_SENSOR_JSON(buf, &msg) != GUARDX_OK) {
        fprintf(stderr, "main: malformed sensor payload dropped: %s\n", buf);
        return;
    }

    if (db_write_sensor_raw(&msg, now_ms(), z->zone_id) != GUARDX_OK)
        fprintf(stderr, "main: zone %d sensor write failed (seq=%u)\n",
                z->zone_id, msg.seq);
}

static void on_button(const char *node_id, const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    button_msg_t msg;
    const fire_zone_t *z = find_zone_by_rpia(node_id);

    if (!z) {
        fprintf(stderr, "main: 미등록 노드 '%s' 버튼 메시지 무시\n", node_id);
        return;
    }
    if (len >= (int)sizeof(buf))
        return;
    memcpy(buf, payload, (size_t)len);
    buf[len] = '\0';

    if (PARSE_BUTTON_JSON(buf, &msg) != GUARDX_OK) {
        fprintf(stderr, "main: malformed button payload dropped: %s\n", buf);
        return;
    }

    if (db_write_button(&msg, now_ms(), z->zone_id) != GUARDX_OK)
        fprintf(stderr, "main: zone %d button write failed\n", z->zone_id);
    printf("main: zone %d emergency button logged (count=%u)\n",
           z->zone_id, msg.press_count);

    /* 규약 4-2: 이 토픽은 로깅 전용, 실제 제어는 A->C 하드웨어 인터락이
     * 이미 처리했다(main.c 원본 on_button과 동일 근거) - 판정 로직에
     * 넣지 않는다. VMS 경보만 여기서 낸다. */
    {
        char json[GUARDX_JSON_MAX];
        int n = snprintf(json, sizeof(json),
            "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"zone_id\":%d,"
            "\"press_count\":%u}",
            (unsigned long long)now_ms(), z->zone_id, msg.press_count);
        mqtt_sub_publish_alert("guardx/alert/button", json, n);
    }
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

    if (zone_loader_load(g_zones, &g_zone_count) != GUARDX_OK) {
        fprintf(stderr, "main: zone mapping load failed, aborting\n");
        db_writer_close();
        return 1;
    }

    if (mqtt_sub_init(on_sensor, on_button) != GUARDX_OK) {
        fprintf(stderr, "main: mqtt init failed, aborting\n");
        db_writer_close();
        return 1;
    }

    printf("main: rpib_ingest started - %d zone(s)\n", g_zone_count);

    while (running)
        poll(NULL, 0, 1000);

    printf("main: shutting down\n");
    mqtt_sub_cleanup();
    db_writer_close();
    return 0;
}
