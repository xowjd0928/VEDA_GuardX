/*
 * guardx_hal.c - RPi C 디바이스 드라이버 wrapper 구현
 *
 * GuardX_App_Convention.md 1번: /dev 경로는 이 파일 내부에만 등장.
 * 나중에 디바이스 경로가 바뀌어도 이 파일의 DEV_PATH_* 만 수정.
 *
 * RPi A HAL과 대칭 구조이되 방향이 반대다(read -> write).
 * pump/amp/stepper는 wrapper 패턴이 동일해 매크로로 생성하고,
 * PCA9685만 채널 인자가 있어 개별 구현한다.
 *
 * 팬만 예외: 커널 6.8+에서 레거시 in-kernel PWM API(pwm_request 등)가
 * 제거돼 커널 모듈 대신 sysfs PWM(/sys/class/pwm/pwmchip0)으로 제어한다.
 * 이는 실물 검증 코드(pwm.c)가 쓴 바로 그 경로이며 sysfs ABI는 커널
 * 버전에 안정적이다. 그래서 팬은 /dev 노드가 없고 이 파일에서 sysfs를
 * 직접 연다 (dtoverlay=pwm,pin=12,func=4 필요).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "guardx_hal.h"

#define DEV_PATH_PCA9685  "/dev/rpic_pca9685"
#define DEV_PATH_STEPPER  "/dev/rpic_stepper"
#define DEV_PATH_PUMP     "/dev/rpic_pump"
/* rpic_amp 제거됨: MAX98357A SD_MODE는 ASoC, 소리는 rpic_audio(ALSA) 담당 */

/* --- 팬 sysfs PWM 경로/파라미터 (검증 코드 pwm.c 기준) --- */
#define FAN_PWMCHIP       "/sys/class/pwm/pwmchip0"
#define FAN_PWM_DIR       FAN_PWMCHIP "/pwm0"
#define FAN_PERIOD_NS     40000L    /* 25kHz (1e9/25000) */
#define FAN_EXPORT_TRIES  100
#define FAN_EXPORT_WAIT_US 10000

/* 디바이스별 fd. -1 = 미오픈 */
static int fd_pca9685 = -1;
static int fd_stepper = -1;
static int fd_pump    = -1;
/* 팬은 fd가 아니라 sysfs 준비 상태로 관리:
 *   0 = 미오픈, 1 = 실 sysfs 사용, 2 = soft(no-op) 모드
 * pwmchip0가 없으면(오버레이 미구성/시뮬레이션 환경) 노드 전체를 죽이지
 * 않고 팬만 no-op로 두어, 커널 모듈의 simulate 역할을 대신한다. */
#define FAN_MODE_CLOSED 0
#define FAN_MODE_REAL   1
#define FAN_MODE_SOFT   2
static int fan_ready = FAN_MODE_CLOSED;

/* 공통 wrapper 생성 매크로 (open/set/close, 단일 값 write 디바이스용):
 *  NAME: 함수 접두사 (rpic_pump 등)
 *  FD:   해당 static fd 변수
 *  PATH: /dev 경로
 *  TYPE: write 데이터 타입 */
#define DEFINE_HAL_WRAPPERS(NAME, FD, PATH, TYPE)                         \
    guardx_err_t NAME##_open(void)                                       \
    {                                                                     \
        if (FD >= 0)                                                      \
            return GUARDX_OK; /* 이미 열려있으면 성공 취급 */             \
        FD = open(PATH, O_RDWR);                                          \
        if (FD < 0) {                                                     \
            perror(PATH " open");                                         \
            return GUARDX_ERR_OPEN;                                       \
        }                                                                 \
        return GUARDX_OK;                                                 \
    }                                                                     \
    guardx_err_t NAME##_set(TYPE on)                                      \
    {                                                                     \
        ssize_t n;                                                        \
        if (FD < 0)                                                       \
            return GUARDX_ERR_NOT_OPEN;                                   \
        n = write(FD, &on, sizeof(TYPE));                                 \
        if (n != (ssize_t)sizeof(TYPE))                                   \
            return GUARDX_ERR_WRITE;                                      \
        return GUARDX_OK;                                                 \
    }                                                                     \
    guardx_err_t NAME##_close(void)                                       \
    {                                                                     \
        if (FD < 0)                                                       \
            return GUARDX_OK; /* 이미 닫혀있으면 성공 취급 */             \
        if (close(FD) < 0) {                                              \
            FD = -1;                                                      \
            return GUARDX_ERR_CLOSE;                                      \
        }                                                                 \
        FD = -1;                                                          \
        return GUARDX_OK;                                                 \
    }

/* 스텝모터(방향)도 단일 int32 값 write라 같은 매크로로 생성.
 * (매크로 인자명이 'on'이지만 stepper_data_t 의미는 방향: +CW/-CCW/0=정지) */
DEFINE_HAL_WRAPPERS(rpic_stepper, fd_stepper, DEV_PATH_STEPPER, stepper_data_t)
DEFINE_HAL_WRAPPERS(rpic_pump, fd_pump, DEV_PATH_PUMP, pump_data_t)

/* --- 팬: 커널 모듈 대신 sysfs PWM으로 제어 (상단 주석 참조) --- */

/* sysfs 파일 하나에 문자열을 쓴다. 성공 0, 실패 -1. */
static int fan_sysfs_write(const char *path, const char *val)
{
    ssize_t len = (ssize_t)strlen(val);
    ssize_t n;
    int fd = open(path, O_WRONLY);

    if (fd < 0)
        return -1;
    n = write(fd, val, (size_t)len);
    close(fd);
    return (n == len) ? 0 : -1;
}

guardx_err_t rpic_fan_open(void)
{
    char period_str[16];
    int i;

    if (fan_ready)
        return GUARDX_OK;

    /* pwmchip0 자체가 없으면(오버레이 미구성/시뮬레이션) soft 모드로 진행.
     * 노드 전체를 죽이지 않되, 실물이면 팬이 안 도는 것이므로 경고를 남긴다. */
    if (access(FAN_PWMCHIP, F_OK) != 0) {
        fprintf(stderr,
                "rpic_fan: %s 없음 - sysfs PWM 미구성, 팬 no-op(soft) 모드로 진행 "
                "(실물이면 config.txt의 dtoverlay=pwm,pin=12,func=4 확인)\n",
                FAN_PWMCHIP);
        fan_ready = FAN_MODE_SOFT;
        return GUARDX_OK;
    }

    /* pwm0이 아직 export 안 됐으면 export (이미 돼 있으면 그대로 사용) */
    if (access(FAN_PWM_DIR, F_OK) != 0)
        (void)fan_sysfs_write(FAN_PWMCHIP "/export", "0");

    /* export 반영까지 udev가 노드를 만들 시간을 준다 */
    for (i = 0; i < FAN_EXPORT_TRIES && access(FAN_PWM_DIR, F_OK) != 0; i++)
        usleep(FAN_EXPORT_WAIT_US);
    if (access(FAN_PWM_DIR, F_OK) != 0) {
        perror(FAN_PWM_DIR " export");
        return GUARDX_ERR_OPEN;
    }

    /* 검증 코드(pwm.c)와 동일 순서: 비활성 -> 듀티0 -> 주기 -> 듀티0 -> 활성.
     * (freshly export된 채널은 period=0이라 duty를 먼저 0으로 둔 뒤 period
     *  를 설정해야 한다) */
    snprintf(period_str, sizeof(period_str), "%ld", FAN_PERIOD_NS);
    (void)fan_sysfs_write(FAN_PWM_DIR "/enable", "0");
    (void)fan_sysfs_write(FAN_PWM_DIR "/duty_cycle", "0");
    if (fan_sysfs_write(FAN_PWM_DIR "/period", period_str) != 0 ||
        fan_sysfs_write(FAN_PWM_DIR "/duty_cycle", "0") != 0 ||
        fan_sysfs_write(FAN_PWM_DIR "/enable", "1") != 0) {
        perror(FAN_PWM_DIR " configure");
        return GUARDX_ERR_OPEN;
    }

    fan_ready = FAN_MODE_REAL;
    return GUARDX_OK;
}

guardx_err_t rpic_fan_set(fan_data_t duty)
{
    char buf[16];
    long duty_ns;

    if (fan_ready == FAN_MODE_CLOSED)
        return GUARDX_ERR_NOT_OPEN;
    if (duty < 0 || duty > 100)
        return GUARDX_ERR_INVALID;
    if (fan_ready == FAN_MODE_SOFT)
        return GUARDX_OK;   /* soft 모드: 검증만, 실제 출력 없음 */

    duty_ns = (long)duty * FAN_PERIOD_NS / 100;
    snprintf(buf, sizeof(buf), "%ld", duty_ns);
    if (fan_sysfs_write(FAN_PWM_DIR "/duty_cycle", buf) != 0)
        return GUARDX_ERR_WRITE;
    return GUARDX_OK;
}

guardx_err_t rpic_fan_close(void)
{
    if (fan_ready == FAN_MODE_CLOSED)
        return GUARDX_OK;
    /* PWM은 켜둔 채 듀티만 0 - 비활성 출력 상태를 확실히 보장(검증 코드와 동일) */
    if (fan_ready == FAN_MODE_REAL)
        (void)fan_sysfs_write(FAN_PWM_DIR "/duty_cycle", "0");
    fan_ready = FAN_MODE_CLOSED;
    return GUARDX_OK;
}

/* --- PCA9685: 채널 인자가 있어 매크로 대신 개별 구현 --- */

guardx_err_t rpic_pca9685_open(void)
{
    if (fd_pca9685 >= 0)
        return GUARDX_OK;
    fd_pca9685 = open(DEV_PATH_PCA9685, O_RDWR);
    if (fd_pca9685 < 0) {
        perror(DEV_PATH_PCA9685 " open");
        return GUARDX_ERR_OPEN;
    }
    return GUARDX_OK;
}

static guardx_err_t pca9685_write_cmd(int32_t channel, int32_t value)
{
    pca9685_cmd_t cmd = { channel, value };
    ssize_t n;

    if (fd_pca9685 < 0)
        return GUARDX_ERR_NOT_OPEN;
    n = write(fd_pca9685, &cmd, sizeof(cmd));
    if (n != (ssize_t)sizeof(cmd))
        return GUARDX_ERR_WRITE;
    return GUARDX_OK;
}

guardx_err_t rpic_servo_set(int32_t servo_id, int32_t angle)
{
    if (servo_id != 1 && servo_id != 2)
        return GUARDX_ERR_INVALID;
    return pca9685_write_cmd(servo_id == 1 ? RPIC_PCA_CH_SERVO1
                                           : RPIC_PCA_CH_SERVO2, angle);
}

guardx_err_t rpic_pca9685_get(pca9685_state_t *out)
{
    ssize_t n;

    if (out == NULL)
        return GUARDX_ERR_INVALID;
    if (fd_pca9685 < 0)
        return GUARDX_ERR_NOT_OPEN;
    n = read(fd_pca9685, out, sizeof(pca9685_state_t));
    if (n != (ssize_t)sizeof(pca9685_state_t))
        return GUARDX_ERR_READ;
    return GUARDX_OK;
}

guardx_err_t rpic_pca9685_close(void)
{
    if (fd_pca9685 < 0)
        return GUARDX_OK;
    if (close(fd_pca9685) < 0) {
        fd_pca9685 = -1;
        return GUARDX_ERR_CLOSE;
    }
    fd_pca9685 = -1;
    return GUARDX_OK;
}
