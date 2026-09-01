/*
 * modbus_rtu.c - GuardX RPi C Modbus RTU Client(Master) 구현 (Linux termios)
 *
 * 프레임 종료 판정: Modbus RTU 는 프레임 사이 무수신 구간(silence)으로 경계를
 * 나눈다. 여기서는 응답의 "첫 바이트"를 timeout_ms 까지 기다리고, 이후엔
 * 바이트 사이 간격이 MB_INTERBYTE_MS 를 넘으면 프레임이 끝난 것으로 본다.
 * 115200 bps 에서 3.5 char(≈0.3 ms)는 유저스페이스로 잡기엔 너무 짧아,
 * 스케줄링 지터를 감안해 넉넉히 잡되(기본 5 ms) STM 응답을 쪼개지 않는다.
 *
 * 물리계층이 RS-485 반이중으로 바뀐 뒤에도 이 파일은 그대로다 - 양 끝 변환기가
 * 자동 흐름제어형이라 DE/RE 방향전환에 소프트웨어가 관여하지 않는다. 수동
 * 모듈로 바꾸면 write 전후에 그 처리를 끼우면 된다(modbus_rtu.h 참조).
 */
/* cfmakeraw()/tcdrain() 등 glibc 확장 선언을 위해(-std=c11 로 빌드해도 안전). */
#define _DEFAULT_SOURCE

#include "modbus_rtu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>

/* udev 별칭이 있으면 이쪽을 쓴다(modbus_rtu.h 의 modbus_default_device 참조). */
#define MB_DEV_ALIAS         "/dev/guardx-rs485"
#define MB_DEV_FALLBACK      "/dev/ttyUSB0"

#define MB_INTERBYTE_MS      5      /* 프레임 종료로 볼 바이트 간 무수신(ms)  */
#define MB_DEFAULT_TIMEOUT   200    /* 설계 초기값: 응답 타임아웃 200 ms       */
#define MB_DEFAULT_RETRIES   2      /* 설계 초기값: 재시도 2회                 */
#define MB_FRAME_MAX         256    /* RTU 최대 프레임(0x03 응답 5+2*125=255)  */
#define MB_EXCEPTION_FLAG    0x80u  /* 예외 응답 Function = 요청FC | 0x80      */

struct modbus_ctx {
    int         fd;
    uint8_t     slave_id;
    int         timeout_ms;
    int         retries;
    int         verbose;
};

/* ---------------------------------------------------------------------------
 * CRC / 유틸
 * ------------------------------------------------------------------------- */

uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static void mb_append_crc(uint8_t *frame, int *len)
{
    uint16_t crc = modbus_crc16(frame, (size_t)*len);
    frame[(*len)++] = (uint8_t)(crc & 0xFFu);          /* Low 먼저 */
    frame[(*len)++] = (uint8_t)((crc >> 8) & 0xFFu);   /* High 다음 */
}

static void mb_dump(const modbus_ctx_t *ctx, const char *tag, const uint8_t *b, int n)
{
    if (!ctx->verbose)
        return;
    fprintf(stderr, "  %s (%d):", tag, n);
    for (int i = 0; i < n; i++)
        fprintf(stderr, " %02X", b[i]);
    fprintf(stderr, "\n");
}

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return (speed_t)0;   /* 미지원 */
    }
}

/* fd 가 ms 안에 읽을 수 있으면 1, 타임아웃 0, 오류 -1. */
static int wait_readable(int fd, int ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int r;
    do {
        r = poll(&pfd, 1, ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return -1;
    return (r > 0 && (pfd.revents & POLLIN)) ? 1 : 0;
}

static int write_all(int fd, const uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, (size_t)(len - off));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (int)n;
    }
    return 0;
}

/* 응답 한 프레임을 읽는다. 첫 바이트는 timeout_ms, 이후는 바이트 간
 * MB_INTERBYTE_MS 무수신을 프레임 끝으로 본다. 반환 = 수신 바이트 수(0=무응답). */
static int read_response(modbus_ctx_t *ctx, uint8_t *buf, int cap)
{
    int total = 0;

    int ready = wait_readable(ctx->fd, ctx->timeout_ms);
    if (ready <= 0)
        return 0;   /* 타임아웃/오류 → 무응답 취급 */

    for (;;) {
        ssize_t n = read(ctx->fd, buf + total, (size_t)(cap - total));
        if (n > 0) {
            total += (int)n;
            if (total >= cap)
                break;
        } else if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        } else {
            break;   /* n == 0: EOF/hangup → 정지(무한 루프 방지) */
        }
        /* 다음 바이트를 짧게 기다려 프레임 종료(무수신 간격)를 감지 */
        if (wait_readable(ctx->fd, MB_INTERBYTE_MS) <= 0)
            break;
    }
    return total;
}

/* ---------------------------------------------------------------------------
 * open / close
 * ------------------------------------------------------------------------- */

const char *modbus_default_device(void)
{
    /* access(F_OK) 로 존재만 본다 - 권한이 없어도 경로 선택은 별칭이 맞고,
     * 열기 실패는 modbus_open 이 제 사유(EACCES)로 보고하는 편이 낫다. */
    if (access(MB_DEV_ALIAS, F_OK) == 0)
        return MB_DEV_ALIAS;
    return MB_DEV_FALLBACK;
}

modbus_ctx_t *modbus_open(const modbus_cfg_t *cfg)
{
    if (cfg == NULL || cfg->device == NULL)
        return NULL;

    speed_t speed = baud_to_speed(cfg->baud);
    if (speed == (speed_t)0) {
        fprintf(stderr, "modbus_open: 미지원 baud %d\n", cfg->baud);
        return NULL;
    }

    int fd = open(cfg->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "modbus_open: %s: %s\n", cfg->device, strerror(errno));
        return NULL;
    }
    /* NONBLOCK 은 open 이 CD 대기로 막히지 않게만 쓰고, 이후 blocking 으로 */
    (void)fcntl(fd, F_SETFL, 0);

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        fprintf(stderr, "modbus_open: tcgetattr: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    cfmakeraw(&tio);                 /* 8N1 raw, ISIG/ICANON/ECHO 등 전부 off */
    tio.c_cflag |= (CLOCAL | CREAD); /* 모뎀 제어선 무시, 수신 활성           */
    tio.c_cflag &= ~CSTOPB;          /* 1 stop bit                            */
    tio.c_cflag &= ~PARENB;          /* no parity (8N1)                       */
    tio.c_cflag &= ~CRTSCTS;         /* no HW flow control                    */
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);  /* no SW flow control            */
    tio.c_cc[VMIN]  = 0;             /* poll() 로 대기하므로 non-blocking read */
    tio.c_cc[VTIME] = 0;
    if (cfsetispeed(&tio, speed) != 0 || cfsetospeed(&tio, speed) != 0) {
        fprintf(stderr, "modbus_open: cfsetspeed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        fprintf(stderr, "modbus_open: tcsetattr: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    tcflush(fd, TCIOFLUSH);

    modbus_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        close(fd);
        return NULL;
    }
    ctx->fd         = fd;
    ctx->slave_id   = cfg->slave_id;
    ctx->timeout_ms = (cfg->timeout_ms > 0) ? cfg->timeout_ms : MB_DEFAULT_TIMEOUT;
    ctx->retries    = (cfg->retries    > 0) ? cfg->retries    : MB_DEFAULT_RETRIES;
    ctx->verbose    = cfg->verbose;
    return ctx;
}

void modbus_close(modbus_ctx_t *ctx)
{
    if (ctx == NULL)
        return;
    if (ctx->fd >= 0)
        close(ctx->fd);
    free(ctx);
}

void modbus_set_slave(modbus_ctx_t *ctx, uint8_t slave_id)
{
    if (ctx != NULL)
        ctx->slave_id = slave_id;
}

void modbus_set_verbose(modbus_ctx_t *ctx, int enabled)
{
    if (ctx != NULL)
        ctx->verbose = enabled ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * 요청/응답 코어
 * ------------------------------------------------------------------------- */

/* 완성된 요청 프레임(CRC 포함)을 전송한다. expect_response 면 응답을 읽어
 * CRC 까지 검증한 뒤 resp/resp_len 에 담는다. 타임아웃/CRC 오류는 retries
 * 만큼 재전송한다. broadcast(!expect_response)는 전송만 하고 MB_OK. */
static mb_status_t mb_request(modbus_ctx_t *ctx, const uint8_t *req, int req_len,
                              int expect_response, uint8_t *resp, int *resp_len)
{
    mb_status_t last = MB_ERR_TIMEOUT;

    for (int attempt = 0; attempt <= ctx->retries; attempt++) {
        tcflush(ctx->fd, TCIFLUSH);   /* 직전 잔여 바이트 폐기 */

        mb_dump(ctx, "TX", req, req_len);
        if (write_all(ctx->fd, req, req_len) != 0)
            return MB_ERR_IO;

        if (!expect_response) {
            /* Broadcast: 응답 없음. 바이트가 완전히 나가도록 잠깐 대기. */
            tcdrain(ctx->fd);
            if (resp_len != NULL)
                *resp_len = 0;
            return MB_OK;
        }

        int n = read_response(ctx, resp, MB_FRAME_MAX);
        mb_dump(ctx, "RX", resp, n);

        if (n == 0) { last = MB_ERR_TIMEOUT; continue; }
        if (n < 5)  { last = MB_ERR_FRAME;   continue; }   /* 최소 응답 5 bytes */

        uint16_t crc_calc = modbus_crc16(resp, (size_t)(n - 2));
        uint16_t crc_recv = (uint16_t)(resp[n - 2] | (resp[n - 1] << 8));
        if (crc_calc != crc_recv) { last = MB_ERR_CRC; continue; }

        if (resp_len != NULL)
            *resp_len = n;
        return MB_OK;
    }
    return last;
}

int modbus_transceive_raw(modbus_ctx_t *ctx, const uint8_t *req, int req_len,
                          uint8_t *resp, int resp_cap)
{
    if (ctx == NULL || req == NULL || resp == NULL || req_len <= 0)
        return MB_ERR_PARAM;

    tcflush(ctx->fd, TCIFLUSH);
    mb_dump(ctx, "TX", req, req_len);
    if (write_all(ctx->fd, req, req_len) != 0)
        return MB_ERR_IO;

    int cap = (resp_cap < MB_FRAME_MAX) ? resp_cap : MB_FRAME_MAX;
    int n = read_response(ctx, resp, cap);
    mb_dump(ctx, "RX", resp, n);
    return n;
}

/* 응답의 Slave/FC 를 검사하고 예외를 걸러낸다. 정상이면 MB_OK. */
static mb_status_t check_header(modbus_ctx_t *ctx, uint8_t req_fc,
                                const uint8_t *resp, int n, uint8_t *exc_code)
{
    if (resp[0] != ctx->slave_id)
        return MB_ERR_FRAME;
    if (resp[1] == (uint8_t)(req_fc | MB_EXCEPTION_FLAG)) {
        if (exc_code != NULL)
            *exc_code = (n >= 3) ? resp[2] : 0;
        return MB_ERR_EXCEPTION;
    }
    if (resp[1] != req_fc)
        return MB_ERR_FRAME;
    return MB_OK;
}

/* ---------------------------------------------------------------------------
 * 기능 코드 API
 * ------------------------------------------------------------------------- */

mb_status_t modbus_read_holding(modbus_ctx_t *ctx, uint16_t addr, uint16_t count,
                                uint16_t *out, uint8_t *exc_code)
{
    if (ctx == NULL || out == NULL || count < 1 || count > 125)
        return MB_ERR_PARAM;
    if (ctx->slave_id == 0)          /* Broadcast 읽기는 성립하지 않음 */
        return MB_ERR_PARAM;

    uint8_t req[8]; int rl = 0;
    req[rl++] = ctx->slave_id;
    req[rl++] = 0x03;
    req[rl++] = (uint8_t)(addr >> 8);
    req[rl++] = (uint8_t)(addr & 0xFF);
    req[rl++] = (uint8_t)(count >> 8);
    req[rl++] = (uint8_t)(count & 0xFF);
    mb_append_crc(req, &rl);

    uint8_t resp[MB_FRAME_MAX]; int n = 0;
    mb_status_t st = mb_request(ctx, req, rl, 1, resp, &n);
    if (st != MB_OK)
        return st;

    st = check_header(ctx, 0x03, resp, n, exc_code);
    if (st != MB_OK)
        return st;

    uint8_t byte_count = resp[2];
    if (byte_count != (uint8_t)(2 * count) || n != (3 + byte_count + 2))
        return MB_ERR_FRAME;

    for (uint16_t i = 0; i < count; i++)
        out[i] = (uint16_t)((resp[3 + 2 * i] << 8) | resp[4 + 2 * i]);   /* Hi, Lo */
    return MB_OK;
}

mb_status_t modbus_write_single(modbus_ctx_t *ctx, uint16_t addr, uint16_t value,
                                uint8_t *exc_code)
{
    if (ctx == NULL)
        return MB_ERR_PARAM;

    uint8_t req[8]; int rl = 0;
    req[rl++] = ctx->slave_id;
    req[rl++] = 0x06;
    req[rl++] = (uint8_t)(addr >> 8);
    req[rl++] = (uint8_t)(addr & 0xFF);
    req[rl++] = (uint8_t)(value >> 8);
    req[rl++] = (uint8_t)(value & 0xFF);
    mb_append_crc(req, &rl);

    int expect = (ctx->slave_id != 0);   /* Broadcast 쓰기는 응답 없음 */
    uint8_t resp[MB_FRAME_MAX]; int n = 0;
    mb_status_t st = mb_request(ctx, req, rl, expect, resp, &n);
    if (st != MB_OK || !expect)
        return st;

    st = check_header(ctx, 0x06, resp, n, exc_code);
    if (st != MB_OK)
        return st;

    /* 정상 응답은 요청 Echo(8 bytes). 주소/값이 요청과 같아야 한다. */
    if (n != 8 ||
        resp[2] != req[2] || resp[3] != req[3] ||
        resp[4] != req[4] || resp[5] != req[5])
        return MB_ERR_FRAME;
    return MB_OK;
}

mb_status_t modbus_write_multiple(modbus_ctx_t *ctx, uint16_t addr, uint16_t count,
                                  const uint16_t *values, uint8_t *exc_code)
{
    if (ctx == NULL || values == NULL || count < 1 || count > 123)
        return MB_ERR_PARAM;

    uint8_t req[MB_FRAME_MAX]; int rl = 0;
    req[rl++] = ctx->slave_id;
    req[rl++] = 0x10;
    req[rl++] = (uint8_t)(addr >> 8);
    req[rl++] = (uint8_t)(addr & 0xFF);
    req[rl++] = (uint8_t)(count >> 8);
    req[rl++] = (uint8_t)(count & 0xFF);
    req[rl++] = (uint8_t)(2 * count);                 /* Byte Count */
    for (uint16_t i = 0; i < count; i++) {
        req[rl++] = (uint8_t)(values[i] >> 8);        /* Hi 먼저 */
        req[rl++] = (uint8_t)(values[i] & 0xFF);      /* Lo 다음 */
    }
    mb_append_crc(req, &rl);

    int expect = (ctx->slave_id != 0);
    uint8_t resp[MB_FRAME_MAX]; int n = 0;
    mb_status_t st = mb_request(ctx, req, rl, expect, resp, &n);
    if (st != MB_OK || !expect)
        return st;

    st = check_header(ctx, 0x10, resp, n, exc_code);
    if (st != MB_OK)
        return st;

    /* 정상 응답: Slave FC StartAddr(2) Quantity(2) + CRC = 8 bytes. */
    if (n != 8 ||
        resp[2] != req[2] || resp[3] != req[3] ||
        resp[4] != req[4] || resp[5] != req[5])
        return MB_ERR_FRAME;
    return MB_OK;
}

/* ---------------------------------------------------------------------------
 * 문자열
 * ------------------------------------------------------------------------- */

const char *modbus_strerror(mb_status_t s)
{
    switch (s) {
        case MB_OK:            return "OK";
        case MB_ERR_PARAM:     return "invalid argument";
        case MB_ERR_OPEN:      return "serial open/config failed";
        case MB_ERR_IO:        return "serial read/write failed";
        case MB_ERR_TIMEOUT:   return "response timeout (no reply)";
        case MB_ERR_CRC:       return "response CRC mismatch";
        case MB_ERR_FRAME:     return "malformed response";
        case MB_ERR_EXCEPTION: return "slave exception response";
        default:               return "unknown error";
    }
}

const char *modbus_exc_str(uint8_t exc_code)
{
    switch (exc_code) {
        case 0x01: return "Illegal Function";
        case 0x02: return "Illegal Data Address";
        case 0x03: return "Illegal Data Value";
        default:   return "Other";
    }
}
