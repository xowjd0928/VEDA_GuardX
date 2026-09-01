/*
 * matrix_link.c - LED 매트릭스 표시 페이로드 -> Modbus 레지스터
 *
 * 설계 배경과 페이로드 규격은 matrix_link.h 참조.
 *
 * JSON은 cmd_parser.c와 같은 철학으로 직접 훑는다 - 스키마가 정수 몇
 * 개로 고정이고 발신자가 아군 노드(RPi B)뿐이라 라이브러리를 들일 이유가
 * 없다. 중첩/배열/이스케이프는 이 스키마에 없다.
 */
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_arbiter.h"
#include "actuator_registry.h"
#include "fan_control.h"
#include "guardx_modbus_regs.h"
#include "matrix_link.h"
#include "modbus_rtu.h"

/* 좌표 다섯 개(status + A.x A.y B.x B.y)는 STM32가 FC10 한 프레임으로만
 * 받는다 - 네 좌표가 섞인 중간 상태를 화면에 그리지 않기 위한 규약이다
 * (modbus_slave.c의 intruder_block 검사). 유일한 예외가 status=0이라
 * "지우기"만 FC06 단일 쓰기로 나간다. */
#define TRACK_REG_COUNT  5

/* zone 하나가 차지하는 칸 수 (온도, 습도). 레지스터 맵이 100부터
 * temp,humid,temp,humid... 로 번갈아 놓여 있어 zone N의 시작 주소는
 * REG_ZONE1_TEMP_X10 + (N-1)*ZONE_REG_COUNT 로 구해진다. */
#define ZONE_REG_COUNT   2

/* 한 우편함이 담을 수 있는 최대 레지스터 수 = 가장 긴 표시(추적 5개). */
#define SLOT_REGS_MAX    TRACK_REG_COUNT

/* 페이로드 상한. 실제는 120바이트 남짓이고 cmd_parser.h의 GUARDX_JSON_MAX와
 * 같은 값이지만, 그 헤더는 액추에이터 스키마용이라 여기서 끌어오지 않는다. */
#define MATRIX_JSON_MAX  512

/* 시리얼 기본값은 modbus_default_device()가 고른다 - modbus_test와 같은 함수를
 * 쓰므로 둘이 어긋날 수 없다(어긋나면 CLI로 확인한 배선이 데몬에서 안 되는
 * 혼란이 생긴다).
 *
 * RS-485 전환 후 RPi C는 USB 변환기(SZH-CVBE-008)로 붙으므로 GPIO UART
 * (/dev/serial0)가 아니라 USB 시리얼이다. 번호(ttyUSB0/1/...)는 뽑았다 꽂을
 * 때마다 바뀌므로 udev 별칭 /dev/guardx-rs485가 있으면 그쪽을 우선한다.
 * 명시 지정은 여전히 GUARDX_MATRIX_DEV로 한다. */
#define MATRIX_BAUD          115200
#define MATRIX_TIMEOUT_MS    200
#define MATRIX_RETRIES       2

/*
 * 우편함 배치. 표시 종류마다 한 칸씩 - 칸을 공유하면 서로 다른 레지스터
 * 구간인데도 나중 값이 앞 값을 덮어 유실시킨다(matrix_link.h 참조).
 */
enum {
    SLOT_TRACK = 0,
    SLOT_FIRE,
    SLOT_ZONE_BASE,                                 /* +0..+3 = zone 1..4 */
    SLOT_COUNT = SLOT_ZONE_BASE + GX_ZONE_COUNT
};

typedef struct {
    uint16_t start;                  /* 시작 레지스터 PDU 주소 */
    uint16_t count;                  /* 쓸 개수. 1이면 FC06, 2 이상이면 FC10 */
    uint16_t regs[SLOT_REGS_MAX];
    int      pending;                /* 1 = 아직 안 쓴 값이 있다 */
} slot_t;

static slot_t           slots[SLOT_COUNT];
static int              cursor;      /* 워커 전용 - 라운드로빈 시작 위치 */
static int              worker_stop;
static int              worker_alive;
static pthread_t        worker;
static pthread_mutex_t  lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   wake = PTHREAD_COND_INITIALIZER;

static modbus_ctx_t    *ctx;
/* A retained active fire received after restart is also treated as a rising
 * edge. Recovery only resets this latch; it never opens the shutter. */
static bool             fire_response_active;

/* 연속 실패 로그 억제. 배선이 빠지면 매 갱신마다 같은 줄이 쌓여
 * 정작 봐야 할 로그를 덮는다. 시리얼 하나의 건강 상태라 표시 종류별로
 * 나누지 않는다 - 끊기면 어차피 전부 같이 실패한다. */
static int fail_streak;
#define FAIL_LOG_LIMIT  5

static const char *slot_name(int idx)
{
    if (idx == SLOT_TRACK)
        return "추적";
    if (idx == SLOT_FIRE)
        return "화재";
    return "온습도";
}

/*
 * "키":정수 하나 뽑기. 찾으면 0, 없거나 정수가 아니면 -1.
 *
 * 키 이름이 문자열 값 안에 등장하는 경우는 고려하지 않는다(이 스키마에
 * 문자열 값은 node_id뿐이고 그 값에 좌표 키가 들어갈 일이 없다).
 */
static int jint(const char *json, const char *key, long *out)
{
    char pat[24];
    const char *p;
    char *end;
    long v;

    if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat))
        return -1;

    p = strstr(json, pat);
    if (p == NULL)
        return -1;

    p += strlen(pat);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;

    errno = 0;
    v = strtol(p, &end, 10);
    if (end == p || errno != 0)
        return -1;

    *out = v;
    return 0;
}

/* mosquitto payload는 NUL 종료 보장이 없다. 복사해서 종료시킨다.
 * 성공 0, 길이가 이상하면 -1. */
static int copy_payload(const char *payload, int len, char *buf, size_t cap)
{
    if (payload == NULL || len <= 0 || len >= (int)cap) {
        fprintf(stderr, "matrix: payload 길이 이상 (%d) - 버림\n", len);
        return -1;
    }
    memcpy(buf, payload, (size_t)len);
    buf[len] = '\0';
    return 0;
}

/* 우편함에 값 한 벌을 넣고 워커를 깨운다. 같은 칸에 값이 남아 있었으면
 * 덮어쓴다(latest-wins). */
static void post(int idx, uint16_t start, uint16_t count, const uint16_t *regs)
{
    pthread_mutex_lock(&lock);
    slots[idx].start = start;
    slots[idx].count = count;
    memcpy(slots[idx].regs, regs, (size_t)count * sizeof(regs[0]));
    slots[idx].pending = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

/* 아래 둘은 lock을 쥔 채로 부를 것. */

static int any_pending(void)
{
    int i;

    for (i = 0; i < SLOT_COUNT; i++)
        if (slots[i].pending)
            return 1;
    return 0;
}

/* 밀린 칸 하나를 꺼내 out에 복사하고 비운다. 반환값은 그 칸의 번호,
 * 밀린 것이 없으면 -1. cursor를 옮겨 다음엔 그 다음 칸부터 본다 -
 * 한 칸이 계속 갱신돼도 다른 칸이 굶지 않는다. */
static int take_pending(slot_t *out)
{
    int i;

    for (i = 0; i < SLOT_COUNT; i++) {
        int idx = (cursor + i) % SLOT_COUNT;

        if (slots[idx].pending) {
            *out = slots[idx];
            slots[idx].pending = 0;
            cursor = (idx + 1) % SLOT_COUNT;
            return idx;
        }
    }
    return -1;
}

/* 워커: 우편함에 값이 들어오면 Modbus로 내보낸다. */
static void *worker_main(void *arg)
{
    (void)arg;

    for (;;) {
        slot_t job;
        int idx;
        mb_status_t st;
        uint8_t exc = 0;

        pthread_mutex_lock(&lock);
        while (!any_pending() && !worker_stop)
            pthread_cond_wait(&wake, &lock);
        if (worker_stop) {
            pthread_mutex_unlock(&lock);
            break;
        }
        idx = take_pending(&job);
        pthread_mutex_unlock(&lock);

        if (idx < 0)
            continue;   /* any_pending()과의 사이에 비었을 리 없지만 방어적으로 */

        if (job.count == 1)
            st = modbus_write_single(ctx, job.start, job.regs[0], &exc);
        else
            st = modbus_write_multiple(ctx, job.start, job.count, job.regs, &exc);

        if (st == MB_OK) {
            if (fail_streak >= FAIL_LOG_LIMIT)
                printf("matrix: modbus 복구됨 (%d건 실패 후)\n", fail_streak);
            fail_streak = 0;
            continue;
        }

        fail_streak++;
        if (fail_streak <= FAIL_LOG_LIMIT) {
            if (st == MB_ERR_EXCEPTION)
                fprintf(stderr, "matrix: [%s] modbus 예외 0x%02X %s\n",
                        slot_name(idx), exc, modbus_exc_str(exc));
            else
                fprintf(stderr, "matrix: [%s] modbus 실패: %s\n",
                        slot_name(idx), modbus_strerror(st));
            if (fail_streak == FAIL_LOG_LIMIT)
                fprintf(stderr, "matrix: 이후 같은 실패는 복구될 때까지 생략\n");
        }
    }

    return NULL;
}

guardx_err_t matrix_link_init(void)
{
    const char *dev = getenv("GUARDX_MATRIX_DEV");
    modbus_cfg_t cfg;

    if (dev != NULL && strcmp(dev, "off") == 0) {
        printf("matrix: GUARDX_MATRIX_DEV=off - LED 매트릭스 송출 비활성\n");
        return GUARDX_OK;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.device     = (dev != NULL && dev[0] != '\0') ? dev
                                                     : modbus_default_device();
    cfg.baud       = MATRIX_BAUD;
    cfg.slave_id   = GX_MODBUS_SLAVE_ID;
    cfg.timeout_ms = MATRIX_TIMEOUT_MS;
    cfg.retries    = MATRIX_RETRIES;
    cfg.verbose    = 0;

    ctx = modbus_open(&cfg);
    if (ctx == NULL) {
        fprintf(stderr, "matrix: 시리얼 열기 실패 (%s) - 송출 비활성\n",
                cfg.device);
        return GUARDX_ERR_OPEN;
    }

    if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
        fprintf(stderr, "matrix: 워커 생성 실패 - 송출 비활성\n");
        modbus_close(ctx);
        ctx = NULL;
        return GUARDX_ERR_OPEN;
    }
    worker_alive = 1;

    printf("matrix: LED 매트릭스 송출 준비됨 (%s, slave %u)\n",
           cfg.device, cfg.slave_id);
    return GUARDX_OK;
}

void matrix_link_handle_track(const char *payload, int len)
{
    char buf[MATRIX_JSON_MAX];
    static const char *const COORD_KEY[4] = { "ax", "ay", "bx", "by" };
    uint16_t regs[TRACK_REG_COUNT];
    long status = 0;
    int i;

    if (!worker_alive)
        return;   /* 비활성 - 구독은 살아 있지만 버린다 */

    if (copy_payload(payload, len, buf, sizeof(buf)) != 0)
        return;

    if (jint(buf, "status", &status) != 0 || status < 0 ||
        status > GX_INTRUDER_BOTH_VALID) {
        fprintf(stderr, "matrix: status 없음/범위 초과 - 버림: %s\n", buf);
        return;
    }
    regs[0] = (uint16_t)status;

    /* status가 0이어도 좌표 네 개는 함께 검증한다. STM32가 FC10을 받을 때
     * 다섯 값을 모두 범위 검사하므로, 한 개라도 어긋나면 프레임 전체가
     * 예외로 거절된다 - 여기서 미리 걸러야 원인이 보인다. */
    for (i = 0; i < 4; i++) {
        long v = 0;
        if (jint(buf, COORD_KEY[i], &v) != 0 || v < 0 || v > GX_COORD_MAX) {
            fprintf(stderr, "matrix: %s 없음/범위(0~%d) 초과 - 버림: %s\n",
                    COORD_KEY[i], GX_COORD_MAX, buf);
            return;
        }
        regs[i + 1] = (uint16_t)v;
    }

    /* 지우기(status=0)만 단일 쓰기다 - STM32가 좌표 묶음의 중간 상태를
     * 막으려고 그 경우에만 FC06을 허용한다. */
    post(SLOT_TRACK, REG_INTRUDER_TRACK_STATUS,
         (status == 0) ? 1 : TRACK_REG_COUNT, regs);
}

void matrix_link_handle_fire(const char *payload, int len)
{
    char buf[MATRIX_JSON_MAX];
    uint16_t reg;
    long bitmap = 0;
    bool active;

    /* worker_alive 검사를 뒤로 미룬 것은 의도적이다. 이 메시지는 LED 표시용인
     * 동시에 RPi C 가 화재 상태를 아는 유일한 경로라, RS-485 가 죽었다고 여기서
     * 버리면 사이렌까지 같이 죽는다. 표시는 못 해도 소리는 나야 한다. */
    if (copy_payload(payload, len, buf, sizeof(buf)) != 0)
        return;

    if (jint(buf, "zone_bitmap", &bitmap) != 0 ||
        bitmap < 0 || bitmap > (long)GX_FIRE_ZONE_BITMAP_MAX) {
        fprintf(stderr, "matrix: zone_bitmap 없음/범위(0~%u) 초과 - 버림: %s\n",
                GX_FIRE_ZONE_BITMAP_MAX, buf);
        return;
    }

    /* 한 구역이라도 켜져 있으면 화재. 같은 값이 반복 도착해도 조율기가
     * 전이만 골라 처리하므로 그냥 매번 알린다. */
    active = bitmap != 0;
    if (active && !fire_response_active) {
        guardx_err_t ret = actuator_shutter_close();

        if (ret != GUARDX_OK)
            fprintf(stderr, "matrix: fire shutter CLOSE failed (%d)\n", ret);
    }
    fire_response_active = active;

    audio_arbiter_set_fire(active);
    /* 팬도 같은 신호를 받는다 - 화재 중에는 AUTO 든 수동이든 0% 다.
     * 이 판단이 브로커 건너편에 있으면 링크가 끊긴 화재에서 팬이 계속
     * 돈다(shared/fan_protocol.h). */
    fan_control_set_fire(active);

    if (!worker_alive)
        return;

    reg = (uint16_t)bitmap;
    post(SLOT_FIRE, REG_FIRE_ZONE_BITMAP, 1, &reg);
}

void matrix_link_handle_zones(const char *payload, int len)
{
    char buf[MATRIX_JSON_MAX];
    uint16_t regs[ZONE_REG_COUNT];
    long zone = 0, temp_x10 = 0, humidity = 0;

    if (!worker_alive)
        return;

    if (copy_payload(payload, len, buf, sizeof(buf)) != 0)
        return;

    /* 대상 zone은 토픽이 아니라 payload가 정한다(matrix_link.h 참조). */
    if (jint(buf, "zone_id", &zone) != 0 || zone < 1 || zone > GX_ZONE_COUNT) {
        fprintf(stderr, "matrix: zone_id 없음/범위(1~%d) 초과 - 버림: %s\n",
                GX_ZONE_COUNT, buf);
        return;
    }

    /* 온도와 습도는 FC10 한 프레임으로 나가므로 하나라도 범위를 벗어나면
     * 둘 다 안 써진다. 여기서 미리 걸러야 원인이 보인다. */
    if (jint(buf, "temp_x10", &temp_x10) != 0 ||
        temp_x10 < 0 || temp_x10 > GX_ZONE_TEMP_X10_MAX) {
        fprintf(stderr, "matrix: temp_x10 없음/범위(0~%d) 초과 - 버림: %s\n",
                GX_ZONE_TEMP_X10_MAX, buf);
        return;
    }
    if (jint(buf, "humidity", &humidity) != 0 ||
        humidity < 0 || humidity > GX_ZONE_HUMIDITY_MAX) {
        fprintf(stderr, "matrix: humidity 없음/범위(0~%d) 초과 - 버림: %s\n",
                GX_ZONE_HUMIDITY_MAX, buf);
        return;
    }

    regs[0] = (uint16_t)temp_x10;
    regs[1] = (uint16_t)humidity;

    post(SLOT_ZONE_BASE + (int)(zone - 1),
         (uint16_t)(REG_ZONE1_TEMP_X10 + (zone - 1) * ZONE_REG_COUNT),
         ZONE_REG_COUNT, regs);
}

void matrix_link_cleanup(void)
{
    if (!worker_alive)
        return;

    pthread_mutex_lock(&lock);
    worker_stop = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    worker_alive = 0;

    /* 프로세스가 내려가도 STM32는 10초 뒤 스스로 점을 지우지만, 그 10초
     * 동안 "죽은 VMS가 가리키던 자리"가 화면에 남는다. 명시적으로 지운다.
     *
     * 화재 표시(120)와 온습도(100~107)는 일부러 건드리지 않는다. 저쪽은
     * 현장의 상태이지 이 프로세스의 세션이 아니라서, 데몬을 재시작했다는
     * 이유로 진행 중인 화재 표시를 끄면 안 된다. */
    (void)modbus_write_single(ctx, REG_INTRUDER_TRACK_STATUS, 0, NULL);

    modbus_close(ctx);
    ctx = NULL;
}
