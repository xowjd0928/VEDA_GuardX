/*
 * guardx_hal.c - RPi A 디바이스 드라이버 wrapper 구현
 *
 * GuardX_App_Convention.md 1번: /dev 경로는 이 파일 내부에만 등장.
 * 나중에 디바이스 경로가 바뀌어도 이 파일의 DEV_PATH_* 만 수정.
 *
 * temphum/button/irtemp는 wrapper 패턴이 완전히 동일해 매크로로
 * 생성한다(버튼만 poll용 fd 접근자가 추가로 있음). gas/spark는
 * 공유 ADC 모듈(rpia_adc.ko)의 adc_data_t에서 각자 한 채널의 raw
 * 값만 뽑아 넘기므로(환산 없음 - 임계값 판단은 RPi B의 몫) 매크로
 * 대신 개별 구현으로 뺐다(아래 참조).
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "guardx_hal.h"

#define DEV_PATH_ADC      "/dev/rpia_adc"
#define DEV_PATH_TEMPHUM  "/dev/rpia_temphum"
#define DEV_PATH_IRTEMP   "/dev/rpia_irtemp"
#define DEV_PATH_BUTTON   "/dev/rpia_button"

/* 디바이스별 fd. -1 = 미오픈.
 * fd_gas/fd_spark는 둘 다 DEV_PATH_ADC를 각자 독립적으로 open한다
 * (같은 /dev/rpia_adc를 fd 2개로 따로 열어 각자 read() - 커널
 * 드라이버가 매 read()마다 두 채널을 함께 재측정하므로 정합성
 * 문제는 없고, SPI 트랜잭션이 마이크로초 단위라 1Hz 폴링에서
 * 중복 비용도 무시할 수준. device_registry.c가 gas/spark를 여전히
 * 별도 registry 항목으로 다루므로 이 방식이 가장 덜 침습적이다). */
static int fd_gas     = -1;
static int fd_temphum = -1;
static int fd_spark   = -1;
static int fd_irtemp  = -1;
static int fd_button  = -1;

/* rpia_adc.ko의 adc_data_t와 반드시 레이아웃 일치
 * (drivers/include/rpia_adc.h 참조) */
typedef struct {
    uint16_t gas_raw;
    uint16_t spark_raw;
} adc_data_t;

static guardx_err_t read_adc_raw(int fd, adc_data_t *out)
{
    ssize_t n = read(fd, out, sizeof(adc_data_t));

    if (n != (ssize_t)sizeof(adc_data_t))
        return GUARDX_ERR_READ;
    return GUARDX_OK;
}

guardx_err_t rpia_gas_open(void)
{
    if (fd_gas >= 0)
        return GUARDX_OK;
    fd_gas = open(DEV_PATH_ADC, O_RDONLY);
    if (fd_gas < 0) {
        perror(DEV_PATH_ADC " open (gas)");
        return GUARDX_ERR_OPEN;
    }
    return GUARDX_OK;
}

guardx_err_t rpia_gas_read(gas_data_t *out)
{
    adc_data_t raw;
    guardx_err_t ret;

    if (out == NULL)
        return GUARDX_ERR_INVALID;
    if (fd_gas < 0)
        return GUARDX_ERR_NOT_OPEN;

    ret = read_adc_raw(fd_gas, &raw);
    if (ret != GUARDX_OK)
        return ret;

    /* MCP3008 CH1 raw(0~1023) 그대로. ppm 환산/임계값 판단은 RPi B */
    *out = (gas_data_t)raw.gas_raw;
    return GUARDX_OK;
}

guardx_err_t rpia_gas_close(void)
{
    if (fd_gas < 0)
        return GUARDX_OK;
    if (close(fd_gas) < 0) {
        fd_gas = -1;
        return GUARDX_ERR_CLOSE;
    }
    fd_gas = -1;
    return GUARDX_OK;
}

guardx_err_t rpia_spark_open(void)
{
    if (fd_spark >= 0)
        return GUARDX_OK;
    fd_spark = open(DEV_PATH_ADC, O_RDONLY);
    if (fd_spark < 0) {
        perror(DEV_PATH_ADC " open (spark)");
        return GUARDX_ERR_OPEN;
    }
    return GUARDX_OK;
}

guardx_err_t rpia_spark_read(spark_data_t *out)
{
    adc_data_t raw;
    guardx_err_t ret;

    if (out == NULL)
        return GUARDX_ERR_INVALID;
    if (fd_spark < 0)
        return GUARDX_ERR_NOT_OPEN;

    ret = read_adc_raw(fd_spark, &raw);
    if (ret != GUARDX_OK)
        return ret;

    /* MCP3008 CH0 raw(0~1023) 그대로. TS0226은 불꽃일수록 raw가
     * 낮아짐(반비례) - "raw < 임계값이면 불꽃" 판단은 RPi B의 몫 */
    *out = (spark_data_t)raw.spark_raw;
    return GUARDX_OK;
}

guardx_err_t rpia_spark_close(void)
{
    if (fd_spark < 0)
        return GUARDX_OK;
    if (close(fd_spark) < 0) {
        fd_spark = -1;
        return GUARDX_ERR_CLOSE;
    }
    fd_spark = -1;
    return GUARDX_OK;
}

/* 공통 wrapper 생성 매크로:
 *  NAME: 함수 접두사 (rpia_gas 등)
 *  FD:   해당 static fd 변수
 *  PATH: /dev 경로
 *  TYPE: read 데이터 타입
 *  FLAGS: open 플래그 */
#define DEFINE_HAL_WRAPPERS(NAME, FD, PATH, TYPE, FLAGS)                  \
    guardx_err_t NAME##_open(void)                                        \
    {                                                                     \
        if (FD >= 0)                                                      \
            return GUARDX_OK; /* 이미 열려있으면 성공 취급 */             \
        FD = open(PATH, FLAGS);                                           \
        if (FD < 0) {                                                     \
            perror(PATH " open");                                         \
            return GUARDX_ERR_OPEN;                                       \
        }                                                                 \
        return GUARDX_OK;                                                 \
    }                                                                     \
    guardx_err_t NAME##_read(TYPE *out)                                   \
    {                                                                     \
        ssize_t n;                                                        \
        if (out == NULL)                                                  \
            return GUARDX_ERR_INVALID;                                    \
        if (FD < 0)                                                       \
            return GUARDX_ERR_NOT_OPEN;                                   \
        n = read(FD, out, sizeof(TYPE));                                  \
        if (n != (ssize_t)sizeof(TYPE))                                   \
            return GUARDX_ERR_READ;                                       \
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

DEFINE_HAL_WRAPPERS(rpia_temphum, fd_temphum, DEV_PATH_TEMPHUM, temphum_data_t, O_RDONLY)
DEFINE_HAL_WRAPPERS(rpia_irtemp,  fd_irtemp,  DEV_PATH_IRTEMP,  irtemp_data_t,  O_RDONLY)

/* 버튼은 이벤트 루프에서 poll()로 감시해야 하므로 논블로킹으로 연다.
 * (poll에서 POLLIN 확인 후 read하므로 실제로 블로킹될 일은 없지만,
 *  방어적으로 O_NONBLOCK 지정) */
DEFINE_HAL_WRAPPERS(rpia_button,  fd_button,  DEV_PATH_BUTTON,  button_data_t,  O_RDONLY | O_NONBLOCK)

int rpia_button_get_fd(void)
{
    return fd_button;
}
