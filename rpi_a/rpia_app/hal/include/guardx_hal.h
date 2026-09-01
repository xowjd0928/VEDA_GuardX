#ifndef GUARDX_HAL_H
#define GUARDX_HAL_H

/*
 * GuardX_App_Convention.md 1번: RPi A/C hal/ 레이어 드라이버 wrapper.
 * App은 /dev 경로·ioctl 매직넘버를 직접 다루지 않고 이 함수들만 쓴다.
 * /dev 경로는 hal/src/ 안의 소스 내부에만 등장한다.
 *
 * 모든 함수는 성공 시 GUARDX_OK(0), 실패 시 guardx_err_t 음수 반환.
 */

#include <stdint.h>
#include "guardx_err.h"

/* 드라이버와 공유하는 데이터 구조체. 커널 헤더(linux/types.h) 의존을
 * 피하기 위해 유저스페이스용으로 동일 레이아웃을 재선언한다.
 * !!! drivers/include/ 안의 헤더 구조체와 반드시 일치해야 함 !!! */
typedef struct {
    int16_t temperature;  /* x10 스케일 (235 -> 23.5도) */
    int16_t humidity;     /* x10 스케일 (602 -> 60.2%) */
} temphum_data_t;

/* rpia_adc.ko(MCP3008) 하나가 가스/스파크 아날로그 값의 유일한 커널
 * 진입점이다(GuardX_Driver_Convention.md 1-1번). RPi A는 raw 값만
 * 넘기고 환산/임계값 판단은 RPi B(판단 노드)가 한다 - 센서 노드는
 * raw, 판단 노드가 결정한다는 설계 원칙에 맞춘다. */
typedef int32_t  gas_data_t;     /* MCP3008 CH1 raw (0~1023), MQ-2 가스 */
typedef int32_t  spark_data_t;   /* MCP3008 CH0 raw (0~1023), TS0226 불꽃(반비례) */
typedef uint32_t button_data_t;  /* 마지막 read 이후 눌림 횟수 */

/* MLX90614 비접촉온도: 주변(ambient)+대상(object) x10 스케일.
 * !!! drivers/include/rpia_irtemp.h의 irtemp_data_t와 레이아웃 일치 !!! */
typedef struct {
    int16_t ambient;  /* x10 스케일 (250 -> 25.0도), 주변온도 */
    int16_t object;   /* x10 스케일 (235 -> 23.5도), 대상온도 */
} irtemp_data_t;

/* --- 가스 센서 (MQ-2, rpia_adc 경유) --- */
guardx_err_t rpia_gas_open(void);
guardx_err_t rpia_gas_read(gas_data_t *out);
guardx_err_t rpia_gas_close(void);

/* --- 온습도 센서 --- */
guardx_err_t rpia_temphum_open(void);
guardx_err_t rpia_temphum_read(temphum_data_t *out);
guardx_err_t rpia_temphum_close(void);

/* --- 불꽃 센서 (TS0226, rpia_adc 경유) --- */
guardx_err_t rpia_spark_open(void);
guardx_err_t rpia_spark_read(spark_data_t *out);
guardx_err_t rpia_spark_close(void);

/* --- 비접촉 온도 센서 (MLX90614) --- */
guardx_err_t rpia_irtemp_open(void);
guardx_err_t rpia_irtemp_read(irtemp_data_t *out);
guardx_err_t rpia_irtemp_close(void);

/* --- 비상 버튼 --- */
guardx_err_t rpia_button_open(void);
guardx_err_t rpia_button_read(button_data_t *out);
guardx_err_t rpia_button_close(void);

/* 버튼은 이벤트 루프의 poll() 대상이므로 fd 접근자가 필요하다.
 * (일반 센서는 주기 폴링이라 fd 노출 불필요 - 버튼만 예외)
 * open 전이면 -1 반환. */
int rpia_button_get_fd(void);

#endif /* GUARDX_HAL_H */
