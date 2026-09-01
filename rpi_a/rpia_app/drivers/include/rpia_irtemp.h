#ifndef RPIA_IRTEMP_H
#define RPIA_IRTEMP_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * 실제 하드웨어: MLX90614ESF 비접촉 적외선 온도 센서 (SMBus/I2C)
 *
 * 다이어그램(RPi-S) 대조로 새로 추가된 센서 (5단계). RAM 레지스터
 * 0x07(Tobj1, 대상 표면 온도)을 읽는다. SMBus PEC(패킷 오류 검출,
 * CRC-8)를 기본 지원하는 칩이라 i2c_smbus_read_word_data() +
 * I2C_CLIENT_PEC 플래그로 커널이 자동 검증하게 한다(컨벤션 8-1번
 * "SMBus 헬퍼 우선" 원칙을 그대로 따를 수 있는 경우 - SHT30처럼
 * raw 트랜잭션이 필요 없음).
 *
 * 데이터 형식(데이터시트 8.3.1절): 16bit 중 bit15=에러 플래그,
 * bit0~14=온도(0.02K 단위, 0=0K 기준). 변환식:
 *   Kelvin = raw * 0.02
 *   Celsius = Kelvin - 273.15
 * 커널 공간은 부동소수점 금지라 밀리단위 정수로 계산한다:
 *   raw * 0.02K = raw * 20 mK (0.02K가 정확히 20mK라 반올림 손실 없음)
 *   milliCelsius = raw*20 - 273150
 *   x10 스케일(deci-도) = milliCelsius / 100
 *
 * RAM 레지스터 2개를 읽는다:
 *   Tamb (0x06)  = 주변(칩 내부 기준) 온도
 *   Tobj1 (0x07) = 비접촉으로 겨눈 대상 표면 온도
 * --------------------------------------------------------------------- */
typedef struct {
    int16_t ambient;   /* x10 스케일, 주변온도 (Tamb, 0x06) */
    int16_t object;    /* x10 스케일, 대상온도 (Tobj1, 0x07) */
} irtemp_data_t;

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 6번 참조. 0xA5(rpia_adc) 다음이라 0xA6.
 * --------------------------------------------------------------------- */
#define RPIA_IRTEMP_MAGIC   0xA6
#define IRTEMP_IOC_READ     _IOR(RPIA_IRTEMP_MAGIC, 1, irtemp_data_t)

/* ---------------------------------------------------------------------
 * I2C 버스/주소 (하드코딩, 컨벤션 8-1번 참조).
 * MLX90614 기본 슬레이브 주소 0x5A. 배선 확정 후 값만 교체하면 됨.
 * --------------------------------------------------------------------- */
#define RPIA_IRTEMP_I2C_BUS    1
#define RPIA_IRTEMP_I2C_ADDR   0x5A

#endif /* RPIA_IRTEMP_H */
