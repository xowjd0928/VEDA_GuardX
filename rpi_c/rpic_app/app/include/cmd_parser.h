#ifndef CMD_PARSER_H
#define CMD_PARSER_H

#include <stddef.h>
#include <stdbool.h>
#include "guardx_err.h"

/* MQTT 페이로드 최대 크기. 액추에이터 명령 스펙
 * (node_id/timestamp/seq/command/action/value) 기준 여유있게 잡음.
 * RPi A의 GUARDX_JSON_MAX와 동일값 유지. */
#define GUARDX_JSON_MAX 512

#define CMD_NAME_MAX    32
#define ACTION_NAME_MAX 8

/* 프로토콜 규약 4-3의 액추에이터 명령을 파싱한 결과 */
typedef struct {
    char command[CMD_NAME_MAX];    /* "servo_1", "fan", "water_pump", ... */
    char action[ACTION_NAME_MAX];  /* ON/OFF/SET/CLOSE/OPEN/STOP */
    int  value;                    /* action=="SET"일 때만 유효 */
    bool has_value;                /* payload에 value 필드가 있었는지 */
    long seq;                      /* RPi B g_actuator_seq — ACK에 그대로 echo */
    bool has_seq;                  /* 옛 발신자가 seq 없이 보낼 수도 있어 방어적으로 둠 */
} actuator_cmd_t;

/* JSON payload -> actuator_cmd_t.
 * RPi A의 CREATE_JSON과 대칭으로, 외부 JSON 라이브러리 없이 직접
 * 파싱한다. 스키마가 단순·고정이고 발신자가 아군 노드(RPi B)뿐이라
 * 라이브러리 의존을 늘릴 이유가 없음.
 * json은 NUL 종료 문자열이어야 한다 (mosquitto payload는 NUL 종료가
 * 보장되지 않으므로 호출 측에서 복사+종료 처리할 것).
 * command/action 누락 시 GUARDX_ERR_INVALID. */
guardx_err_t PARSE_CMD_JSON(const char *json, actuator_cmd_t *out);

#endif /* CMD_PARSER_H */
