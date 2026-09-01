/*
 * test_matrix_link.c - RPi C <-> STM32 표시 계약 단위 테스트
 *
 * 검증 대상은 matrix_link.c의 "MQTT 페이로드 -> Modbus 레지스터 쓰기"
 * 변환이다. 이 경계가 틀리면 증상이 "LED에 아무것도 안 뜬다" 하나로만
 * 나타나서 원인 추적이 오래 걸린다 - 실기기 없이 여기서 잡는다.
 *
 * ── 어떻게 시리얼 없이 도나 ──
 * 실제 modbus_rtu.c 대신 이 파일의 스텁을 링크한다. 스텁은 아무 데도 안 쓰고 "어떤
 * 레지스터에 무슨 값이 FC 몇으로 나갔는가"를 기록만 하므로, 테스트가
 * STM32 레지스터 맵 계약을 그대로 단언할 수 있다.
 *
 * ── 왜 이 항목들인가 ──
 * 전부 "틀려도 컴파일은 되고 실기기에서만 조용히 깨지는" 것들이다:
 *   - zone N -> 시작 주소 계산 (100 + (N-1)*2)
 *   - 범위 밖 값 거르기. STM32는 값 하나만 어긋나도 FC10 프레임 전체를
 *     예외 0x03으로 거절하므로, 온도가 틀리면 습도까지 안 써진다.
 *   - status=0(지우기)만 FC06, 나머지 좌표는 FC10 (STM32 규약)
 *   - 우편함 분리: 서로 다른 표시가 상대를 유실시키지 않는가
 *
 * 빌드/실행:  cd rpi_c/rpic_app/test && make test_matrix_link && ./test_matrix_link
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guardx_modbus_regs.h"
#include "matrix_link.h"
#include "modbus_rtu.h"

/* ------------------------------------------------------------------ *
 * modbus_rtu 스텁 - 실제 시리얼 대신 쓰기 기록만 남긴다
 * ------------------------------------------------------------------ */

#define MAX_WRITES 64

typedef struct {
    uint16_t addr;
    uint16_t count;            /* 1 = FC06 단일, 2 이상 = FC10 다중 */
    uint16_t regs[8];
} write_rec_t;

static write_rec_t  g_writes[MAX_WRITES];
static int          g_write_count;
static int          g_open_calls;
static int          g_close_calls;
static int          g_fire_shutter_close_calls;

/* 워커 스레드가 쓰고 테스트 스레드가 읽는다. 테스트는 wait_writes()로
 * 개수가 늘기를 기다린 뒤에만 읽으므로 별도 락을 두지 않는다 - 기록은
 * 워커 하나만 하고, 개수 증가가 곧 그 기록이 완성됐다는 신호다. */
static volatile int g_write_seq;

/* modbus_open은 주소만 유효하면 되는 더미 핸들을 준다. */
static int g_dummy_ctx;

modbus_ctx_t *modbus_open(const modbus_cfg_t *cfg)
{
    assert(cfg != NULL);
    assert(cfg->slave_id == GX_MODBUS_SLAVE_ID);
    g_open_calls++;
    return (modbus_ctx_t *)&g_dummy_ctx;
}

void modbus_close(modbus_ctx_t *ctx)
{
    (void)ctx;
    g_close_calls++;
}

static void record(uint16_t addr, uint16_t count, const uint16_t *values)
{
    write_rec_t *w;

    assert(g_write_count < MAX_WRITES);
    assert(count <= 8);

    w = &g_writes[g_write_count];
    w->addr = addr;
    w->count = count;
    memcpy(w->regs, values, (size_t)count * sizeof(values[0]));

    g_write_count++;
    g_write_seq++;
}

mb_status_t modbus_write_single(modbus_ctx_t *ctx, uint16_t addr, uint16_t value,
                                uint8_t *exc_code)
{
    (void)ctx;
    (void)exc_code;
    record(addr, 1, &value);
    return MB_OK;
}

mb_status_t modbus_write_multiple(modbus_ctx_t *ctx, uint16_t addr, uint16_t count,
                                  const uint16_t *values, uint8_t *exc_code)
{
    (void)ctx;
    (void)exc_code;
    record(addr, count, values);
    return MB_OK;
}

/* matrix_link.c가 실패 로그에서만 쓰는 것들 - 링크만 되면 된다. */
const char *modbus_strerror(mb_status_t s) { (void)s; return "stub"; }
const char *modbus_exc_str(uint8_t c)      { (void)c; return "stub"; }
const char *modbus_default_device(void)    { return "/dev/null"; }

/* 화재 표시는 LED 말고도 두 곳에 전달된다 - 사이렌(조율기)과 팬.
 * 이 테스트는 **표시 계약**만 보므로 둘 다 받아만 두고 버린다.
 * 스텁이 없으면 링크가 깨져서, 표시 경로가 멀쩡한지조차 확인할 수 없다. */
void audio_arbiter_set_fire(bool active) { (void)active; }
void fan_control_set_fire(bool active)   { (void)active; }
guardx_err_t actuator_shutter_close(void)
{
    g_fire_shutter_close_calls++;
    return GUARDX_OK;
}

/* ------------------------------------------------------------------ *
 * 테스트 유틸
 * ------------------------------------------------------------------ */

static int  g_checks;
static int  g_failures;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            printf("  [FAIL] %s:%d  ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000L, (ms % 1000L) * 1000000L };
    nanosleep(&ts, NULL);
}

/* 워커가 비동기라 발행 직후엔 아직 안 나갔을 수 있다. 기록이 want개가
 * 될 때까지 기다린다. 반환 0=도달, -1=시간초과. */
static int wait_writes(int want, long timeout_ms)
{
    long waited = 0;

    while (g_write_seq < want && waited < timeout_ms) {
        sleep_ms(5);
        waited += 5;
    }
    return (g_write_seq >= want) ? 0 : -1;
}

/* 아무것도 더 안 나가는지 확인할 때. 워커에게 나갈 기회를 준 뒤 센다. */
static void settle(void)
{
    sleep_ms(60);
}

static void reset_writes(void)
{
    settle();
    g_write_count = 0;
    g_write_seq = 0;
    memset(g_writes, 0, sizeof(g_writes));
}

static void send_track(const char *json)  { matrix_link_handle_track(json, (int)strlen(json)); }
static void send_fire(const char *json)   { matrix_link_handle_fire(json, (int)strlen(json)); }
static void send_zones(const char *json)  { matrix_link_handle_zones(json, (int)strlen(json)); }

/* ------------------------------------------------------------------ *
 * 테스트
 * ------------------------------------------------------------------ */

/* 화재 bitmap은 레지스터 120에 FC06 단일 쓰기로 나가야 한다. */
static void test_fire_bitmap(void)
{
    printf("[화재] zone_bitmap -> reg %d\n", REG_FIRE_ZONE_BITMAP);
    reset_writes();

    send_fire("{\"node_id\":\"rpib\",\"timestamp\":1,\"seq\":0,\"zone_bitmap\":5}");
    CHECK(g_fire_shutter_close_calls == 1,
          "fire rising edge closes shutter once (actual %d)",
          g_fire_shutter_close_calls);
    CHECK(wait_writes(1, 500) == 0, "발행이 Modbus로 안 나감");
    CHECK(g_writes[0].addr == REG_FIRE_ZONE_BITMAP,
          "주소가 %u (기대 %d)", g_writes[0].addr, REG_FIRE_ZONE_BITMAP);
    CHECK(g_writes[0].count == 1, "FC06 단일 쓰기여야 하는데 count=%u", g_writes[0].count);
    CHECK(g_writes[0].regs[0] == 5, "값이 %u (기대 5)", g_writes[0].regs[0]);

    /* 0 = 전 구역 정상. "값이 0이라 안 보낸다" 같은 최적화가 들어가면
     * 화재 해제가 화면에 영영 반영되지 않는다. */
    send_fire("{\"zone_bitmap\":2}");
    CHECK(g_fire_shutter_close_calls == 1,
          "active fire does not repeat shutter close (actual %d)",
          g_fire_shutter_close_calls);

    reset_writes();
    send_fire("{\"zone_bitmap\":0}");
    CHECK(g_fire_shutter_close_calls == 1,
          "fire recovery does not move shutter (actual %d)",
          g_fire_shutter_close_calls);
    CHECK(wait_writes(1, 500) == 0, "해제(0)가 안 나감 - 화면에 화재가 남는다");
    CHECK(g_writes[0].regs[0] == 0, "해제 값이 %u", g_writes[0].regs[0]);

    /* bit0~3 밖은 STM32가 예외로 거절한다(max 15). */
    send_fire("{\"zone_bitmap\":1}");
    CHECK(g_fire_shutter_close_calls == 2,
          "next fire rising edge closes shutter again (actual %d)",
          g_fire_shutter_close_calls);

    reset_writes();
    send_fire("{\"zone_bitmap\":16}");
    settle();
    CHECK(g_write_count == 0, "범위 초과(16)를 걸러내지 못함");

    reset_writes();
    send_fire("{\"zone_bitmap\":-1}");
    settle();
    CHECK(g_write_count == 0, "음수를 걸러내지 못함");

    reset_writes();
    send_fire("{\"node_id\":\"rpib\"}");
    settle();
    CHECK(g_write_count == 0, "필드 누락을 걸러내지 못함");
}

/* zone N 온습도는 100+(N-1)*2 에 FC10 2칸으로 나가야 한다. */
static void test_zone_addressing(void)
{
    static const uint16_t expect[GX_ZONE_COUNT] = {
        REG_ZONE1_TEMP_X10, REG_ZONE2_TEMP_X10,
        REG_ZONE3_TEMP_X10, REG_ZONE4_TEMP_X10
    };
    char json[160];
    int z;

    printf("[온습도] zone N -> reg %d + (N-1)*2\n", REG_ZONE1_TEMP_X10);

    for (z = 1; z <= GX_ZONE_COUNT; z++) {
        reset_writes();
        snprintf(json, sizeof(json),
                 "{\"node_id\":\"rpib\",\"zone_id\":%d,"
                 "\"temp_x10\":%d,\"humidity\":%d}",
                 z, 200 + z, 40 + z);
        send_zones(json);

        CHECK(wait_writes(1, 500) == 0, "zone %d 발행이 안 나감", z);
        CHECK(g_writes[0].addr == expect[z - 1],
              "zone %d 주소가 %u (기대 %u)", z, g_writes[0].addr, expect[z - 1]);
        CHECK(g_writes[0].count == 2,
              "zone %d는 온도+습도 2칸이어야 하는데 count=%u", z, g_writes[0].count);
        CHECK(g_writes[0].regs[0] == (uint16_t)(200 + z),
              "zone %d 온도가 %u", z, g_writes[0].regs[0]);
        CHECK(g_writes[0].regs[1] == (uint16_t)(40 + z),
              "zone %d 습도가 %u", z, g_writes[0].regs[1]);
    }
}

/*
 * 범위를 벗어난 값은 프레임을 만들기 전에 걸러야 한다.
 *
 * STM32는 FC10을 받을 때 모든 값을 검사해 하나라도 어긋나면 프레임 전체를
 * 거절한다. 즉 습도가 101이면 같이 실린 온도까지 안 써진다 - 화면 절반만
 * 갱신되는 게 아니라 아무것도 안 바뀌므로, 여기서 못 거르면 원인이 안 보인다.
 */
static void test_zone_range_rejection(void)
{
    char json[160];

    printf("[온습도] 범위 밖 거르기 (온 0~%d / 습 0~%d)\n",
           GX_ZONE_TEMP_X10_MAX, GX_ZONE_HUMIDITY_MAX);

    reset_writes();
    snprintf(json, sizeof(json), "{\"zone_id\":1,\"temp_x10\":%d,\"humidity\":50}",
             GX_ZONE_TEMP_X10_MAX + 1);
    send_zones(json);
    settle();
    CHECK(g_write_count == 0, "온도 상한 초과를 걸러내지 못함");

    reset_writes();
    snprintf(json, sizeof(json), "{\"zone_id\":1,\"temp_x10\":250,\"humidity\":%d}",
             GX_ZONE_HUMIDITY_MAX + 1);
    send_zones(json);
    settle();
    CHECK(g_write_count == 0, "습도 상한 초과를 걸러내지 못함");

    reset_writes();
    send_zones("{\"zone_id\":0,\"temp_x10\":250,\"humidity\":50}");
    settle();
    CHECK(g_write_count == 0, "zone_id 0을 걸러내지 못함");

    reset_writes();
    snprintf(json, sizeof(json), "{\"zone_id\":%d,\"temp_x10\":250,\"humidity\":50}",
             GX_ZONE_COUNT + 1);
    send_zones(json);
    settle();
    CHECK(g_write_count == 0, "zone_id 상한 초과를 걸러내지 못함");

    /* 경계값은 통과해야 한다 - 과하게 막으면 정상값이 안 뜬다. */
    reset_writes();
    snprintf(json, sizeof(json), "{\"zone_id\":1,\"temp_x10\":%d,\"humidity\":%d}",
             GX_ZONE_TEMP_X10_MAX, GX_ZONE_HUMIDITY_MAX);
    send_zones(json);
    CHECK(wait_writes(1, 500) == 0, "경계값(상한)이 거부됨");

    reset_writes();
    send_zones("{\"zone_id\":1,\"temp_x10\":0,\"humidity\":0}");
    CHECK(wait_writes(1, 500) == 0, "경계값(하한 0)이 거부됨");
}

/* 추적 좌표: status=0만 FC06, 나머지는 좌표 5개 FC10. */
static void test_track_frame_shape(void)
{
    printf("[추적] status=0 -> FC06, 그 외 -> FC10 5칸\n");

    reset_writes();
    send_track("{\"status\":3,\"ax\":420,\"ay\":180,\"bx\":510,\"by\":205}");
    CHECK(wait_writes(1, 500) == 0, "좌표 발행이 안 나감");
    CHECK(g_writes[0].addr == REG_INTRUDER_TRACK_STATUS,
          "주소가 %u", g_writes[0].addr);
    CHECK(g_writes[0].count == 5, "좌표는 5칸 묶음이어야 하는데 count=%u",
          g_writes[0].count);
    CHECK(g_writes[0].regs[0] == 3 && g_writes[0].regs[1] == 420 &&
          g_writes[0].regs[2] == 180 && g_writes[0].regs[3] == 510 &&
          g_writes[0].regs[4] == 205, "좌표 값이 어긋남");

    /* 지우기는 단일 쓰기 - STM32가 좌표 묶음의 중간 상태를 막으려고
     * status=0일 때만 FC06을 허용한다. */
    reset_writes();
    send_track("{\"status\":0,\"ax\":0,\"ay\":0,\"bx\":0,\"by\":0}");
    CHECK(wait_writes(1, 500) == 0, "지우기가 안 나감");
    CHECK(g_writes[0].count == 1, "지우기는 FC06이어야 하는데 count=%u",
          g_writes[0].count);
    CHECK(g_writes[0].regs[0] == 0, "지우기 값이 %u", g_writes[0].regs[0]);

    /* 좌표 범위(0~1000) 밖 */
    reset_writes();
    send_track("{\"status\":1,\"ax\":1001,\"ay\":0,\"bx\":0,\"by\":0}");
    settle();
    CHECK(g_write_count == 0, "좌표 범위 초과를 걸러내지 못함");

    /* status는 0/1/3만 유효(2는 "예측점만 유효"라 STM32가 거절한다).
     * 상한(3) 초과는 여기서 걸러야 한다. */
    reset_writes();
    send_track("{\"status\":4,\"ax\":0,\"ay\":0,\"bx\":0,\"by\":0}");
    settle();
    CHECK(g_write_count == 0, "status 범위 초과를 걸러내지 못함");
}

/*
 * 우편함 분리 - 이 테스트가 A안(슬롯 분리)의 존재 이유다.
 *
 * 한 칸을 공유하던 옛 구조에서는 서로 다른 표시가 연달아 오면 나중 것이
 * 앞 것을 덮어써 레지스터 구간이 다른데도 쓰기가 통째로 유실됐다.
 * 세 종류를 연달아 넣고 셋 다 나가는지 본다.
 */
static void test_slots_do_not_clobber(void)
{
    int i;
    int saw_track = 0, saw_fire = 0, saw_zone = 0;

    printf("[우편함] 서로 다른 표시가 상대를 유실시키지 않는가\n");
    reset_writes();

    send_track("{\"status\":3,\"ax\":10,\"ay\":20,\"bx\":30,\"by\":40}");
    send_fire("{\"zone_bitmap\":2}");
    send_zones("{\"zone_id\":3,\"temp_x10\":251,\"humidity\":55}");

    CHECK(wait_writes(3, 1000) == 0,
          "3건 중 %d건만 나감 - 우편함이 서로를 덮어쓴다", g_write_seq);

    for (i = 0; i < g_write_count; i++) {
        if (g_writes[i].addr == REG_INTRUDER_TRACK_STATUS) saw_track = 1;
        if (g_writes[i].addr == REG_FIRE_ZONE_BITMAP)      saw_fire = 1;
        if (g_writes[i].addr == REG_ZONE3_TEMP_X10)        saw_zone = 1;
    }
    CHECK(saw_track, "추적 좌표가 유실됨");
    CHECK(saw_fire, "화재 bitmap이 유실됨");
    CHECK(saw_zone, "zone 3 온습도가 유실됨");

    /* 서로 다른 zone도 각자의 칸을 써야 한다(zone끼리도 안 덮어씀). */
    reset_writes();
    send_zones("{\"zone_id\":1,\"temp_x10\":200,\"humidity\":40}");
    send_zones("{\"zone_id\":2,\"temp_x10\":210,\"humidity\":41}");
    CHECK(wait_writes(2, 1000) == 0,
          "zone 1/2가 서로를 덮어씀 (%d건만 나감)", g_write_seq);
}

/*
 * 같은 칸 안에서는 최신 값이 이긴다(latest-wins).
 *
 * 좌표를 큐에 쌓아 순서대로 그리면 화면이 실제보다 뒤처지므로, 밀린
 * 동안 온 옛 값은 버리는 것이 규약이다. 마지막 값은 반드시 나가야 한다.
 */
static void test_latest_wins_within_slot(void)
{
    int i;
    int saw_last = 0;

    printf("[우편함] 같은 칸은 최신 값이 이긴다\n");
    reset_writes();

    send_fire("{\"zone_bitmap\":1}");
    send_fire("{\"zone_bitmap\":2}");
    send_fire("{\"zone_bitmap\":4}");
    settle();

    CHECK(g_write_count >= 1, "아무것도 안 나감");
    for (i = 0; i < g_write_count; i++)
        if (g_writes[i].regs[0] == 4)
            saw_last = 1;
    CHECK(saw_last, "마지막 값(4)이 반영되지 않음");
    CHECK(g_write_count <= 3, "쌓인 값을 다 내보냄 (%d건) - latest-wins 아님",
          g_write_count);
}

int main(void)
{
    /* 실제 시리얼 대신 스텁이 열린다. "off"를 주면 구독만 하고 워커를
     * 안 띄우므로 여기서는 주지 않는다. */
    unsetenv("GUARDX_MATRIX_DEV");

    if (matrix_link_init() != GUARDX_OK) {
        printf("matrix_link_init 실패 - 스텁이 링크됐는지 확인\n");
        return 1;
    }
    CHECK(g_open_calls == 1, "modbus_open이 %d번 불림", g_open_calls);

    test_fire_bitmap();
    test_zone_addressing();
    test_zone_range_rejection();
    test_track_frame_shape();
    test_slots_do_not_clobber();
    test_latest_wins_within_slot();

    /* cleanup은 추적만 지운다 - 화재/온습도는 현장 상태라 데몬 재시작을
     * 이유로 끄면 안 된다(matrix_link.c 주석 참조). */
    reset_writes();
    matrix_link_cleanup();
    settle();
    CHECK(g_write_count == 1, "cleanup이 %d건 씀 (추적 지우기 1건이어야 함)",
          g_write_count);
    if (g_write_count == 1) {
        CHECK(g_writes[0].addr == REG_INTRUDER_TRACK_STATUS,
              "cleanup이 지운 주소가 %u", g_writes[0].addr);
        CHECK(g_writes[0].regs[0] == 0, "cleanup 값이 %u", g_writes[0].regs[0]);
    }
    CHECK(g_close_calls == 1, "modbus_close가 %d번 불림", g_close_calls);

    printf("\n%s  (%d개 확인, 실패 %d)\n",
           g_failures == 0 ? "전부 통과" : "실패 있음", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
