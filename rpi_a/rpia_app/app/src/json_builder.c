/*
 * json_builder.c - 센서 스냅샷 -> MQTT JSON 페이로드 직렬화
 *
 * 외부 JSON 라이브러리 없이 snprintf로 직접 생성한다.
 * 스키마가 단순·고정이라 라이브러리 의존을 늘릴 이유가 없음.
 *
 * x10 정수 -> 실수 변환은 여기서만 수행한다(App 컨벤션).
 * float 포맷팅 대신 정수 나눗셈/나머지로 "%d.%d"를 만들어
 * 로케일/부동소수점 오차 문제를 피한다.
 */

#include <stdio.h>
#include <stdlib.h>
#include "json_builder.h"
#include "guardx_protocol.h"

/* x10 int16 값을 "23.5" / "-5.3" 형태 문자열로 변환 */
static void x10_to_str(int16_t v, char *out, size_t outsize)
{
    int16_t whole = v / 10;
    int16_t frac  = abs(v % 10);

    /* -0.5처럼 정수부가 0인 음수는 부호가 사라지므로 별도 처리 */
    if (v < 0 && whole == 0)
        snprintf(out, outsize, "-0.%d", frac);
    else
        snprintf(out, outsize, "%d.%d", whole, frac);
}

int CREATE_JSON(char *buf, size_t bufsize, const sensor_snapshot_t *snap,
                uint64_t timestamp_ms, uint32_t seq)
{
    char temp_str[8], hum_str[8], ir_amb_str[8], ir_obj_str[8];

    x10_to_str(snap->temphum.temperature, temp_str, sizeof(temp_str));
    x10_to_str(snap->temphum.humidity, hum_str, sizeof(hum_str));
    x10_to_str(snap->irtemp.ambient, ir_amb_str, sizeof(ir_amb_str));
    x10_to_str(snap->irtemp.object, ir_obj_str, sizeof(ir_obj_str));

    /* gas/spark는 HAL이 MCP3008 raw(0~1023)를 그대로 넘긴다.
     * ppm 환산·불꽃 임계값 판단은 RPi B(판단 노드)의 몫. */
    return snprintf(buf, bufsize,
        "{"
        "\"node_id\":\"" GUARDX_NODE_RPIA "\","
        "\"timestamp\":%llu,"
        "\"seq\":%u,"
        "\"values\":{"
            "\"gas_raw\":%d,"
            "\"temperature\":%s,"
            "\"humidity\":%s,"
            "\"spark_raw\":%d,"
            "\"irtemp_ambient\":%s,"
            "\"irtemp_object\":%s"
        "},"
        "\"valid\":{"
            "\"gas\":%s,"
            "\"temphum\":%s,"
            "\"spark\":%s,"
            "\"irtemp\":%s"
        "}"
        "}",
        (unsigned long long)timestamp_ms,
        seq,
        snap->gas,
        temp_str,
        hum_str,
        snap->spark,
        ir_amb_str,
        ir_obj_str,
        snap->gas_valid     ? "true" : "false",
        snap->temphum_valid ? "true" : "false",
        snap->spark_valid   ? "true" : "false",
        snap->irtemp_valid  ? "true" : "false");
}

int CREATE_BUTTON_JSON(char *buf, size_t bufsize, uint32_t press_count,
                       uint64_t timestamp_ms, uint32_t seq)
{
    return snprintf(buf, bufsize,
        "{"
        "\"node_id\":\"" GUARDX_NODE_RPIA "\","
        "\"timestamp\":%llu,"
        "\"seq\":%u,"
        "\"event\":\"emergency_button\","
        "\"press_count\":%u"
        "}",
        (unsigned long long)timestamp_ms,
        seq,
        press_count);
}
