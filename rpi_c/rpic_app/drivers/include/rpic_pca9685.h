#ifndef RPIC_PCA9685_H
#define RPIC_PCA9685_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ---------------------------------------------------------------------
 * 실제 하드웨어: PCA9685 16채널 12bit PWM 컨트롤러 (I2C)
 *
 * RPi C의 서보 2개(문/가스밸브)를 이 칩으로 구동한다.
 *
 * !!! 변경 이력(실물 검증 반영) !!!
 * 초기 설계에서는 DC팬도 이 칩의 CH2로 함께 구동했으나, 부팅 설정이
 * 3.5mm 아날로그 오디오(dtparam=audio=off)를 끄고 오디오를 MAX98357A
 * (I2S)로 옮기면서 RPi 내장 하드웨어 PWM 자원이 비었다. 그래서 팬은
 * PCA9685에서 떼어내 GPIO12 하드웨어 PWM(dtoverlay=pwm,pin=12,func=4)
 * 으로 이전했다 - 팬 제어는 이제 별도 드라이버(rpic_fan) 담당이다.
 * 그 결과 PCA9685는 서보 전용이 되어 채널 주파수 공유 제약이 사라졌다.
 *
 * 채널 배치 (물리 배선 기준, 배선 변경 시 여기만 수정):
 *  - CH0: Servo 가스밸브 (servo_1)   ← 문은 스텝모터(rpic_stepper)로 이관
 *  - CH1: 미사용 (servo_2 명령 삭제됨)
 *
 * 전원: 칩 로직은 3.3V(RPi), 서보 전원은 별도 5V 4A 어댑터 라인
 * (V+ 단자). RPi와 공통 GND 필수(그라운드 미연결 시 서보가 떨거나
 * 각도가 튄다 - 실물 테스트에서 확인된 흔한 실수).
 * --------------------------------------------------------------------- */

/* 논리 채널 ID (App/HAL과 공유). 물리 채널 매핑은 드라이버 내부에서. */
#define RPIC_PCA_CH_SERVO1  0   /* 가스밸브 서보 */
#define RPIC_PCA_CH_SERVO2  1   /* 미사용 (servo_2 명령 삭제) */
#define RPIC_PCA_CH_COUNT   2

/* write()/ioctl로 전달하는 제어 명령. value = 서보 각도(0~180). */
typedef struct {
    __s32 channel;   /* RPIC_PCA_CH_* */
    __s32 value;
} pca9685_cmd_t;

/* read()/ioctl로 반환하는 마지막 적용 상태 (디버깅/테스트용) */
typedef struct {
    __s32 servo1_angle;
    __s32 servo2_angle;
} pca9685_state_t;

/* ---------------------------------------------------------------------
 * GuardX_Driver_Convention.md 6번 참조 (RPi C는 0xC1부터)
 * --------------------------------------------------------------------- */
#define RPIC_PCA9685_MAGIC  0xC1
#define PCA9685_IOC_SET     _IOW(RPIC_PCA9685_MAGIC, 1, pca9685_cmd_t)
#define PCA9685_IOC_GET     _IOR(RPIC_PCA9685_MAGIC, 2, pca9685_state_t)

/* I2C 버스/주소 (RPi 기본 I2C-1, PCA9685 기본 주소 0x40.
 * A0~A5 점퍼로 주소를 바꿨다면 여기 수정) */
#define RPIC_PCA9685_I2C_BUS   1
#define RPIC_PCA9685_I2C_ADDR  0x40

/* PWM 주파수. 서보(SG90류) 표준 50Hz. */
#define PCA9685_PWM_FREQ_HZ    50

/* 서보 펄스폭 tick (4096 tick = 1주기 20ms @50Hz)
 * 실물 검증 코드(pca_sg90) 기준 0.6ms(0도)~2.4ms(180도).
 *   tick = pulse_us * 4096 * 50Hz / 1e6
 *   0.6ms -> 122.88 ≈ 123,  2.4ms -> 491.52 ≈ 492
 * !!! 참고: 검증 시 개체 특성상 실제 가동 범위가 ~160도에서 물리적으로
 *     멈췄다(전기적 명령은 0~180 그대로 두고, 기구 조립 후 필요하면
 *     펄스폭을 재보정한다) !!! */
#define SERVO_COUNT_MIN   123
#define SERVO_COUNT_MAX   492

/* value 유효 범위 (프로토콜 규약 4-3의 SET value 범위와 일치) */
#define SERVO_ANGLE_MIN   0
#define SERVO_ANGLE_MAX   180

/* 부팅/종료 시 적용하는 안전 상태.
 * !!! 미확정: 기구 구조(문 힌지 방향, 밸브 레버 방향)가 확정돼야
 * "닫힘" 각도를 정할 수 있음. 잠정 0도 !!! */
#define SERVO1_SAFE_ANGLE  0    /* 문: 평상 위치 */
#define SERVO2_SAFE_ANGLE  0    /* 가스밸브: 열림(평상) 위치 */

#endif /* RPIC_PCA9685_H */
