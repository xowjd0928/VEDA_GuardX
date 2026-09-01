/*
 * fan_control_test.c — 팬 우선순위(화재 > AUTO > 수동) 검증. 하드웨어 불필요.
 *
 * 빌드/실행:
 *   cd rpi_c/rpic_app/test && make fan_control_test && ./fan_control_test
 *   (또는) gcc -std=gnu11 -I../app/include -I../hal/include \
 *              -I../../common/include -I../../../shared \
 *              fan_control_test.c ../app/src/fan_control.c -o fan_control_test -lpthread
 *
 * HAL(rpic_fan_set)과 MQTT 발행을 여기서 스텁으로 대신한다. 진짜 팬을 돌리지
 * 않고도 "화재 중에는 무슨 일이 있어도 0%" 를 못 박아 두려는 것이다 — 이
 * 규칙이 깨지는 것은 코드를 읽어서는 잘 안 보이고, 현장에서 드러나면 늦다.
 */

#include "fan_control.h"
#include "fan_protocol.h"
#include "guardx_hal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 스텁 */

static int g_duty = -1;        /* 마지막으로 팬에 실제로 넣은 값 */
static int g_set_calls;

guardx_err_t rpic_fan_set(fan_data_t duty)
{
    g_duty = (int)duty;
    g_set_calls++;
    return GUARDX_OK;
}

/* fan_control 이 상태를 알리는 경로. 여기서는 마지막 payload 만 들고 있는다. */
static char g_state[256];

guardx_err_t mqtt_sub_publish_retained(const char *topic, const char *json,
                                       int len)
{
    (void)topic;
    if (len > (int)sizeof(g_state) - 1)
        len = (int)sizeof(g_state) - 1;
    memcpy(g_state, json, (size_t)len);
    g_state[len] = '\0';
    return GUARDX_OK;
}

static int state_has(const char *needle)
{
    return strstr(g_state, needle) != NULL;
}

/* ---------------------------------------------------------------- 검사 */

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  실패: %s  (duty=%d, state=%s)\n", msg, g_duty, g_state); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int main(void)
{
    fan_control_init();
    CHECK(g_duty == 0, "기동 직후에는 정지");
    CHECK(!fan_control_auto_enabled(), "AUTO 는 기본 꺼짐");
    printf("init: 정지 상태에서 시작, AUTO 꺼짐\n");

    /* 1) AUTO OFF 에서는 혼잡 단계가 팬을 건드리지 않는다. */
    fan_control_set_level(2);
    CHECK(g_duty == 0, "AUTO 꺼짐 - 단계가 올라도 팬은 그대로");

    /* 2) 수동 제어는 AUTO 가 꺼져 있을 때만 먹는다. */
    CHECK(fan_control_manual(60) == GUARDX_OK, "수동 60% 적용");
    CHECK(g_duty == 60, "수동 60%");
    CHECK(state_has("\"duty\":60"), "상태 발행에 60 이 실림");
    printf("manual: AUTO 꺼짐 상태에서 60%% 적용됨\n");

    /* 3) AUTO 를 켜면 그 순간의 단계로 즉시 옮겨간다(위험 -> 90%). */
    fan_control_set_auto(true);
    CHECK(g_duty == GUARDX_FAN_DUTY_CRIT, "AUTO 켜면 위험 단계 90%");
    CHECK(state_has("\"auto\":true"), "상태에 auto=true");
    printf("auto on: 위험 단계 -> %d%%\n", g_duty);

    /* 4) AUTO 중 수동 명령은 거절된다. 화면도 잠그지만 여기가 백스톱이다. */
    CHECK(fan_control_manual(10) != GUARDX_OK, "AUTO 중 수동 명령 거절");
    CHECK(g_duty == GUARDX_FAN_DUTY_CRIT, "거절됐으므로 듀티 그대로");
    printf("auto on: 수동 명령 거절됨\n");

    /* 5) 단계가 내려가면 듀티도 따라 내려간다. */
    fan_control_set_level(1);
    CHECK(g_duty == GUARDX_FAN_DUTY_WARN, "주의 단계 75%");
    fan_control_set_level(0);
    CHECK(g_duty == GUARDX_FAN_DUTY_NORMAL, "정상 단계 40%");
    CHECK(g_duty != 0, "정상도 0 이 아니다 - 기본 환기는 계속 돈다");
    printf("auto: 단계 2->1->0 => %d%% 로 내려옴\n", g_duty);

    /* 6) 화재는 AUTO 를 이긴다. */
    fan_control_set_fire(true);
    CHECK(g_duty == 0, "화재 - AUTO 중이어도 0%");
    CHECK(state_has("\"fire\":true"), "상태에 fire=true");
    /* 화재 중에는 단계가 올라가도 0% 를 유지한다. */
    fan_control_set_level(2);
    CHECK(g_duty == 0, "화재 중 단계 상승 - 여전히 0%");
    CHECK(fan_control_manual(80) != GUARDX_OK, "화재 중 수동 명령 거절");
    CHECK(g_duty == 0, "화재 중 - 수동으로도 못 올림");
    printf("fire: AUTO·수동 어느 쪽으로도 0%% 를 못 벗어남\n");

    /* 7) 화재 해제 + AUTO ON -> 자동 재개 (지금 단계는 위험). */
    fan_control_set_fire(false);
    CHECK(g_duty == GUARDX_FAN_DUTY_CRIT, "화재 해제 + AUTO -> 자동 재개");
    printf("fire clear (AUTO on): %d%% 로 자동 재개\n", g_duty);

    /* 8) AUTO 를 끄면 **현재 출력을 유지**한다. 0 으로 떨어뜨리지 않는다. */
    fan_control_set_auto(false);
    CHECK(g_duty == GUARDX_FAN_DUTY_CRIT, "AUTO 끔 - 현재 출력 유지");
    CHECK(state_has("\"auto\":false"), "상태에 auto=false");
    printf("auto off: 출력 %d%% 그대로 유지\n", g_duty);

    /* 9) AUTO OFF 상태에서 화재 -> 0%, 해제해도 0% 유지(수동만 풀림). */
    fan_control_set_fire(true);
    CHECK(g_duty == 0, "AUTO 꺼짐 + 화재 - 0%");
    fan_control_set_fire(false);
    CHECK(g_duty == 0, "화재 해제 + AUTO 꺼짐 - 0% 유지(스스로 안 살아남)");
    CHECK(fan_control_manual(30) == GUARDX_OK, "해제 후 수동은 다시 먹는다");
    CHECK(g_duty == 30, "수동 30%");
    printf("fire clear (AUTO off): 0%% 유지, 수동 조작만 풀림\n");

    printf("\nALL FAN CONTROL TESTS PASSED\n");
    return 0;
}
