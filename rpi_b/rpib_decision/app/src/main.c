/*
 * main.c - rpib_decision: 화재 판정 + 명령 목록 발행
 *
 * rpib_engine 3분할 중 판정 담당. 원본과의 핵심 차이:
 *  - sensor_reading 원시값은 안 쓴다(rpib_ingest 몫) - composite_score만
 *    UPSERT한다.
 *  - fire_scenario()/recover_scenario()가 RPi C로 직접 발행하지 않는다 -
 *    fire_cmd_msg_t를 만들어 내부 토픽(guardx/internal/rpib/fire_command)
 *    으로 던지면 rpib_dispatch가 실제 발행 + fire_event_command 기록을
 *    맡는다(fire_internal.h).
 *  - db_write_transition()이 이제 동기이고 event_id를 직접 돌려준다 -
 *    그 event_id를 내부 메시지마다 실어야 dispatch가 fire_event_command를
 *    올바른 사건에 붙일 수 있다.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <time.h>

#include "sensor_parser.h"
#include "decision.h"
#include "db_writer.h"
#include "mqtt_bus.h"
#include "threshold_loader.h"
#include "zone_loader.h"

#define SENSOR_SILENCE_WARN_MS 5000
#define SCENARIO_VALVE_CLOSE_ANGLE 90   /* servo_1: 가스밸브 잠금 (미확정치, 원본과 동일) */
#define SCENARIO_SOUND_FIRE         1

/* ---------------------------------------------------------------------
 * LED 매트릭스 표시 (B -> C, guardx/display/rpic/*)
 *
 * 토픽 문자열은 rpi_c/rpic_app/app/include/matrix_link.h와 같아야 한다.
 * 양쪽이 자기 상수로 들고 있는 것은 액추에이터 토픽과 같은 관례다 -
 * 여기서 rpi_c 헤더를 include하면 빌드가 서로 얽힌다.
 * --------------------------------------------------------------------- */
#define TOPIC_DISPLAY_FIRE      "guardx/display/rpic/fire"
#define TOPIC_DISPLAY_ZONE_FMT  "guardx/display/rpic/zones/%d"

/* STM32 레지스터 맵이 못 박은 값들(rpi_c guardx_modbus_regs.h와 같아야 함).
 * 화재 bitmap이 bit0~3이고 온습도 칸이 zone 4개뿐이라, 이 범위 밖 zone은
 * LED에 표시할 자리가 아예 없다. */
#define MATRIX_ZONE_COUNT       4
#define MATRIX_TEMP_X10_MAX     65534
#define MATRIX_HUMIDITY_MAX     100

/*
 * 값이 안 바뀌어도 이 주기마다 한 번은 다시 쏜다.
 *
 * STM32가 리부팅되면 레지스터가 리셋 초기값으로 돌아가는데, RPi B는 그
 * 사실을 알 방법이 없다("나 방금 켜졌다"를 알리는 경로가 없다). 변화만
 * 기다리면 다음 온도 변화가 올 때까지 화면이 "없음"으로 남고, 화재
 * bitmap은 다음 전이까지 0으로 남는다.
 *
 * 30초는 "사람이 화면을 보고 이상하다고 느끼기 전에 복구된다"와 "시리얼을
 * 추적 좌표와 나눠 쓰는데 부담을 주지 않는다" 사이의 절충이다. zone 4개
 * 전부라 해도 30초에 Modbus 쓰기 5건(zone 4 + 화재 1)이라 무시할 만하다.
 */
#define DISPLAY_REFRESH_MS      30000

static volatile sig_atomic_t running = 1;

/* LED 표시 발행 seq. 이 프로세스는 액추에이터를 직접 발행하지 않으므로
 * (rpib_dispatch 몫) 표시 전용 카운터 하나면 된다. */
static uint32_t display_seq;

/* 화재 표시를 마지막으로 성공 발행한 시각(mono_ms). 0 = 아직 없음.
 * zone별인 온습도와 달리 bitmap은 사이트 전체 한 벌이라 전역이다. */
static uint64_t g_fire_display_ms;

typedef struct {
    fire_zone_t     info;
    decision_zone_t decision;
    uint64_t        last_sensor_ms;
    bool            warned_stuck;
    bool            warned_relax;
    bool            silence_warned;

    /* LED 매트릭스로 마지막에 내보낸 온습도 표시값. 같은 값을 다시 쏘지
     * 않으려고 들고 있다 - 발행이 retained라 값이 그대로면 다시 보낼 이유가
     * 없고, RPi A가 1Hz로 올려주는 것을 그대로 중계하면 Modbus 트랜잭션이
     * 초당 한 번씩 무의미하게 돈다(같은 시리얼을 추적 좌표가 함께 쓴다).
     * display_sent=false면 아직 한 번도 안 보낸 것. */
    bool            display_sent;
    long            display_temp_x10;
    long            display_humidity;
    uint64_t        display_last_ms;   /* 마지막 성공 발행 시각(mono_ms) */
} zone_runtime_t;

static zone_runtime_t g_zones[MAX_FIRE_ZONES];
static int            g_zone_count;

static zone_runtime_t *find_zone_by_rpia(const char *node_id)
{
    int i;
    for (i = 0; i < g_zone_count; i++)
        if (strcmp(g_zones[i].info.rpia_node_id, node_id) == 0)
            return &g_zones[i];
    return NULL;
}

static guardx_err_t load_zones_at_boot(void)
{
    fire_zone_t loaded[MAX_FIRE_ZONES];
    int count, i;
    guardx_err_t err = zone_loader_load(loaded, &count);

    if (err != GUARDX_OK)
        return err;

    for (i = 0; i < count; i++) {
        memset(&g_zones[i], 0, sizeof(g_zones[i]));
        g_zones[i].info = loaded[i];
        DECISION_ZONE_INIT(&g_zones[i].decision);
    }
    g_zone_count = count;
    return GUARDX_OK;
}

static void sig_handler(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* 내부 토픽 발행 헬퍼 - fire_scenario/recover_scenario가 명령 1건마다 부른다.
 * 발행 실패는 로그만(원본 pub_action/pub_set과 같은 정책: 한 건 실패가
 * 나머지 명령을 막지 않는다). */
/* cause_str: 원본 main.c의 pub_action/pub_set "reason" 인자와 같은 뜻 -
 * fire_scenario는 실제 판정 원인(DECISION_CAUSE_STR), recover_scenario는
 * 원인이 아니라 "recover" 리터럴(원본과 동일 - 해제엔 원인이랄 게 없다,
 * DECISION_LAST_CAUSE는 직전 화재 원인을 그대로 들고 있어 그걸 쓰면
 * "이 원인 때문에 해제됐다"는 잘못된 기록이 된다 - db_write_transition의
 * NULL cause 처리와 같은 근거). */
static void pub_cmd(long long event_id, const zone_runtime_t *z,
                    const char *cause_str,
                    const char *command, const char *action, int value, bool has_value)
{
    fire_cmd_msg_t m;

    memset(&m, 0, sizeof(m));
    m.event_id = event_id;
    m.zone_id = z->info.zone_id;
    snprintf(m.rpic_node, sizeof(m.rpic_node), "%s", z->info.rpic_node_id);
    snprintf(m.cause, sizeof(m.cause), "%s", cause_str);
    m.trigger_seq = DECISION_LAST_TRIGGER_SEQ(&z->decision);
    snprintf(m.command, sizeof(m.command), "%s", command);
    snprintf(m.action, sizeof(m.action), "%s", action);
    m.value = value;
    m.has_value = has_value;

    if (mqtt_bus_publish_fire_cmd(&m) == GUARDX_OK)
        printf("main: -> internal [%s] %s %s\n", m.rpic_node, command, action);
}

/* 화재 확정 시나리오: 격리(밸브) -> 피난(문) -> 팬 강제정지(연소 조장
 * 방지, VEDA-177) -> 소화(펌프) -> 경보(앰프). 순서 근거는 원본 main.c
 * 주석과 동일(각 명령이 독립 멱등이라 순서 의존 없음). */
static void fire_scenario(long long event_id, const zone_runtime_t *z)
{
    const char *cause = DECISION_CAUSE_STR(DECISION_LAST_CAUSE(&z->decision));

    printf("main: !!! FIRE CONFIRMED zone %d '%s' !!!\n",
           z->info.zone_id, z->info.zone_name);

    pub_cmd(event_id, z, cause, "servo_1", "SET", SCENARIO_VALVE_CLOSE_ANGLE, true);
    pub_cmd(event_id, z, cause, "shutter", "CLOSE", 0, false);
    pub_cmd(event_id, z, cause, "fan", "OFF", 0, false);
    pub_cmd(event_id, z, cause, "water_pump", "ON", 0, false);
    pub_cmd(event_id, z, cause, "sound", "SET", SCENARIO_SOUND_FIRE, true);
}

static void recover_scenario(long long event_id, const zone_runtime_t *z)
{
    printf("main: zone %d situation recovered - stopping active response\n",
           z->info.zone_id);

    pub_cmd(event_id, z, "recover", "water_pump", "OFF", 0, false);
    pub_cmd(event_id, z, "recover", "sound", "SET", 0, true);
    pub_cmd(event_id, z, "recover", "fan", "OFF", 0, false);
    printf("main: (servo_1 valve / shutter left as-is - manual reset required)\n");
}

static void check_freeze(zone_runtime_t *z)
{
    const fire_config_t *c = DECISION_GET_CONFIG();
    int fz = DECISION_FREEZE_CYCLES(&z->decision);

    if (fz == 0) {
        z->warned_stuck = false;
        z->warned_relax = false;
        return;
    }
    if (fz >= c->n_recover && fz < c->freeze_relax_cycles && !z->warned_stuck) {
        fprintf(stderr, "main: WARNING - zone %d recovery frozen for %d cycles\n",
                z->info.zone_id, fz);
        z->warned_stuck = true;
    }
    if (fz >= c->freeze_relax_cycles && !z->warned_relax) {
        fprintf(stderr, "main: zone %d freeze relax threshold reached (%d cycles)\n",
                z->info.zone_id, fz);
        z->warned_relax = true;
    }
}

static void publish_fire_alert(const zone_runtime_t *z, decision_event_t ev)
{
    char json[GUARDX_JSON_MAX];
    int len;

    if (ev == DECISION_EVENT_FIRE) {
        len = snprintf(json, sizeof(json),
            "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"zone_id\":%d,"
            "\"event_type\":\"fire_confirmed\",\"cause\":\"%s\",\"trigger_seq\":%u}",
            (unsigned long long)now_ms(), z->info.zone_id,
            DECISION_CAUSE_STR(DECISION_LAST_CAUSE(&z->decision)),
            DECISION_LAST_TRIGGER_SEQ(&z->decision));
    } else {
        len = snprintf(json, sizeof(json),
            "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"zone_id\":%d,"
            "\"event_type\":\"recovered\"}",
            (unsigned long long)now_ms(), z->info.zone_id);
    }
    mqtt_bus_publish_alert("guardx/alert/fire", json, len);
}

/*
 * 현장 LED 평면도의 화재 표시 갱신 (STM32 레지스터 120).
 *
 * 경보(guardx/alert/fire)와 달리 "한 zone의 전이"가 아니라 "지금 전 구역이
 * 어떤가"를 매번 통째로 보낸다. STM32가 받는 것이 bitmap 한 칸이라 그
 * 자체가 사이트 전체 상태이기 때문이다 - 전이만 보내면 RPi C가 이전 값을
 * 기억하고 비트를 합성해야 하고, 그러면 같은 상태가 두 노드에 나뉜다.
 * 매번 전체를 보내면 이 값은 멱등이라 유실·중복에도 화면이 어긋나지 않는다.
 *
 * 호출 시점: 화재 전이가 일어난 직후(확정/해제 양쪽) + 기동 직후 1회 +
 * DISPLAY_REFRESH_MS마다 1회(on_sensor에서). 판정 상태를 그대로 읽으므로
 * 전이가 g_zones에 반영된 뒤에 불러야 한다.
 *
 * 발행에 성공했을 때만 g_fire_display_ms를 갱신한다 - 실패하면 주기가
 * 리셋되지 않아 다음 센서 사이클에 곧바로 다시 시도한다.
 */
static void publish_fire_display(void)
{
    char json[GUARDX_JSON_MAX];
    unsigned bitmap = 0;
    int i, len;

    for (i = 0; i < g_zone_count; i++) {
        int zid = g_zones[i].info.zone_id;

        if (DECISION_STATE(&g_zones[i].decision) != DECISION_STATE_FIRE)
            continue;

        /* fire_zone 테이블은 zone_id에 상한이 없지만 LED는 4칸뿐이다.
         * 조용히 버리면 "불났는데 화면에 안 뜬다"의 원인을 못 찾는다. */
        if (zid < 1 || zid > MATRIX_ZONE_COUNT) {
            fprintf(stderr, "main: zone %d은 LED 평면도 범위(1~%d) 밖 - "
                    "화재 표시 생략\n", zid, MATRIX_ZONE_COUNT);
            continue;
        }
        bitmap |= 1u << (zid - 1);
    }

    len = snprintf(json, sizeof(json),
        "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"seq\":%u,"
        "\"zone_bitmap\":%u}",
        (unsigned long long)now_ms(), display_seq++, bitmap);

    if (mqtt_bus_publish_display(TOPIC_DISPLAY_FIRE, json, len) == GUARDX_OK)
        g_fire_display_ms = mono_ms();
}

/*
 * 현장 LED 평면도의 zone 온습도 갱신 (STM32 레지스터 100~107).
 *
 * !!! 유효할 때만 보낸다 !!!
 * 센서가 죽었다는 것을 Modbus로 알릴 방법이 없다 - "없음"을 뜻하는
 * 0xFFFF/0x00FF가 쓰기 허용 범위 밖이라 리셋 초기값으로만 존재한다
 * (rpi_c guardx_modbus_regs.h 참조). 그래서 무효 사이클은 발행하지 않고,
 * 결과적으로 센서가 죽으면 LED에 마지막 정상값이 그대로 남는다. 제대로
 * 고치려면 STM32에 추적 좌표와 같은 만료 타이머가 필요하다.
 *
 * 실 하드웨어가 없는 zone(VMS 기준 더미 구역)은 애초에 센서 메시지가 오지
 * 않으므로 여기까지 오지 않는다 - STM32의 리셋 초기값이 그대로 남아 화면에
 * "없음"으로 표시된다. 더미를 위해 따로 처리할 것이 없다.
 */
static void publish_zone_display(zone_runtime_t *z, const sensor_msg_t *msg)
{
    char topic[64];
    char json[GUARDX_JSON_MAX];
    long temp_x10, humidity;
    int len;

    if (!msg->temphum_valid)
        return;

    if (z->info.zone_id < 1 || z->info.zone_id > MATRIX_ZONE_COUNT)
        return;   /* 표시할 자리가 없다 - 경고는 화재 쪽에서 이미 찍는다 */

    /* x10 정수로 내린다. RPi A가 x10 정수를 실수로 바꿔 보낸 것을 되돌리는
     * 셈인데, LED 레지스터가 0.1℃ 단위 정수라 어차피 여기서 다시 정수가
     * 된다. libm을 끌어오지 않으려고 lround 대신 직접 반올림한다. */
    temp_x10 = (long)(msg->temperature * 10.0 +
                      (msg->temperature >= 0.0 ? 0.5 : -0.5));
    humidity = (long)(msg->humidity + 0.5);

    /* 레지스터가 부호 없는 16비트라 영하를 표현할 방법이 없다. 0으로
     * 붙잡으면 화면에 0.0℃로 뜬다 - 실제와 다르지만, 범위를 벗어난 값을
     * 보내면 프레임이 통째로 거절돼 습도까지 못 쓰는 것보다는 낫다.
     * 영하가 실제로 문제되는 현장이면 레지스터 규약부터 바꿔야 한다. */
    if (temp_x10 < 0)
        temp_x10 = 0;
    if (temp_x10 > MATRIX_TEMP_X10_MAX)
        temp_x10 = MATRIX_TEMP_X10_MAX;
    if (humidity < 0)
        humidity = 0;
    if (humidity > MATRIX_HUMIDITY_MAX)
        humidity = MATRIX_HUMIDITY_MAX;

    /* 값이 그대로면 보내지 않는다 - 다만 영원히 침묵하면 안 된다.
     * STM32가 리부팅되면 레지스터가 "없음"으로 돌아가는데 RPi B는 그걸
     * 알 방법이 없어서, 변화만 기다리면 온도가 바뀔 때까지 화면이 빈 채로
     * 남는다. DISPLAY_REFRESH_MS마다 한 번은 같은 값이라도 다시 쏴서
     * 그런 상태가 오래 가지 않게 한다. */
    if (z->display_sent && z->display_temp_x10 == temp_x10 &&
        z->display_humidity == humidity &&
        mono_ms() - z->display_last_ms < DISPLAY_REFRESH_MS)
        return;

    snprintf(topic, sizeof(topic), TOPIC_DISPLAY_ZONE_FMT, z->info.zone_id);
    len = snprintf(json, sizeof(json),
        "{\"node_id\":\"rpib\",\"timestamp\":%llu,\"seq\":%u,"
        "\"zone_id\":%d,\"temp_x10\":%ld,\"humidity\":%ld}",
        (unsigned long long)now_ms(), display_seq++, z->info.zone_id,
        temp_x10, humidity);

    /* 발행에 성공했을 때만 "보냈다"고 기록한다. 순서를 뒤집으면(먼저
     * 기록하고 나중에 발행) 실패한 값이 보낸 것으로 남아, 다음 사이클에
     * 값이 같다는 이유로 재시도조차 하지 않는다 - 그 갱신은 영영 유실된다.
     * 실패 시 그냥 두면 다음 센서 사이클(1초)에 자연히 다시 시도한다. */
    if (mqtt_bus_publish_display(topic, json, len) != GUARDX_OK)
        return;

    z->display_temp_x10 = temp_x10;
    z->display_humidity = humidity;
    z->display_last_ms = mono_ms();
    z->display_sent = true;
}

static void on_sensor(const char *node_id, const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    sensor_msg_t msg;
    decision_event_t ev;
    zone_runtime_t *z = find_zone_by_rpia(node_id);
    long long event_id = 0;

    if (!z) {
        fprintf(stderr, "main: 미등록 노드 '%s' 무시\n", node_id);
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

    z->last_sensor_ms = mono_ms();

    ev = DECISION_FEED(&z->decision, &msg);
    db_write_sensor_score(z->info.zone_id, msg.seq,
                          DECISION_LAST_SCORE(&z->decision), now_ms());
    check_freeze(z);

    /* 온습도 LED 표시는 화재와 무관한 상시 표시라 early return보다 앞에
     * 둔다 - 전이가 없는 평상시(대부분의 사이클)가 정확히 이 값이 계속
     * 갱신돼야 하는 구간이다. */
    publish_zone_display(z, &msg);

    /* 화재 표시도 같은 이유로 주기 갱신한다(STM32 리부팅 복구).
     * 전이 때만 쏘면 "화재 없음"이 몇 시간이고 재확인되지 않는다.
     *
     * 워치독이 도는 main 스레드가 아니라 여기(콜백 스레드)에서 부르는
     * 것은 publish_fire_display()가 g_zones[].decision을 읽기 때문이다 -
     * 그 값을 쓰는 것도 이 콜백 스레드라, 여기서 부르면 새 공유가 생기지
     * 않는다(파일 상단 스레드 구조 주석의 전제를 그대로 지킨다). */
    if (mono_ms() - g_fire_display_ms >= DISPLAY_REFRESH_MS)
        publish_fire_display();

    if (ev == DECISION_EVENT_NONE)
        return;

    if (db_write_transition(z->info.zone_id, ev, DECISION_LAST_CAUSE(&z->decision),
                            DECISION_LAST_TRIGGER_SEQ(&z->decision), now_ms(),
                            &event_id) != GUARDX_OK) {
        fprintf(stderr, "main: zone %d fire_event write failed - "
                "명령을 못 낸다(event_id 없이는 fire_event_command를 못 붙임)\n",
                z->info.zone_id);
        return;
    }

    publish_fire_alert(z, ev);
    publish_fire_display();   /* 판정 상태를 읽으므로 전이가 반영된 뒤에 */
    if (ev == DECISION_EVENT_FIRE)
        fire_scenario(event_id, z);
    else
        recover_scenario(event_id, z);
}

static void on_clear_fire(const char *payload, int len)
{
    char buf[GUARDX_JSON_MAX];
    const char *p;
    int zone_id;
    zone_runtime_t *z = NULL;
    decision_event_t ev;
    long long event_id = 0;
    int i;

    if (len <= 0 || len >= (int)sizeof(buf)) {
        fprintf(stderr, "main: clear_fire payload 크기 이상 (%d) 무시\n", len);
        return;
    }
    memcpy(buf, payload, (size_t)len);
    buf[len] = '\0';

    p = strstr(buf, "\"zone_id\"");
    if (!p || !(p = strchr(p, ':'))) {
        fprintf(stderr, "main: clear_fire에 zone_id 없음 - 무시: %s\n", buf);
        return;
    }
    zone_id = atoi(p + 1);

    for (i = 0; i < g_zone_count; i++)
        if (g_zones[i].info.zone_id == zone_id) {
            z = &g_zones[i];
            break;
        }
    if (!z) {
        fprintf(stderr, "main: clear_fire 대상 zone %d 없음 - 무시\n", zone_id);
        return;
    }

    ev = DECISION_FORCE_RECOVER(&z->decision);
    if (ev != DECISION_EVENT_RECOVER) {
        printf("main: zone %d은 이미 NORMAL - 수동 해제 무시(멱등)\n", zone_id);
        return;
    }

    printf("main: zone %d '%s' 수동 화재 해제 (운영자 요청)\n",
           z->info.zone_id, z->info.zone_name);

    if (db_write_transition(z->info.zone_id, ev, DECISION_LAST_CAUSE(&z->decision),
                            DECISION_LAST_TRIGGER_SEQ(&z->decision), now_ms(),
                            &event_id) != GUARDX_OK) {
        fprintf(stderr, "main: zone %d 해제 fire_event 기록 실패\n", zone_id);
        return;
    }
    publish_fire_alert(z, ev);
    publish_fire_display();   /* 판정 상태를 읽으므로 전이가 반영된 뒤에 */
    recover_scenario(event_id, z);
}

static void on_config(const char *payload, int len)
{
    (void)payload;
    (void)len;
    printf("main: config reload signal received\n");
    if (threshold_load_and_apply() != GUARDX_OK)
        fprintf(stderr, "main: threshold reload failed, keeping current config\n");
}

int main(void)
{
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    if (db_writer_open() != GUARDX_OK) {
        fprintf(stderr, "main: db open failed, aborting\n");
        return 1;
    }
    if (load_zones_at_boot() != GUARDX_OK) {
        fprintf(stderr, "main: zone mapping load failed, aborting\n");
        db_writer_close();
        return 1;
    }
    if (threshold_load_and_apply() != GUARDX_OK)
        fprintf(stderr, "main: threshold load failed, using compiled-in defaults\n");

    if (mqtt_bus_init(on_sensor, on_config, on_clear_fire) != GUARDX_OK) {
        fprintf(stderr, "main: mqtt init failed, aborting\n");
        db_writer_close();
        return 1;
    }

    for (i = 0; i < g_zone_count; i++)
        g_zones[i].last_sensor_ms = mono_ms();

    /* LED 화재 표시를 기동 시 한 번 맞춰 둔다. 발행이 retained라, 이걸
     * 안 하면 지난 실행이 남긴 화재 표시가 브로커에 그대로 남아 RPi C가
     * 재접속할 때마다 되살아난다 - 상황이 끝났어도 지울 방법이 없다.
     *
     * 지금 판정 상태는 전 zone NORMAL이므로(부팅 시 DECISION_ZONE_INIT)
     * 사실상 "전부 정상"을 쏘는 것이다. 화재 진행 중에 이 프로세스만
     * 재시작하면 LED가 잠깐 정상으로 보였다가 다음 확정(n_confirm 사이클)에
     * 다시 켜진다 - 재시작이 판정 상태를 지우는 기존 한계와 같은 뿌리이고,
     * decision.h의 "상태 영속화" 미결 항목이 해결되면 함께 없어진다.
     * 온습도는 다음 센서 사이클(1초)에 자연히 갱신되므로 여기서 손대지 않는다. */
    publish_fire_display();

    printf("main: rpib_decision started - %d zone(s)\n", g_zone_count);

    while (running) {
        poll(NULL, 0, 1000);
        for (i = 0; i < g_zone_count; i++) {
            zone_runtime_t *z = &g_zones[i];
            if (mono_ms() - z->last_sensor_ms > SENSOR_SILENCE_WARN_MS) {
                if (!z->silence_warned) {
                    fprintf(stderr, "main: WARNING - zone %d '%s' no sensor "
                            "data for %d ms\n", z->info.zone_id,
                            z->info.zone_name, SENSOR_SILENCE_WARN_MS);
                    z->silence_warned = true;
                }
            } else {
                z->silence_warned = false;
            }
        }
    }

    printf("main: shutting down\n");
    mqtt_bus_cleanup();
    db_writer_close();
    return 0;
}
