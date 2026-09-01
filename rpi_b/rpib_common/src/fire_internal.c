/*
 * fire_internal.c - fire_cmd_msg_t <-> JSON (평면, 중첩 없음)
 *
 * rpi_c/rpic_app/app/src/cmd_parser.c와 같은 철학·같은 파서 구조
 * (find_value/extract_string) - 이 팀 코드베이스 전체가 이 스키마
 * 단순성 전제로 라이브러리 없이 직접 처리하는 관례를 따른다.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fire_internal.h"

static const char *find_value(const char *json, const char *key)
{
    char quoted[32];
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

int fire_cmd_build_json(char *out, int outlen, const fire_cmd_msg_t *m)
{
    char value_part[32] = "";

    if (m->has_value)
        snprintf(value_part, sizeof(value_part), ",\"value\":%d", m->value);

    return snprintf(out, (size_t)outlen,
        "{\"event_id\":%lld,\"zone_id\":%d,\"rpic_node\":\"%s\","
        "\"cause\":\"%s\",\"trigger_seq\":%u,"
        "\"command\":\"%s\",\"action\":\"%s\"%s}",
        m->event_id, m->zone_id, m->rpic_node, m->cause, m->trigger_seq,
        m->command, m->action, value_part);
}

guardx_err_t fire_cmd_parse_json(const char *json, fire_cmd_msg_t *out)
{
    const char *p;

    if (!json || !out)
        return GUARDX_ERR_INVALID;

    memset(out, 0, sizeof(*out));

    p = find_value(json, "event_id");
    if (!p || !(*p == '-' || isdigit((unsigned char)*p)))
        return GUARDX_ERR_INVALID;
    out->event_id = atoll(p);

    p = find_value(json, "zone_id");
    if (!p || !(*p == '-' || isdigit((unsigned char)*p)))
        return GUARDX_ERR_INVALID;
    out->zone_id = atoi(p);

    p = find_value(json, "rpic_node");
    if (!p || extract_string(p, out->rpic_node, sizeof(out->rpic_node)) < 0)
        return GUARDX_ERR_INVALID;

    p = find_value(json, "cause");
    if (p)
        extract_string(p, out->cause, sizeof(out->cause));   /* 선택 - recovered엔 없음 */

    p = find_value(json, "trigger_seq");
    if (p && (*p == '-' || isdigit((unsigned char)*p)))
        out->trigger_seq = (unsigned)atol(p);

    p = find_value(json, "command");
    if (!p || extract_string(p, out->command, sizeof(out->command)) < 0)
        return GUARDX_ERR_INVALID;

    p = find_value(json, "action");
    if (!p || extract_string(p, out->action, sizeof(out->action)) < 0)
        return GUARDX_ERR_INVALID;

    p = find_value(json, "value");
    if (p && (*p == '-' || isdigit((unsigned char)*p))) {
        out->value = atoi(p);
        out->has_value = true;
    }

    return GUARDX_OK;
}
