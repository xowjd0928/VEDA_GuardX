#ifndef CMD_BUILDER_H
#define CMD_BUILDER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sensor_parser.h"   /* GUARDX_JSON_MAX */

/*
 * 액추에이터 명령 JSON 조립 (프로토콜 규약 4-3, RPi B -> RPi C).
 * RPi A json_builder와 같은 위치의 모듈 - B 관점에서는 "출력 직렬화".
 *
 * [프로토콜 확장 제안 - 팀 합의 전 잠정]
 * 규약 4-3 스키마에 없는 두 필드를 추가로 싣는다:
 *   "reason":     화재 확정 원인 지표 ("gas"/"temp"/"spark")
 *   "sensor_seq": 확정 시점의 RPi A 발행 seq
 * 목적: 센서 구간(A->B)과 명령 구간(B->C)은 메시지가 이어지지 않는
 * 독립 구간이라, 이 필드가 없으면 "어떤 센서값이 이 펌프를 켰는지"를
 * 역추적할 수단이 없다. RPi C의 cmd_parser는 모르는 키를 무시하므로
 * 하위 호환은 확인됨. 규약 문서 반영은 팀 합의 후.
 * reason=NULL로 부르면 두 필드를 생략한다 (규약 그대로).
 */

/* 값이 없는 의미 명령 (ON/OFF/CLOSE/OPEN/STOP) */
int CREATE_CMD_ACTION_JSON(char *buf, size_t bufsize,
                           const char *command, const char *action,
                           uint64_t timestamp_ms, uint32_t seq,
                           const char *reason, uint32_t sensor_seq);

/* SET 명령 (value 동반) */
int CREATE_CMD_SET_JSON(char *buf, size_t bufsize,
                        const char *command, int value,
                        uint64_t timestamp_ms, uint32_t seq,
                        const char *reason, uint32_t sensor_seq);

#endif /* CMD_BUILDER_H */
