/*
 * cmd_builder.c - 액추에이터 명령 JSON 직렬화
 *
 * RPi A json_builder.c와 같은 철학: 스키마 단순·고정, snprintf 직접
 * 생성, 외부 라이브러리 배제.
 */

#include <stdio.h>

#include "cmd_builder.h"
#include "guardx_protocol.h"

/* 공통 envelope + command/action까지 조립. 반환: 쓴 길이 */
static int build_head(char *buf, size_t bufsize,
                      const char *command, const char *action,
                      uint64_t timestamp_ms, uint32_t seq)
{
    return snprintf(buf, bufsize,
        "{"
        "\"node_id\":\"" GUARDX_NODE_RPIB "\","
        "\"timestamp\":%llu,"
        "\"seq\":%u,"
        "\"command\":\"%s\","
        "\"action\":\"%s\"",
        (unsigned long long)timestamp_ms, seq, command, action);
}

/* 역추적 필드(확장 제안) + 닫는 중괄호. reason=NULL이면 필드 생략 */
static int build_tail(char *buf, size_t bufsize,
                      const char *reason, uint32_t sensor_seq)
{
    if (reason == NULL)
        return snprintf(buf, bufsize, "}");
    return snprintf(buf, bufsize,
                    ",\"reason\":\"%s\",\"sensor_seq\":%u}",
                    reason, sensor_seq);
}

int CREATE_CMD_ACTION_JSON(char *buf, size_t bufsize,
                           const char *command, const char *action,
                           uint64_t timestamp_ms, uint32_t seq,
                           const char *reason, uint32_t sensor_seq)
{
    int n = build_head(buf, bufsize, command, action, timestamp_ms, seq);

    if (n < 0 || (size_t)n >= bufsize)
        return n;
    return n + build_tail(buf + n, bufsize - n, reason, sensor_seq);
}

int CREATE_CMD_SET_JSON(char *buf, size_t bufsize,
                        const char *command, int value,
                        uint64_t timestamp_ms, uint32_t seq,
                        const char *reason, uint32_t sensor_seq)
{
    int n = build_head(buf, bufsize, command, GUARDX_ACTION_SET,
                       timestamp_ms, seq);

    if (n < 0 || (size_t)n >= bufsize)
        return n;
    n += snprintf(buf + n, bufsize - n, ",\"value\":%d", value);
    if ((size_t)n >= bufsize)
        return n;
    return n + build_tail(buf + n, bufsize - n, reason, sensor_seq);
}
