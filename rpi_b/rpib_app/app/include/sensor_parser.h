#ifndef SENSOR_PARSER_H
#define SENSOR_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include "guardx_err.h"

/* MQTT 페이로드 최대 크기 (RPi A/C의 GUARDX_JSON_MAX와 동일값 유지) */
#define GUARDX_JSON_MAX 512

/* RPi A 센서 발행(프로토콜 규약 4-1)을 파싱한 결과.
 * RPi A json_builder.c가 만드는 신 스키마와 1:1 대응한다.
 * 필드명은 fire_schema.sql의 sensor_channel.channel_key(=DB 컬럼)와
 * 동일하게 맞춰, 나중에 파일→PostgreSQL 전환 시 재매핑이 없게 한다.
 * gas/spark는 MCP3008 raw(0~1023)를 그대로 담는다 - ppm 환산·불꽃
 * 임계 판정은 RPi B(판단 노드)의 몫(규약 raw 정책). */
typedef struct {
    uint32_t seq;             /* RPi A 발행 seq (DB 기록/역추적용) */
    int      gas_raw;         /* MQ-2 ADC raw 0~1023 (CH1) */
    int      spark_raw;       /* TS0226 불꽃 ADC raw 0~1023 (CH0) */
    double   temperature;     /* SHT30 °C ("23.5" 형태 실수) */
    double   humidity;        /* SHT30 % */
    double   irtemp_ambient;  /* MLX90614 주변 온도 °C (reg 0x06) */
    double   irtemp_object;   /* MLX90614 표면 온도 °C (reg 0x07) */
    bool     gas_valid;       /* valid=false면 해당 값은 신뢰 불가 */
    bool     temphum_valid;
    bool     spark_valid;
    bool     irtemp_valid;
} sensor_msg_t;

/* 비상 버튼 로그(프로토콜 규약 4-2) 파싱 결과 */
typedef struct {
    uint32_t seq;
    uint32_t press_count;
} button_msg_t;

/* JSON payload -> 구조체. RPi C cmd_parser와 같은 철학의 최소 파서
 * (발신자가 아군 노드뿐, 스키마 단순·고정, 외부 라이브러리 배제).
 * json은 NUL 종료 문자열이어야 한다. 필수 필드 누락 시 GUARDX_ERR_INVALID. */
guardx_err_t PARSE_SENSOR_JSON(const char *json, sensor_msg_t *out);
guardx_err_t PARSE_BUTTON_JSON(const char *json, button_msg_t *out);

#endif /* SENSOR_PARSER_H */
