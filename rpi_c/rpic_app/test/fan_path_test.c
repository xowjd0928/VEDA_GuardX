/*
 * fan_path_test.c — VMS AUTO 명령이 RPi C 까지 닿는지 검증. 하드웨어 불필요.
 *
 * 검사 구간 (RPi C 쪽 전부):
 *   RPi B 가 발행한 JSON  ->  PARSE_CMD_JSON  ->  DISPATCH_CMD
 *                         ->  handle_fan_auto ->  fan_control
 *
 * 실제 파일(cmd_parser.c · actuator_registry.c · fan_control.c)을 그대로
 * 링크한다. 페이로드를 여기서 손으로 적는 대신 RPi B 의 set_actuator 가
 * 만드는 문자열 모양을 그대로 쓴다(task_vms.cpp 참조) - 형식이 어긋나면
 * 여기서 걸린다.
 *
 * ⚠ RPi B 의 actuator_command 카탈로그 검증은 DB 라 여기서 못 잡는다.
 *   그건 Database/history/fire/20260813_02_fan_auto_command.sql 로 넣고
 *   아래로 확인한다:
 *     psql -d guardx -c "SELECT kind FROM actuator_command
 *                        WHERE command_key='fan_auto'"   -- onoff 여야 함
 *   이 행이 없으면 VMS 버튼이 눌려도 RPi B 에서 거부돼 여기까지 오지 않는다.
 *
 * 빌드/실행: make fan_path_test && ./fan_path_test
 */

#include "actuator_registry.h"
#include "cmd_parser.h"
#include "fan_control.h"
#include "fan_protocol.h"
#include "guardx_hal.h"
#include "guardx_protocol.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 스텁 */

static int g_duty = -1;

guardx_err_t rpic_fan_set(fan_data_t duty) { g_duty = (int)duty; return GUARDX_OK; }
guardx_err_t rpic_pump_set(pump_data_t v)  { (void)v; return GUARDX_OK; }
guardx_err_t rpic_servo_set(int32_t id, int32_t angle) { (void)id; (void)angle; return GUARDX_OK; }
guardx_err_t rpic_stepper_set(stepper_data_t v) { (void)v; return GUARDX_OK; }

guardx_err_t rpic_pca9685_open(void)  { return GUARDX_OK; }
guardx_err_t rpic_pca9685_close(void) { return GUARDX_OK; }
guardx_err_t rpic_fan_open(void)      { return GUARDX_OK; }
guardx_err_t rpic_fan_close(void)     { return GUARDX_OK; }
guardx_err_t rpic_stepper_open(void)  { return GUARDX_OK; }
guardx_err_t rpic_stepper_close(void) { return GUARDX_OK; }
guardx_err_t rpic_pump_open(void)     { return GUARDX_OK; }
guardx_err_t rpic_pump_close(void)    { return GUARDX_OK; }

guardx_err_t audio_event_init(const char *d) { (void)d; return GUARDX_OK; }
void audio_event_play(int s) { (void)s; }
void audio_event_stop(void) {}
void audio_event_cleanup(void) {}

static char g_state[256];
guardx_err_t mqtt_sub_publish_retained(const char *t, const char *j, int len)
{
    (void)t;
    if (len > (int)sizeof(g_state) - 1) len = (int)sizeof(g_state) - 1;
    memcpy(g_state, j, (size_t)len);
    g_state[len] = '\0';
    return GUARDX_OK;
}

/* ---------------------------------------------------------------- 도우미 */

/* RPi B 의 set_actuator 가 guardx/actuator/rpic 로 내보내는 모양 그대로. */
static guardx_err_t deliver(const char *command, const char *action)
{
    char json[256];
    actuator_cmd_t cmd;
    guardx_err_t rc;

    snprintf(json, sizeof(json),
             "{\"node_id\":\"rpic\",\"timestamp\":1786600000000,\"seq\":7,"
             "\"command\":\"%s\",\"action\":\"%s\"}", command, action);

    rc = PARSE_CMD_JSON(json, &cmd);
    if (rc != GUARDX_OK) {
        printf("  파싱 실패: %s\n", json);
        return rc;
    }
    return DISPATCH_CMD(&cmd);
}

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
    CHECK(!fan_control_auto_enabled(), "시작 시 AUTO 꺼짐");

    /* 1) 카탈로그에 등록된 command_key 그대로 온다. RPi C 가 모르는 명령이면
     *    DISPATCH_CMD 가 거절하므로, 이 한 줄이 이름 오타를 잡는다. */
    CHECK(deliver(GUARDX_CMD_FAN_AUTO, "ON") == GUARDX_OK,
          "fan_auto ON 이 라우팅됨");
    CHECK(fan_control_auto_enabled(), "AUTO 켜짐");
    CHECK(g_duty == GUARDX_FAN_DUTY_NORMAL,
          "AUTO 켜자마자 정상 단계 40% 적용");
    CHECK(strstr(g_state, "\"auto\":true") != NULL, "상태 발행 auto=true");
    printf("VMS AUTO ON  -> RPi B -> RPi C: AUTO 켜짐, %d%%\n", g_duty);

    /* 2) AUTO 중 수동 fan 명령은 RPi C 에서 거절된다(화면 잠금의 백스톱). */
    CHECK(deliver(GUARDX_CMD_FAN, "SET") != GUARDX_OK,
          "value 없는 SET 은 애초에 거절");
    CHECK(deliver(GUARDX_CMD_FAN, "ON") != GUARDX_OK,
          "AUTO 중 수동 ON 거절");
    CHECK(g_duty == GUARDX_FAN_DUTY_NORMAL, "거절됐으므로 듀티 그대로");
    printf("AUTO 중 수동 fan ON  -> 거절됨 (듀티 %d%% 유지)\n", g_duty);

    /* 3) 혼잡 단계가 오르면 AUTO 가 따라간다. */
    fan_control_set_level(2);
    CHECK(g_duty == GUARDX_FAN_DUTY_CRIT, "위험 단계 90%");
    printf("혼잡 단계 2 -> %d%%\n", g_duty);

    /* 4) AUTO OFF 도 같은 경로로 돌아온다. 출력은 그대로 유지된다. */
    CHECK(deliver(GUARDX_CMD_FAN_AUTO, "OFF") == GUARDX_OK,
          "fan_auto OFF 라우팅됨");
    CHECK(!fan_control_auto_enabled(), "AUTO 꺼짐");
    CHECK(g_duty == GUARDX_FAN_DUTY_CRIT, "AUTO 끔 - 현재 출력 유지");
    printf("VMS AUTO OFF -> AUTO 꺼짐, 출력 %d%% 유지\n", g_duty);

    /* 5) 이제 수동이 다시 먹는다. */
    CHECK(deliver(GUARDX_CMD_FAN, "OFF") == GUARDX_OK, "수동 OFF 적용");
    CHECK(g_duty == 0, "수동 OFF -> 0%");
    printf("AUTO 꺼진 뒤 수동 fan OFF -> %d%%\n", g_duty);

    /* 6) fan_auto 는 SET 을 받지 않는다. 카탈로그도 onoff 로 등록해 RPi B 가
     *    먼저 거르지만, 다른 경로로 들어와도 여기서 막힌다. */
    CHECK(deliver(GUARDX_CMD_FAN_AUTO, "SET") != GUARDX_OK,
          "fan_auto SET 은 거절");
    printf("fan_auto SET -> 거절됨\n");

    printf("\nALL FAN PATH TESTS PASSED\n");
    return 0;
}
