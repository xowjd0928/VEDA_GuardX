/*
 * actuator_registry.c - 액추에이터 registry + 명령 디스패치
 *
 * command 문자열(프로토콜 규약 4-3) -> HAL 호출 매핑은 전부 이 파일
 * 안에 있다. 액추에이터가 추가되면 핸들러 함수 + actuators[] 한 줄만
 * 추가하면 됨.
 *
 * I2S 상황음은 sound 명령으로만 재생한다.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "actuator_registry.h"
#include "guardx_protocol.h"
#include "audio_event.h"
#include "fan_control.h"
#include "fan_protocol.h"

/* 프로토콜 4-3: fan은 ON/OFF도 허용 - ON일 때 적용할 기본 듀티.
 * !!! 미확정: 팬 최소 구동 듀티가 정해지면 재검토 !!! */
#define FAN_ON_DEFAULT_DUTY 100
#define SHUTTER_PHYSICAL_CLOSE_DIR STEPPER_CCW
#define SHUTTER_PHYSICAL_OPEN_DIR  STEPPER_CW

/* 부팅/종료 시 적용하는 안전 상태. 서보 안전 각도는 드라이버가
 * 로드/언로드 시점에 자체 적용하므로(rpic_pca9685.h SAFE 값),
 * App은 이진 액추에이터만 확실히 꺼주면 된다. */
static void apply_safe_state(void)
{
    if (rpic_fan_set(0) != GUARDX_OK)
        fprintf(stderr, "registry: safe state fan off failed\n");
    if (rpic_pump_set(0) != GUARDX_OK)
        fprintf(stderr, "registry: safe state pump off failed\n");
    /* 스텝모터는 연속 회전이라 켜둔 채 나가면 계속 돈다 - 반드시 정지.
     * (기동 시엔 방향 불명이므로 정지에서 시작) */
    if (rpic_stepper_set(0) != GUARDX_OK)
        fprintf(stderr, "registry: safe state stepper stop failed\n");
}

/* ---------------------------------------------------------------------
 * command별 핸들러
 * --------------------------------------------------------------------- */

/* action 문자열 판별 헬퍼 */
static bool is_on(const actuator_cmd_t *c)  { return strcmp(c->action, GUARDX_ACTION_ON)  == 0; }
static bool is_off(const actuator_cmd_t *c) { return strcmp(c->action, GUARDX_ACTION_OFF) == 0; }
static bool is_set(const actuator_cmd_t *c) { return strcmp(c->action, GUARDX_ACTION_SET) == 0; }
static bool is_close(const actuator_cmd_t *c) { return strcmp(c->action, GUARDX_ACTION_CLOSE) == 0; }
static bool is_open(const actuator_cmd_t *c)  { return strcmp(c->action, GUARDX_ACTION_OPEN)  == 0; }
static bool is_stop(const actuator_cmd_t *c)  { return strcmp(c->action, GUARDX_ACTION_STOP)  == 0; }

static guardx_err_t handle_servo(const actuator_cmd_t *cmd, int32_t servo_id)
{
    /* 서보는 SET(각도)만 유효 (프로토콜 4-3 표). 소리 없음. */
    if (!is_set(cmd) || !cmd->has_value)
        return GUARDX_ERR_INVALID;
    if (cmd->value < 0 || cmd->value > 180)
        return GUARDX_ERR_INVALID;
    return rpic_servo_set(servo_id, cmd->value);
}

static guardx_err_t handle_servo1(const actuator_cmd_t *cmd)
{
    return handle_servo(cmd, 1);   /* servo_1 = 가스밸브 (servo_2는 삭제됨) */
}

/* 수동 팬 명령. 실제 적용은 fan_control 이 한다 - 화재 중이거나 AUTO 가
 * 켜져 있으면 거기서 거절된다. 여기서 rpic_fan_set 을 직접 부르면 그
 * 우선순위(화재 > AUTO > 수동)를 우회하게 된다. */
static guardx_err_t handle_fan(const actuator_cmd_t *cmd)
{
    if (is_on(cmd))
        return fan_control_manual(FAN_ON_DEFAULT_DUTY);
    if (is_off(cmd))
        return fan_control_manual(0);
    if (is_set(cmd) && cmd->has_value)
        return fan_control_manual(cmd->value);
    return GUARDX_ERR_INVALID;
}

/* AUTO 켜고 끄기. SET 은 받지 않는다 - 켜고 끄는 것 외에 값이 없다. */
static guardx_err_t handle_fan_auto(const actuator_cmd_t *cmd)
{
    if (is_on(cmd)) {
        fan_control_set_auto(true);
        return GUARDX_OK;
    }
    if (is_off(cmd)) {
        fan_control_set_auto(false);
        return GUARDX_OK;
    }
    return GUARDX_ERR_INVALID;
}

/* 화재셔터: MQTT에는 CLOSE/OPEN/STOP만 노출한다.
 * 실제 모터 방향은 이 계층에서만 매핑한다.
 *   CLOSE -> CCW(-1)/BOTTOM
 *   OPEN  -> CW(+1)/TOP
 *   STOP  -> 정지
 * 방향별 리밋 정지는 커널 드라이버가 IRQ로 직접 처리한다. */
static guardx_err_t handle_shutter(const actuator_cmd_t *cmd)
{
    if (is_close(cmd))
        return actuator_shutter_close();
    if (is_open(cmd))
        return rpic_stepper_set(SHUTTER_PHYSICAL_OPEN_DIR);
    if (is_stop(cmd))
        return rpic_stepper_set(STEPPER_STOP);
    return GUARDX_ERR_INVALID;
}

guardx_err_t actuator_shutter_close(void)
{
    return rpic_stepper_set(SHUTTER_PHYSICAL_CLOSE_DIR);
}

static guardx_err_t handle_pump(const actuator_cmd_t *cmd)
{
    if (is_on(cmd))
        return rpic_pump_set(1);
    if (is_off(cmd))
        return rpic_pump_set(0);
    return GUARDX_ERR_INVALID;
}

/* I2S 스피커 상황음. SET(value=scenario: 0 기본/1 화재/2 강도/3 비상) 또는
 * ON(기본음). 소리는 audio_event 워커가 비동기로 내므로 즉시 성공 반환한다.
 * 카드가 없으면 audio_event_play가 조용히 no-op이 될 뿐이다.
 *
 * OFF는 재생 중인 음을 끊는다. 화재 사이렌은 audio_arbiter가 화재 상태를
 * 보고 되풀이하므로, 여기서 끊어도 화재가 계속이면 다시 울린다 - 운영자가
 * 사이렌을 끄고 싶으면 화재 상태를 내려야지 이 명령으로는 안 된다. */
static guardx_err_t handle_sound(const actuator_cmd_t *cmd)
{
    if (is_set(cmd) && cmd->has_value) {
        audio_event_play(cmd->value);
        return GUARDX_OK;
    }
    if (is_on(cmd)) {
        audio_event_play(AUDIO_SCENE_BEEP);
        return GUARDX_OK;
    }
    if (is_off(cmd)) {
        audio_event_stop();
        return GUARDX_OK;
    }
    return GUARDX_ERR_INVALID;
}

/* 프로토콜에는 있지만 현재 하드웨어에 배선되지 않은 액추에이터
 * (led: 미배선).
 * 명령을 받으면 경고만 남기고 성공 취급하지 않는다 - RPi B가
 * 잘못된 성공 가정을 하지 않도록 INVALID 반환. */
static guardx_err_t handle_unwired(const actuator_cmd_t *cmd)
{
    fprintf(stderr, "registry: '%s' not wired on this node (ignored)\n",
            cmd->command);
    return GUARDX_ERR_INVALID;
}

/* ---------------------------------------------------------------------
 * registry
 * --------------------------------------------------------------------- */

typedef struct {
    const char   *command;
    guardx_err_t (*handler)(const actuator_cmd_t *cmd);
} actuator_entry_t;

static const actuator_entry_t actuators[] = {
    { GUARDX_CMD_SERVO_1,    handle_servo1  },
    { GUARDX_CMD_FAN,        handle_fan     },
    { GUARDX_CMD_FAN_AUTO,   handle_fan_auto },
    { GUARDX_CMD_SHUTTER,    handle_shutter },
    { GUARDX_CMD_WATER_PUMP, handle_pump    },
    { GUARDX_CMD_SOUND,      handle_sound   },   /* I2S 스피커 상황음 */
    { GUARDX_CMD_LED,        handle_unwired },   /* 미배선 */
};

#define NUM_ACTUATORS (sizeof(actuators) / sizeof(actuators[0]))

/* open/close는 시그니처가 동일해서 함수 포인터 배열로 처리 (RPi A와
 * 동일 패턴). PCA9685 하나가 서보×2+팬을 겸하므로 엔트리는 3개다. */
typedef struct {
    const char   *name;
    guardx_err_t (*open_fn)(void);
    guardx_err_t (*close_fn)(void);
} device_entry_t;

static const device_entry_t devices[] = {
    { "pca9685", rpic_pca9685_open, rpic_pca9685_close },
    { "fan",     rpic_fan_open,     rpic_fan_close     },
    { "stepper", rpic_stepper_open, rpic_stepper_close },
    { "pump",    rpic_pump_open,    rpic_pump_close    },
};

#define NUM_DEVICES (sizeof(devices) / sizeof(devices[0]))

guardx_err_t OPEN_ALL(void)
{
    size_t i;
    guardx_err_t ret;

    for (i = 0; i < NUM_DEVICES; i++) {
        ret = devices[i].open_fn();
        if (ret != GUARDX_OK) {
            fprintf(stderr, "OPEN_ALL: %s open failed (%d)\n",
                    devices[i].name, ret);
            /* 이미 열린 것들 되감기 */
            while (i > 0) {
                i--;
                devices[i].close_fn();
            }
            return ret;
        }
    }

    /* 기동 시점 상태 불명이므로 전부 꺼진 상태에서 시작 */
    apply_safe_state();

    /* I2S 상황음 워커만 시작한다. ALSA 장치는 sound 명령을 처리할 때
     * 열고 재생 직후 닫는다. 카드가 없거나 재생에 실패해도 노드는 계속 산다. */
    (void)audio_event_init(NULL);
    return GUARDX_OK;
}

guardx_err_t DISPATCH_CMD(const actuator_cmd_t *cmd)
{
    size_t i;

    if (cmd == NULL)
        return GUARDX_ERR_INVALID;

    for (i = 0; i < NUM_ACTUATORS; i++) {
        if (strcmp(cmd->command, actuators[i].command) == 0)
            return actuators[i].handler(cmd);
    }

    fprintf(stderr, "DISPATCH_CMD: unknown command '%s'\n", cmd->command);
    return GUARDX_ERR_INVALID;
}

void CLOSE_ALL(void)
{
    size_t i;

    /* 종료 시 액추에이터를 켜둔 채 나가지 않는다 */
    apply_safe_state();

    /* 오디오 워커 정지 + 진행 중 재생 중단 */
    audio_event_cleanup();

    for (i = 0; i < NUM_DEVICES; i++)
        devices[i].close_fn();
}
