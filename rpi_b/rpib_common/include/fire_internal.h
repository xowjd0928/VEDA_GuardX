#ifndef FIRE_INTERNAL_H
#define FIRE_INTERNAL_H

#include <stdbool.h>

#include "guardx_err.h"

/*
 * fire_internal.h - rpib_decision -> rpib_dispatch 내부 명령 전달
 *
 * rpib_engine을 3개 프로세스로 쪼개면서, 예전엔 fire_scenario()가 그
 * 자리에서 바로 pub_action()/pub_set()을 불러 RPi C로 명령을 냈던 것이
 * 이제 프로세스 경계를 넘어야 한다. 이 토픽이 그 경계다.
 *
 * VMS는 이 토픽을 구독하지 않는다(guardx/ 로 시작하지만 "internal"이
 * 표시하듯 RPi B 안에서만 도는 배선용 - 외부 규약 문서 대상이 아니다).
 *
 * 화재 시나리오 하나(예: 밸브+셔터+팬+펌프+경보 5건)를 배열 하나로 묶지
 * 않고, 명령 하나당 메시지 하나로 쪼갠다 - cmd_parser.c류의 기존
 * 평면 JSON 파서(중첩 배열 미지원)를 그대로 재사용하기 위해서다.
 * dispatch 쪽은 받는 즉시 그 하나만 발행 + fire_event_command 1행 기록
 * 하면 되므로, 5건을 모았다 흩는 로직이 필요 없다.
 */

#define GUARDX_TOPIC_FIRE_CMD "guardx/internal/rpib/fire_command"
#define GUARDX_QOS_FIRE_CMD   1

/* dispatch가 RPi C로 그대로 옮겨 실을 필드 + fire_event_command 기록에
 * 필요한 필드(event_id) 전부를 담는다 - dispatch는 이 구조체 하나면
 * 재조회 없이 양쪽 다 할 수 있다. */
typedef struct {
    long long event_id;        /* fire_event.event_id - fire_event_command FK */
    int       zone_id;
    char      rpic_node[32];   /* fire_zone.rpic_node_id - 발행 대상 토픽의 노드 */
    char      cause[24];       /* decision_cause_t 문자열 - RPi C 로그/사유 표시용 */
    unsigned  trigger_seq;
    char      command[24];     /* GUARDX_CMD_* (actuator_command.command_key) */
    char      action[8];       /* "ON" / "OFF" / "SET" / "OPEN" / "CLOSE" / "STOP" */
    int       value;           /* SET류만 사용 */
    bool      has_value;
} fire_cmd_msg_t;

/* out에 JSON 한 줄을 쓰고 길이를 반환한다. 실패(버퍼 부족 등) 시 음수. */
int fire_cmd_build_json(char *out, int outlen, const fire_cmd_msg_t *m);

/* json을 파싱해 out을 채운다. cmd_parser.c와 같은 평면 파서 - 필수
 * 필드(event_id/zone_id/rpic_node/command/action) 중 하나라도 없으면
 * GUARDX_ERR_INVALID. */
guardx_err_t fire_cmd_parse_json(const char *json, fire_cmd_msg_t *out);

#endif /* FIRE_INTERNAL_H */
