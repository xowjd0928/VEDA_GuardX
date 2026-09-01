#ifndef RPIC_PUMP_H
#define RPIC_PUMP_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * 실제 하드웨어: 워터펌프 + HG7881(L9110S) 2채널 모터 드라이버
 *
 * 펌프는 RPi GPIO로 직접 못 돌리므로(전류) HG7881 한 채널을 스위치로
 * 쓴다. 채널 입력 2핀(IA/IB) 논리:
 *  - IA=H, IB=L : 정방향 구동 (펌프 ON)
 *  - IA=L, IB=L : 정지 (펌프 OFF)
 *  - IA=L, IB=H : 역방향 - 펌프는 단방향 기기라 사용하지 않는다
 * 속도 제어가 필요하면 IA에 PWM을 걸 수 있지만, 소화용 펌프는
 * ON/OFF만 쓰기로 확정(프로토콜 규약 4-3)이라 디지털 제어만 구현.
 *
 * 전원: HG7881 VCC는 5V 4A 어댑터 라인, RPi와 공통 GND 필수.
 * --------------------------------------------------------------------- */
typedef __s32 pump_data_t;   /* 0=OFF, 1=ON */

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 6번 참조 (RPi C는 0xC1부터)
 * --------------------------------------------------------------------- */
#define RPIC_PUMP_MAGIC  0xC2
#define PUMP_IOC_SET     _IOW(RPIC_PUMP_MAGIC, 1, pump_data_t)
#define PUMP_IOC_GET     _IOR(RPIC_PUMP_MAGIC, 2, pump_data_t)

/* GPIO 핀 (하드코딩, HG7881 채널 B 입력 2핀)
 * !!! 임의값 - 실물 배선 후 확정 (RPi A와 동일한 지위) !!! */
#define RPIC_PUMP_GPIO_IA  23
#define RPIC_PUMP_GPIO_IB  24

#endif /* RPIC_PUMP_H */
