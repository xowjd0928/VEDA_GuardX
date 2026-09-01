#ifndef RPIA_TEMPHUM_H
#define RPIA_TEMPHUM_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 3-1 참조
 * 물리량은 float 없이 고정폭 정수 + x10 스케일로 표현한다.
 * 예: 23.5도 -> 235, 60.2% -> 602
 * --------------------------------------------------------------------- */
typedef struct {
    int16_t temperature;  /* x10 스케일 (예: 235 -> 23.5도) */
    int16_t humidity;     /* x10 스케일 (예: 602 -> 60.2%) */
} temphum_data_t;

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 6번 참조
 * 매직넘버 상위 니블: 0xA = RPi A
 * --------------------------------------------------------------------- */
#define RPIA_TEMPHUM_MAGIC   0xA2

/* App은 원칙적으로 read()로 접근하지만(3번 항목),
 * 컨벤션 6번의 ioctl 커맨드 정의 규칙에 맞춰 동일 데이터를
 * ioctl로도 얻을 수 있도록 예비 커맨드를 정의해둔다. */
#define TEMPHUM_IOC_READ     _IOR(RPIA_TEMPHUM_MAGIC, 1, temphum_data_t)

/* ---------------------------------------------------------------------
 * 실제 하드웨어: SHT30 온습도 센서 (I2C)
 *
 * 4단계 개정: 기존 DHT22류 단일 GPIO 비트뱅킹 타이밍 프로토콜을 전부
 * 폐기하고 I2C(컨벤션 8-1번)로 교체했다. RPi 기본 I2C1 버스, 기본
 * 주소 0x44(ADDR 핀이 GND일 때) 가정 - 배선 확정 후 값만 교체하면 됨.
 * --------------------------------------------------------------------- */
#define RPIA_TEMPHUM_I2C_BUS    1
#define RPIA_TEMPHUM_I2C_ADDR   0x44

#endif /* RPIA_TEMPHUM_H */
