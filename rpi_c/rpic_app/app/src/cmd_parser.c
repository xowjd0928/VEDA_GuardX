/*
 * cmd_parser.c - MQTT 액추에이터 명령 JSON -> actuator_cmd_t 파싱
 *
 * RPi A json_builder.c와 같은 철학: 스키마가 단순·고정이라 외부 JSON
 * 라이브러리 없이 직접 처리한다. 발신자가 아군 노드(RPi B, 우리가
 * 만든 json)뿐이라는 전제하의 최소 파서다:
 *  - 키 순서 무관
 *  - 공백 허용
 *  - 중첩 객체/배열/이스케이프는 미지원 (현재 스키마에 없음)
 * 스키마가 복잡해지면 그때 라이브러리(cJSON 등) 도입을 검토할 것.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "cmd_parser.h"
#include "guardx_protocol.h"

/* "키" 다음의 값 시작 위치(콜론 뒤 첫 비공백 문자)를 찾는다.
 * 못 찾으면 NULL. 키 이름이 문자열 값 안에 등장하는 경우는
 * 고려하지 않는다(현재 스키마에서 발생 불가). */
static const char *find_value(const char *json, const char *key)
{
    char quoted[CMD_NAME_MAX + 4];
    const char *p;

    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    p = strstr(json, quoted);
    if (!p)
        return NULL;

    p += strlen(quoted);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return *p ? p : NULL;
}

/* 값 위치 p에서 "..." 문자열을 out으로 복사. 실패 시 음수. */
static int extract_string(const char *p, char *out, size_t outsize)
{
    const char *end;
    size_t len;

    if (*p != '"')
        return -1;
    p++;
    end = strchr(p, '"');
    if (!end)
        return -1;

    len = (size_t)(end - p);
    if (len >= outsize)
        return -1;

    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

guardx_err_t PARSE_CMD_JSON(const char *json, actuator_cmd_t *out)
{
    const char *p;

    if (json == NULL || out == NULL)
        return GUARDX_ERR_INVALID;

    memset(out, 0, sizeof(*out));

    /* command (필수) */
    p = find_value(json, GUARDX_JSON_KEY_COMMAND);
    if (!p || extract_string(p, out->command, sizeof(out->command)) < 0)
        return GUARDX_ERR_INVALID;

    /* action (필수) */
    p = find_value(json, GUARDX_JSON_KEY_ACTION);
    if (!p || extract_string(p, out->action, sizeof(out->action)) < 0)
        return GUARDX_ERR_INVALID;

    /* value (SET일 때만 오는 선택 필드). 프로토콜상 정수만 쓴다
     * (서보 각도, 팬 듀티 %). atoi는 뒤따르는 비숫자에서 알아서
     * 멈추므로 "value":90} 형태 그대로 넘겨도 된다. */
    p = find_value(json, GUARDX_JSON_KEY_VALUE);
    if (p && (*p == '-' || isdigit((unsigned char)*p))) {
        out->value = atoi(p);
        out->has_value = true;
    }

    /* seq (선택). ACK에 그대로 echo해서 VMS가 요청-응답을 짝짓게 한다
     * (task_vms.cpp g_actuator_seq 주석 참조). atol — RPi B가 재시작 없이
     * 오래 돌면 int 범위를 넘길 수 있어 처음부터 long으로 받는다. */
    p = find_value(json, GUARDX_JSON_KEY_SEQ);
    if (p && (*p == '-' || isdigit((unsigned char)*p))) {
        out->seq = atol(p);
        out->has_seq = true;
    }

    return GUARDX_OK;
}
