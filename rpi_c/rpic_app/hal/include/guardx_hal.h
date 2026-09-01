#ifndef GUARDX_HAL_H
#define GUARDX_HAL_H

/*
 * GuardX_App_Convention.md 1번: RPi A/C hal/ 레이어 드라이버 wrapper.
 * App은 /dev 경로·ioctl 매직넘버를 직접 다루지 않고 이 함수들만 쓴다.
 * /dev 경로는 hal/src/ 안의 소스 내부에만 등장한다.
 *
 * RPi A HAL이 read 중심이었다면, RPi C는 액추에이터 노드라 set(write)
 * 중심이다. get은 마지막 적용 상태 확인용(디버깅/테스트).
 *
 * 모든 함수는 성공 시 GUARDX_OK(0), 실패 시 guardx_err_t 음수 반환.
 */

#include <stdint.h>
#include "guardx_err.h"

/* 드라이버와 공유하는 데이터 구조체. 커널 헤더(linux/types.h) 의존을
 * 피하기 위해 유저스페이스용으로 동일 레이아웃을 재선언한다.
 * !!! drivers/include/ 안의 헤더 구조체와 반드시 일치해야 함 !!! */

/* rpic_pca9685.h의 논리 채널 ID 재선언 (서보 전용, 팬은 별도 디바이스) */
#define RPIC_PCA_CH_SERVO1  0
#define RPIC_PCA_CH_SERVO2  1

typedef struct {
    int32_t channel;   /* RPIC_PCA_CH_* */
    int32_t value;     /* 서보 각도 0~180 */
} pca9685_cmd_t;

typedef struct {
    int32_t servo1_angle;
    int32_t servo2_angle;
} pca9685_state_t;

typedef int32_t fan_data_t;     /* 팬 듀티 0~100, 0=정지 */
typedef int32_t stepper_data_t; /* 부호 = 방향(+CW/-CCW), 0 = 정지 (연속 회전) */
typedef int32_t pump_data_t;    /* 0=OFF, 1=ON */

/* 스텝모터 방향 상수 (드라이버 rpic_stepper.h와 값 일치 필수).
 * 부호만 의미가 있으므로 양수/음수면 무엇이든 CW/CCW로 동작한다. */
#define STEPPER_STOP   0
#define STEPPER_CW     1
#define STEPPER_CCW  (-1)

/* --- PCA9685 (서보×2, /dev 노드 하나 공유) --- */
guardx_err_t rpic_pca9685_open(void);
guardx_err_t rpic_pca9685_close(void);
guardx_err_t rpic_servo_set(int32_t servo_id, int32_t angle);  /* servo_id: 1|2 */
guardx_err_t rpic_pca9685_get(pca9685_state_t *out);

/* --- DC팬 (하드웨어 PWM, GPIO12 / sysfs pwmchip0. 커널 모듈 아님) --- */
guardx_err_t rpic_fan_open(void);
guardx_err_t rpic_fan_set(fan_data_t duty);   /* 0~100, 0=정지 */
guardx_err_t rpic_fan_close(void);

/* --- 스텝모터 (28BYJ-48 + ULN2003, 연속 회전) --- */
guardx_err_t rpic_stepper_open(void);
guardx_err_t rpic_stepper_set(stepper_data_t dir);  /* +CW / -CCW / 0=정지 */
guardx_err_t rpic_stepper_close(void);

/* --- 워터펌프 (HG7881) --- */
guardx_err_t rpic_pump_open(void);
guardx_err_t rpic_pump_set(pump_data_t on);
guardx_err_t rpic_pump_close(void);

/* 앰프 전원/뮤트(rpic_amp, LM386) 제거됨: MAX98357A(I2S)의 SD_MODE는
 * ASoC 드라이버가 GPIO4로 관리한다. 소리는 app/의 rpic_audio(ALSA)가 낸다. */

#endif /* GUARDX_HAL_H */
