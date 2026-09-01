#ifndef RPIC_STEPPER_H
#define RPIC_STEPPER_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * 실제 하드웨어: 28BYJ-48 스텝모터 + ULN2003 드라이버 보드 (문 개폐 구동)
 *                + 양방향 리밋 리드센서 2개 (CW: GPIO17, CCW: GPIO27)
 *
 * ULN2003 입력 4핀(IN1~IN4)을 RPi GPIO 4개로 직접 구동해 하프스텝
 * (8상) 시퀀스를 낸다. 실물 검증 코드(stepper.c) 기준:
 *  - 하프스텝 8상, 스텝 간 딜레이 2ms (그보다 빠르면 탈조 위험)
 *
 * 핀 배치 (물리 배선 기준, 배선 변경 시 여기만 수정):
 *  - IN1: GPIO5,  IN2: GPIO6,  IN3: GPIO16,  IN4: GPIO26
 *  - CW(+) 리밋 리드센서:  GPIO17 (풀업, 접점이 GND -> LOW=감지)
 *  - CCW(-) 리밋 리드센서: GPIO27 (풀업, 접점이 GND -> LOW=감지)
 *
 * 전원: 모터 전원은 별도 5V 4A 어댑터 라인(ULN2003 보드 +단자),
 * RPi와 공통 GND 필수.
 *
 * ---------------------------------------------------------------------
 * 동작 규격 (연속 회전 + 방향별 리밋 정지)
 *
 * 방향만 주면 정지 명령(또는 해당 방향 리밋 감지)까지 계속 도는 연속
 * 회전 방식. write()에 넘기는 값의 부호만 본다:
 *
 *   value > 0  : 시계방향(CW)으로 연속 회전 시작
 *   value < 0  : 반시계방향(CCW)으로 연속 회전 시작
 *   value == 0 : 정지 (코일 해제)
 *
 * 크기(절댓값)는 현재 무시된다(추후 속도 등으로 확장 여지). 회전은
 * 커널 스레드가 백그라운드로 돌리므로 write()는 방향만 설정하고 즉시
 * 반환한다(블로킹하지 않음).
 *
 * 방향별 리밋 정지(원본 파이썬 gpiozero 리드센서를 커널로 이관, 방향별로 분리):
 *   - CW(+)로 돌 때  GPIO17이 감지되면(LOW) 즉시 정지
 *   - CCW(-)로 돌 때 GPIO27이 감지되면(LOW) 즉시 정지
 * 각 센서는 그 방향의 막다른 지점(홈/엔드 스톱)이다. 반대 방향으로는
 * 그 센서가 감지돼 있어도 빠져나갈 수 있다(리밋에 걸린 뒤 되돌리기).
 * 두 센서 모두 IRQ(양 에지)로 감시하며, 채터링은 STEPPER_DOOR_DEBOUNCE_MS
 * 로 무시(파이썬 bounce_time=0.1과 동일). 감지/해제 이벤트는 dmesg로 확인.
 * --------------------------------------------------------------------- */
typedef __s32 stepper_data_t;   /* 부호 = 방향(+CW/-CCW), 0 = 정지 */

/* 방향 값(가독성용). 실제로는 부호만 보므로 양수/음수면 무엇이든 된다. */
#define STEPPER_STOP   0
#define STEPPER_CW     1
#define STEPPER_CCW  (-1)

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 6번 참조 (RPi C는 0xC1부터, 스텝모터는 0xC5)
 * --------------------------------------------------------------------- */
#define RPIC_STEPPER_MAGIC     0xC5
#define STEPPER_IOC_SET        _IOW(RPIC_STEPPER_MAGIC, 1, stepper_data_t)
#define STEPPER_IOC_GET        _IOR(RPIC_STEPPER_MAGIC, 2, stepper_data_t)
/* 리밋 상태 비트마스크: bit0=CW(GPIO17) 감지, bit1=CCW(GPIO27) 감지 */
#define STEPPER_IOC_GET_LIMITS _IOR(RPIC_STEPPER_MAGIC, 3, __s32)

/* GPIO 핀 (하드코딩, ULN2003 IN1~IN4 + 방향별 리밋 리드센서)
 * !!! 실물 배선 기준 확정값 (검증 코드 stepper.c와 동일) !!! */
#define RPIC_STEPPER_GPIO_IN1        5
#define RPIC_STEPPER_GPIO_IN2        6
#define RPIC_STEPPER_GPIO_IN3        16
#define RPIC_STEPPER_GPIO_IN4        26
#define RPIC_STEPPER_PIN_COUNT       4
#define RPIC_STEPPER_GPIO_CW_LIMIT   17   /* CW(+) 리밋 리드센서 (풀업 필요) */
#define RPIC_STEPPER_GPIO_CCW_LIMIT  27   /* CCW(-) 리밋 리드센서 (풀업 필요) */

/* 하프스텝 파라미터 (검증 코드 stepper.c 기준) */
#define STEPPER_HALF_STEPS       8
#define STEPPER_STEP_DELAY_US    2000
#define STEPPER_LIMIT_SAMPLES    5    /* 에지 뒤 GPIO 다중 표본 */
#define STEPPER_LIMIT_LOW_MIN    4    /* 5회 중 4회 LOW면 실제 접촉 */
#define STEPPER_LIMIT_SAMPLE_MS  10   /* 표본 간격(총 확인 약 40ms) */

#endif /* RPIC_STEPPER_H */
