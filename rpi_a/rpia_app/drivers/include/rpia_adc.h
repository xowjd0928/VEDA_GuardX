#ifndef RPIA_ADC_H
#define RPIA_ADC_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * 실제 하드웨어: MCP3008 8채널 10bit SPI ADC
 *
 * GuardX_Driver_Convention.md 1-1번(물리 칩 1개 = 모듈 1개): 이 칩
 * 하나가 MQ-2(가스)와 TS0226(불꽃) 두 센서의 아날로그 출력을 동시에
 * 서비스한다. 원래 있던 rpia_gas.ko/rpia_spark.ko는 2단계 결정으로
 * 폐기됐다(모듈 간 의존 없이 이 값을 나눠 가질 방법이 마땅치 않았음).
 * 이제 이 모듈이 gas/spark 아날로그 값의 유일한 커널 진입점이고,
 * ppm/임계값 환산은 HAL(guardx_hal.c, 유저스페이스)이 /dev/rpia_adc를
 * 직접 열어 raw 값을 읽은 뒤 수행한다.
 *
 * 채널 배치 (물리 배선 기준, 배선 변경 시 여기만 수정):
 *  - CH0: TS0226 AO (불꽃)
 *  - CH1: MQ-2 AO (가스)
 *
 * SPI 프로토콜 (MCP3008 싱글엔드 읽기, 데이터시트 5.0절):
 *  1) tx[0] = 0x01                        (start bit)
 *  2) tx[1] = 0x80 | (channel << 4)       (single-ended + 채널 선택)
 *  3) tx[2] = 0x00                        (dummy, MISO 클럭 확보용)
 *  4) 응답 rx[1..2]에서 하위 10bit 추출: ((rx[1] & 0x03) << 8) | rx[2]
 *
 * 값 범위: 0~1023 (10bit raw). ppm/임계값 환산은 이 드라이버가 아니라
 * rpia_gas/rpia_spark(또는 그 상위 App 레이어)의 책임이다 - 이 모듈은
 * "칩에서 읽은 그대로"만 넘긴다.
 * --------------------------------------------------------------------- */
typedef struct {
    uint16_t gas_raw;    /* CH1 raw (0~1023) */
    uint16_t spark_raw;  /* CH0 raw (0~1023) */
} adc_data_t;

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 6번 참조
 * --------------------------------------------------------------------- */
#define RPIA_ADC_MAGIC   0xA5
#define ADC_IOC_READ     _IOR(RPIA_ADC_MAGIC, 1, adc_data_t)

/* ---------------------------------------------------------------------
 * SPI 버스/CS/채널 번호 (하드코딩, 컨벤션 8-2번 참조)
 * RPi 기본 SPI0/CE0 가정. 배선 확정 후 값만 교체하면 됨.
 * --------------------------------------------------------------------- */
#define RPIA_ADC_SPI_BUS      0
#define RPIA_ADC_SPI_CS       0
#define RPIA_ADC_CH_SPARK     0
#define RPIA_ADC_CH_GAS       1

/* MCP3008 권장 최대 클럭(3.3V 기준 약 1.35MHz). 여유를 두고 1MHz로 고정 */
#define RPIA_ADC_SPI_HZ       1000000

#endif /* RPIA_ADC_H */
