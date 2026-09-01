#ifndef JSON_BUILDER_H
#define JSON_BUILDER_H

#include <stddef.h>
#include <stdint.h>
#include "device_registry.h"

/* MQTT 페이로드 최대 크기. 현재 스펙(node_id/timestamp/seq/values/valid)
 * 기준 여유있게 잡음 */
#define GUARDX_JSON_MAX 512

/* READ_ALL 스냅샷 -> 센서 데이터 JSON.
 * x10 정수 -> 실수 변환은 App 컨벤션에 따라 이 직렬화 시점에서만 수행.
 * 반환: 생성된 문자열 길이 (snprintf 의미), 버퍼 부족 시 GUARDX_JSON_MAX 이상 */
int CREATE_JSON(char *buf, size_t bufsize, const sensor_snapshot_t *snap,
                uint64_t timestamp_ms, uint32_t seq);

/* 비상 버튼 이벤트 JSON (로깅 경로, MQTT QoS2용) */
int CREATE_BUTTON_JSON(char *buf, size_t bufsize, uint32_t press_count,
                       uint64_t timestamp_ms, uint32_t seq);

#endif /* JSON_BUILDER_H */