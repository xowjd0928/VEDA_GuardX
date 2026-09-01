/*
 * modbus_test.c - GuardX RPi C <-> STM32 Modbus RTU 통신 시험 CLI
 *
 * STM32(Modbus RTU Server, Slave ID 1)와 RS-485(/dev/ttyUSB0)로 통신하는지
 * 확인하는 유저스페이스 도구. LED 매트릭스는 아직 연결하지 않고, Modbus
 * 통신 자체(읽기/쓰기/예외/CRC)만 검증하는 것이 목적이다.
 *
 * 설계 근거: "GuardX STM32 <-> Raspberry Pi Modbus RTU 설계 기준서" v0.2.
 * selftest 는 그 문서 "시험 항목" 시트의 T01~T16 프레임을 그대로 쏘고
 * 응답을 바이트 단위로 대조한다.
 *
 * 배선(설계 "배선·설정" 시트):
 *   STM32 USART6_TX PC6  → RPi GPIO15 RXD (헤더 10번)
 *   STM32 USART6_RX PA12 ← RPi GPIO14 TXD (헤더 8번)
 *   GND 공통.  둘 다 3.3V. (RPi GPIO 는 5V 비관용)
 *   RPi: raspi-config 에서 serial login shell 끄고 serial hardware 켤 것.
 */
/* usleep()/getopt() 선언 보장(-std=c11 로 빌드해도 안전). */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "modbus_rtu.h"
#include "guardx_modbus_regs.h"

/* ---- 레지스터 라벨(설계 "레지스터 맵" 시트) ---- */
typedef struct { uint16_t addr; const char *name; const char *note; } reg_info_t;

static const reg_info_t REG_INFO[] = {
    { REG_LED_COMMAND,           "LED_COMMAND",           "0=OFF 1=ON (PA4)" },
    { REG_PANEL_BRIGHTNESS,      "PANEL_BRIGHTNESS",      "0~255"            },
    { REG_PANEL_REFRESH_LEVEL,   "PANEL_REFRESH_LEVEL",   "1(max)~4(min)"    },
    { REG_DEVICE_STATUS,         "DEVICE_STATUS",         "bit0=UART bit1=HUB75" },
    { REG_UPTIME_SECONDS,        "UPTIME_SECONDS",        "s (wrap)"         },
    { REG_RX_FRAME_COUNT,        "RX_FRAME_COUNT",        "frames"           },
    { REG_CRC_ERROR_COUNT,       "CRC_ERROR_COUNT",       "frames"           },
    { REG_EXCEPTION_COUNT,       "EXCEPTION_COUNT",       "frames"           },
    { REG_ZONE1_TEMP_X10,        "ZONE1_TEMP_X10",        "0.1C 0xFFFF=n/a"  },
    { REG_ZONE1_HUMIDITY,        "ZONE1_HUMIDITY",        "%RH 0x00FF=n/a"   },
    { REG_ZONE2_TEMP_X10,        "ZONE2_TEMP_X10",        "0.1C"             },
    { REG_ZONE2_HUMIDITY,        "ZONE2_HUMIDITY",        "%RH"              },
    { REG_ZONE3_TEMP_X10,        "ZONE3_TEMP_X10",        "0.1C"             },
    { REG_ZONE3_HUMIDITY,        "ZONE3_HUMIDITY",        "%RH"              },
    { REG_ZONE4_TEMP_X10,        "ZONE4_TEMP_X10",        "0.1C"             },
    { REG_ZONE4_HUMIDITY,        "ZONE4_HUMIDITY",        "%RH"              },
    { REG_FIRE_ZONE_BITMAP,      "FIRE_ZONE_BITMAP",      "bit0~3=Zone1~4"   },
    { REG_INTRUDER_TRACK_STATUS, "INTRUDER_TRACK_STATUS", "bit0=A bit1=B"    },
    { REG_INTRUDER_CURRENT_X,    "INTRUDER_CURRENT_X",    "A.x 0~1000"       },
    { REG_INTRUDER_CURRENT_Y,    "INTRUDER_CURRENT_Y",    "A.y 0~1000"       },
    { REG_INTRUDER_DIRECTION_X,  "INTRUDER_DIRECTION_X",  "B.x 0~1000"       },
    { REG_INTRUDER_DIRECTION_Y,  "INTRUDER_DIRECTION_Y",  "B.y 0~1000"       },
    { REG_SCREEN_SELECT,         "SCREEN_SELECT",         "0=auto 1~3"       },
    { REG_PROTOCOL_VERSION,      "PROTOCOL_VERSION",      "0x0002=v0.2"      },
};
#define REG_INFO_COUNT (sizeof(REG_INFO) / sizeof(REG_INFO[0]))

static const char *reg_name(uint16_t addr)
{
    for (size_t i = 0; i < REG_INFO_COUNT; i++)
        if (REG_INFO[i].addr == addr)
            return REG_INFO[i].name;
    return "(reserved)";
}

/* ---- 인자 파서 ---- */
static int parse_u16(const char *s, uint16_t *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 0);   /* 0 → 10/16진수 자동(0x..) */
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 0xFFFF)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

/* ---- 결과 출력 ---- */
static void print_read(uint16_t addr, uint16_t count, const uint16_t *v)
{
    for (uint16_t i = 0; i < count; i++) {
        uint16_t a = (uint16_t)(addr + i);
        printf("  [%3u] %-22s = %5u  (0x%04X)\n", a, reg_name(a), v[i], v[i]);
    }
}

static void print_exc(const char *op, uint8_t exc)
{
    fprintf(stderr, "%s: 예외 0x%02X %s\n", op, exc, modbus_exc_str(exc));
}

/* ---------------------------------------------------------------------------
 * 명령들
 * ------------------------------------------------------------------------- */

static int cmd_read(modbus_ctx_t *ctx, uint16_t addr, uint16_t count)
{
    uint16_t v[125];
    uint8_t exc = 0;
    mb_status_t st = modbus_read_holding(ctx, addr, count, v, &exc);
    if (st == MB_ERR_EXCEPTION) { print_exc("read", exc); return 2; }
    if (st != MB_OK) { fprintf(stderr, "read: %s\n", modbus_strerror(st)); return 1; }
    print_read(addr, count, v);
    return 0;
}

static int cmd_write(modbus_ctx_t *ctx, uint16_t addr, uint16_t value)
{
    uint8_t exc = 0;
    mb_status_t st = modbus_write_single(ctx, addr, value, &exc);
    if (st == MB_ERR_EXCEPTION) { print_exc("write", exc); return 2; }
    if (st != MB_OK) { fprintf(stderr, "write: %s\n", modbus_strerror(st)); return 1; }
    printf("write ok: [%u] %s <- %u (0x%04X)\n", addr, reg_name(addr), value, value);
    return 0;
}

static int cmd_wmulti(modbus_ctx_t *ctx, uint16_t addr, const uint16_t *vals, uint16_t n)
{
    uint8_t exc = 0;
    mb_status_t st = modbus_write_multiple(ctx, addr, n, vals, &exc);
    if (st == MB_ERR_EXCEPTION) { print_exc("wmulti", exc); return 2; }
    if (st != MB_OK) { fprintf(stderr, "wmulti: %s\n", modbus_strerror(st)); return 1; }
    printf("wmulti ok: %u regs from [%u]\n", n, addr);
    return 0;
}

static int cmd_target(modbus_ctx_t *ctx, int argc, char **argv)
{
    if (argc == 1 && strcmp(argv[0], "off") == 0) {
        int rc = cmd_write(ctx, REG_INTRUDER_TRACK_STATUS, 0);
        if (rc == 0)
            printf("target: OFF (점 제거)\n");
        return rc;
    }

    if (argc != 4) {
        fprintf(stderr, "사용법: target <x1> <y1> <x2> <y2> | target off\n");
        return 2;
    }

    uint16_t vals[5] = { GX_INTRUDER_BOTH_VALID, 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        if (parse_u16(argv[i], &vals[i + 1]) || vals[i + 1] > GX_COORD_MAX) {
            fprintf(stderr, "target: 좌표는 0~%d 범위여야 합니다.\n", GX_COORD_MAX);
            return 2;
        }
    }

    int rc = cmd_wmulti(ctx, REG_INTRUDER_TRACK_STATUS, vals, 5);
    if (rc == 0) {
        printf("target: A(%u,%u) B(%u,%u), 10초 유효\n",
               vals[1], vals[2], vals[3], vals[4]);
    }
    return rc;
}

static int cmd_zones(modbus_ctx_t *ctx, int argc, char **argv)
{
    uint16_t vals[8];

    if (argc != 2 && argc != 8) {
        fprintf(stderr, "사용법: zones <t1x10> <h1>\n"
                        "        zones <t1x10> <h1> <t2x10> <h2> <t3x10> <h3> <t4x10> <h4>\n");
        return 2;
    }

    for (int i = 0; i < argc; i++) {
        if (parse_u16(argv[i], &vals[i]) ||
            ((i % 2) == 0 && vals[i] > 65534) ||
            ((i % 2) == 1 && vals[i] > 100)) {
            fprintf(stderr, "zones: 온도는 섭씨x10(0~65534), 습도는 0~100입니다.\n");
            return 2;
        }
    }

    int rc = cmd_wmulti(ctx, REG_ZONE1_TEMP_X10, vals, (uint16_t)argc);
    if (rc == 0) {
        if (argc == 2) {
            printf("zones: Z1=%u.%uC/%u%% 갱신 (Z2~Z4 유지)\n",
                   vals[0] / 10, vals[0] % 10, vals[1]);
        } else {
            printf("zones: Z1=%u.%uC/%u%% Z2=%u.%uC/%u%% Z3=%u.%uC/%u%% Z4=%u.%uC/%u%%\n",
                   vals[0] / 10, vals[0] % 10, vals[1], vals[2] / 10, vals[2] % 10, vals[3],
                   vals[4] / 10, vals[4] % 10, vals[5], vals[6] / 10, vals[6] % 10, vals[7]);
        }
    }
    return rc;
}

static int cmd_fire(modbus_ctx_t *ctx, int argc, char **argv)
{
    uint16_t value;
    uint16_t zone = 0;

    if (argc != 1) {
        fprintf(stderr, "사용법: fire <1-4|off>\n");
        return 2;
    }
    if (strcmp(argv[0], "off") == 0) {
        value = 0;
    } else {
        if (parse_u16(argv[0], &zone) || zone < 1 || zone > 4) {
            fprintf(stderr, "사용법: fire <1-4|off>\n");
            return 2;
        }
        value = (uint16_t)(1u << (zone - 1u));
    }

    int rc = cmd_write(ctx, REG_FIRE_ZONE_BITMAP, value);
    if (rc == 0) {
        if (value)
            printf("fire: Zone %u 화재 표시\n", (unsigned)zone);
        else
            printf("fire: 화재 표시 해제\n");
    }
    return rc;
}

static int cmd_status(modbus_ctx_t *ctx)
{
    uint16_t v[8];
    uint8_t exc = 0;
    mb_status_t st = modbus_read_holding(ctx, REG_LED_COMMAND, 8, v, &exc);
    if (st != MB_OK) {
        fprintf(stderr, "status: %s\n", modbus_strerror(st));
        return 1;
    }
    printf("STM32 Modbus 상태 (Slave 1)\n");
    printf("  LED_COMMAND         : %s\n", v[0] ? "ON" : "OFF");
    printf("  PANEL_BRIGHTNESS    : %u\n", v[1]);
    printf("  PANEL_REFRESH_LEVEL : %u\n", v[2]);
    printf("  DEVICE_STATUS       : 0x%04X  [UART=%d HUB75=%d]\n",
           v[3], (v[3] >> 0) & 1, (v[3] >> 1) & 1);
    printf("  UPTIME_SECONDS      : %u s\n", v[4]);
    printf("  RX_FRAME_COUNT      : %u\n", v[5]);
    printf("  CRC_ERROR_COUNT     : %u\n", v[6]);
    printf("  EXCEPTION_COUNT     : %u\n", v[7]);
    return 0;
}

static int cmd_dump(modbus_ctx_t *ctx)
{
    /* 정의된 주소는 불연속이라 블록 단위로 읽는다. {시작, 개수} */
    static const struct { uint16_t addr; uint16_t count; } blocks[] = {
        { 0,   8 }, { 100, 8 }, { 120, 6 }, { 130, 1 }, { 200, 1 },
    };
    int rc = 0;
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        uint16_t v[8];
        uint8_t exc = 0;
        mb_status_t st = modbus_read_holding(ctx, blocks[b].addr, blocks[b].count, v, &exc);
        if (st != MB_OK) {
            fprintf(stderr, "dump[%u..]: %s\n", blocks[b].addr, modbus_strerror(st));
            rc = 1;
            continue;
        }
        for (uint16_t i = 0; i < blocks[b].count; i++) {
            uint16_t a = (uint16_t)(blocks[b].addr + i);
            printf("  [%3u] %-22s = %5u  (0x%04X)\n", a, reg_name(a), v[i], v[i]);
        }
    }
    return rc;
}

static volatile sig_atomic_t monitor_stop;

static void monitor_sigint(int signo)
{
    (void)signo;
    monitor_stop = 1;
}

static int cmd_monitor(modbus_ctx_t *ctx, int interval_ms)
{
    printf("monitor: %d ms 주기로 상태 폴링 (Ctrl+C 로 종료)\n", interval_ms);
    while (!monitor_stop) {
        uint16_t v[8];
        uint8_t exc = 0;
        mb_status_t st = modbus_read_holding(ctx, REG_LED_COMMAND, 8, v, &exc);
        if (st == MB_OK) {
            printf("\rLED=%s bright=%3u refr=%u up=%5us rx=%u crc=%u exc=%u   ",
                   v[0] ? "ON " : "OFF", v[1], v[2], v[4], v[5], v[6], v[7]);
            fflush(stdout);
        } else {
            printf("\r%-60s", modbus_strerror(st));
            fflush(stdout);
        }
        usleep((useconds_t)interval_ms * 1000);
    }
    printf("\n");
    return 0;
}

static int cmd_raw(modbus_ctx_t *ctx, int argc, char **argv)
{
    /* argv: hex 바이트들(CRC 제외). 도구가 CRC 를 붙여 전송한다. */
    uint8_t frame[256];
    int n = 0;
    for (int i = 0; i < argc && n < 254; i++) {
        char *end = NULL;
        long b = strtol(argv[i], &end, 16);
        if (end == argv[i] || *end != '\0' || b < 0 || b > 0xFF) {
            fprintf(stderr, "raw: 잘못된 hex 바이트 '%s'\n", argv[i]);
            return 1;
        }
        frame[n++] = (uint8_t)b;
    }
    if (n < 2) {
        fprintf(stderr, "raw: 최소 Slave+FC 2바이트 필요\n");
        return 1;
    }
    uint16_t crc = modbus_crc16(frame, (size_t)n);
    frame[n++] = (uint8_t)(crc & 0xFF);
    frame[n++] = (uint8_t)((crc >> 8) & 0xFF);

    uint8_t resp[256];
    int r = modbus_transceive_raw(ctx, frame, n, resp, sizeof(resp));
    if (r < 0) {
        fprintf(stderr, "raw: %s\n", modbus_strerror((mb_status_t)r));
        return 1;
    }
    if (r == 0) {
        printf("raw: 무응답\n");
        return 0;
    }
    printf("raw: 응답 %d bytes:", r);
    for (int i = 0; i < r; i++)
        printf(" %02X", resp[i]);
    printf("\n");
    return 0;
}

/* ---- selftest: 설계 "시험 항목" T01~T16 골든 벡터 ---- */
typedef struct {
    const char *id;
    const char *desc;
    const char *req;     /* CRC 포함 완성 프레임 */
    const char *exp;     /* 기대 응답(빈 문자열 = 무응답) */
} vector_t;

static const vector_t VECTORS[] = {
    { "T01", "기본 레지스터 읽기",   "01 03 00 00 00 03 05 CB", "01 03 06 00 00 00 FF 00 01 D0 85" },
    { "T02", "LED ON 단일 쓰기",     "01 06 00 00 00 01 48 0A", "01 06 00 00 00 01 48 0A" },
    { "T03", "LED OFF 단일 쓰기",    "01 06 00 00 00 00 89 CA", "01 06 00 00 00 00 89 CA" },
    { "T04", "밝기 128",             "01 06 00 01 00 80 D9 AA", "01 06 00 01 00 80 D9 AA" },
    { "T05", "주사율 2",             "01 06 00 02 00 02 A9 CB", "01 06 00 02 00 02 A9 CB" },
    { "T06", "다중 쓰기",            "01 10 00 00 00 03 06 00 01 00 80 00 02 5B 69", "01 10 00 00 00 03 80 08" },
    { "T07", "값 범위 오류",         "01 06 00 01 01 00 D9 9A", "01 86 03 02 61" },
    { "T08", "RO 쓰기 거부",         "01 06 00 03 00 01 B8 0A", "01 86 02 C3 A1" },
    { "T09", "미지원 기능 코드",     "01 04 00 00 00 01 31 CA", "01 84 01 82 C0" },
    { "T10", "CRC 오류 무응답",      "01 06 00 00 00 01 48 0B", "" },
    { "T11", "다른 Slave ID",        "02 03 00 00 00 01 84 39", "" },
    { "T12", "Broadcast 쓰기",       "00 06 00 00 00 01 49 DB", "" },
    { "T13", "좌표 묶음 쓰기",       "01 10 00 79 00 05 0A 00 03 00 C8 01 90 01 5E 01 C2 05 9E", "01 10 00 79 00 05 D1 D3" },
    { "T14", "좌표 묶음 읽기",       "01 03 00 79 00 05 54 10", "01 03 0A 00 03 00 C8 01 90 01 5E 01 C2 58 69" },
    { "T15", "좌표 단일 쓰기 거부",  "01 06 00 7A 00 01 69 D3", "01 86 03 02 61" },
    { "T16", "좌표 즉시 제거",       "01 06 00 79 00 00 58 13", "01 06 00 79 00 00 58 13" },
};
#define VECTOR_COUNT (sizeof(VECTORS) / sizeof(VECTORS[0]))

static int parse_hex_bytes(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    while (*s && n < cap) {
        while (*s == ' ') s++;
        if (!*s) break;
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            return -1;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static int cmd_selftest(modbus_ctx_t *ctx)
{
    int pass = 0, fail = 0;

    printf("=== GuardX Modbus RTU selftest (T01~T16, 설계 골든 벡터) ===\n");
    printf("전제: STM32 가 리셋 직후 기본값(LED off, 밝기 255, 주사율 1)이어야\n"
           "      T01 이 통과한다. selftest 끝에서 기본값으로 되돌려 재실행 대비.\n\n");

    for (size_t i = 0; i < VECTOR_COUNT; i++) {
        const vector_t *tc = &VECTORS[i];
        uint8_t req[64], exp[64], resp[256];
        int rl = parse_hex_bytes(tc->req, req, sizeof(req));
        int el = parse_hex_bytes(tc->exp, exp, sizeof(exp));
        if (rl < 0 || el < 0) {
            fprintf(stderr, "%s: 내부 벡터 파싱 오류\n", tc->id);
            fail++;
            continue;
        }

        int n = modbus_transceive_raw(ctx, req, rl, resp, sizeof(resp));
        if (n < 0) {
            fprintf(stderr, "%s: 전송 오류 %s\n", tc->id, modbus_strerror((mb_status_t)n));
            fail++;
            continue;
        }

        int ok = (n == el) && (memcmp(resp, exp, (size_t)el) == 0);
        if (ok) {
            pass++;
            printf("  OK  %-4s %-16s -> %s\n", tc->id, tc->desc,
                   el ? tc->exp : "(무응답)");
        } else {
            fail++;
            printf("  BAD %-4s %-16s\n", tc->id, tc->desc);
            printf("       기대: %s\n", el ? tc->exp : "(무응답)");
            printf("       실제:");
            if (n == 0) printf(" (무응답)");
            else for (int k = 0; k < n; k++) printf(" %02X", resp[k]);
            printf("\n");
        }
    }

    /* 재실행 대비: 기본값 복구(LED off, 밝기 255, 주사율 1). Slave 1 로. */
    printf("\n기본값 복구 중...\n");
    modbus_set_slave(ctx, GX_MODBUS_SLAVE_ID);
    (void)modbus_write_single(ctx, REG_PANEL_BRIGHTNESS, 255, NULL);
    (void)modbus_write_single(ctx, REG_PANEL_REFRESH_LEVEL, 1, NULL);
    (void)modbus_write_single(ctx, REG_LED_COMMAND, 0, NULL);
    (void)modbus_write_single(ctx, REG_INTRUDER_TRACK_STATUS, 0, NULL);

    printf("\n==== %zu개 중 %d 통과, %d 실패 ====\n", VECTOR_COUNT, pass, fail);
    return fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
"GuardX STM32 <-> RPi Modbus RTU 통신 시험 도구\n"
"\n"
"사용법: %s [옵션]              # 대화형 미니쉘\n"
"        %s [옵션] <명령> [인자] # 단발 실행\n"
"\n"
"옵션:\n"
"  -d <dev>   시리얼 장치 (기본 /dev/guardx-rs485, 없으면 /dev/ttyUSB0)\n"
"  -b <baud>  전송 속도  (기본 115200)\n"
"  -s <id>    Slave ID   (기본 1)\n"
"  -t <ms>    응답 타임아웃 (기본 200)\n"
"  -r <n>     재시도 횟수  (기본 2)\n"
"  -v         TX/RX 프레임 hex 출력\n"
"  -h         이 도움말\n"
"\n"
"미니쉘 명령 옵션:\n"
"  -v         해당 명령의 TX/RX 프레임만 hex 출력 (예: status -v)\n"
"\n"
"미니쉘 키 조작:\n"
"  Tab        명령어 자동완성 (후보가 여러 개면 목록 표시)\n"
"  Up/Down    이전/다음 명령 불러오기\n"
"  Left/Right 커서를 옮겨 중간 문자 수정\n"
"  Backspace/Delete  커서 앞/뒤 문자 삭제\n"
"  Ctrl+C     미니쉘 종료\n"
"\n"
"명령:\n"
"  read <addr> <count>        FC03 Holding Register 읽기\n"
"  write <addr> <value>       FC06 단일 레지스터 쓰기\n"
"  wmulti <addr> <v1> [v2..]  FC10 다중 레지스터 쓰기\n"
"  led <on|off>               LED_COMMAND(0) 쓰기\n"
"  brightness <0-255>         PANEL_BRIGHTNESS(1) 쓰기 (HUB75 즉시 반영)\n"
"  refresh <1-4>              PANEL_REFRESH_LEVEL(2) 쓰기 (HUB75 즉시 반영)\n"
"  screen <0-3>               SCREEN_SELECT(130) 쓰기\n"
"  zones <t1 h1>              Zone 1 온도x10/습도만 갱신\n"
"  zones <t1 h1 ... t4 h4>    Zone 1~4 온도x10/습도 전체 갱신\n"
"  fire <1-4|off>             해당 Zone 화재 표시/해제\n"
"  target <x1> <y1> <x2> <y2> 현재점 A와 방향점 B 갱신 (좌표 0~1000)\n"
"  target off                 두 점 즉시 제거\n"
"  status                     제어+상태+진단 블록(0~7) 읽어 요약\n"
"  dump                       정의된 모든 레지스터 읽어 라벨과 함께 출력\n"
"  monitor [ms]               상태를 주기 폴링(기본 500 ms)\n"
"  raw <hex..>                CRC 제외한 프레임 바이트 입력 → CRC 붙여 전송\n"
"  selftest                   설계 T01~T16 골든 벡터로 STM 검증\n"
"  debug [on|off|status]       TX/RX 패킷 계속 출력 (인자 없으면 전환)\n"
"  exit | quit                 미니쉘 종료\n"
"\n"
"LED 매트릭스에서 바로 확인하는 예:\n"
"  screen 1                         # 온습도 숫자 화면 고정\n"
"  zones 250 45                     # Z1만 25.0C / 45%%로 갱신\n"
"  zones 250 45 260 50 270 55 280 60  # Z1~Z4 온습도 표시\n"
"  screen 2                         # 평면도 화면 고정\n"
"  target 200 400 350 450           # 현재점 A와 방향점 B 표시\n"
"  target off                       # 두 점 지우기\n"
"  fire 3                           # Zone 3 화재 화면 표시\n"
"  fire off                         # 화재 표시 해제\n"
"  brightness 128                   # 패널 밝기\n"
"  refresh 2                        # 패널 주사율\n"
"  screen 0                         # 화면 자동 전환 복귀\n"
"\n"
"단발 실행 예: %s target 200 400 350 450\n",
        prog, prog, prog);
}

static int dispatch_command(modbus_ctx_t *ctx, const char *cmd, int rest, char **args)
{
    int rc = 0;

    if (strcmp(cmd, "read") == 0) {
        uint16_t addr, count;
        if (rest != 2 || parse_u16(args[0], &addr) || parse_u16(args[1], &count) ||
            count < 1 || count > 125) {
            fprintf(stderr, "사용법: read <addr> <count(1-125)>\n");
            rc = 2;
        } else {
            rc = cmd_read(ctx, addr, count);
        }
    } else if (strcmp(cmd, "write") == 0) {
        uint16_t addr, value;
        if (rest != 2 || parse_u16(args[0], &addr) || parse_u16(args[1], &value)) {
            fprintf(stderr, "사용법: write <addr> <value>\n");
            rc = 2;
        } else {
            rc = cmd_write(ctx, addr, value);
        }
    } else if (strcmp(cmd, "wmulti") == 0) {
        uint16_t addr;
        if (rest < 2 || rest > 124 || parse_u16(args[0], &addr)) {
            fprintf(stderr, "사용법: wmulti <addr> <v1> [v2..] (최대 123개)\n");
            rc = 2;
        } else {
            uint16_t vals[123];
            uint16_t n = 0;
            int bad = 0;
            for (int i = 1; i < rest; i++)
                if (parse_u16(args[i], &vals[n++])) { bad = 1; break; }
            if (bad) { fprintf(stderr, "wmulti: 값 파싱 오류\n"); rc = 2; }
            else rc = cmd_wmulti(ctx, addr, vals, n);
        }
    } else if (strcmp(cmd, "led") == 0) {
        if (rest != 1 || (strcmp(args[0], "on") && strcmp(args[0], "off"))) {
            fprintf(stderr, "사용법: led <on|off>\n");
            rc = 2;
        } else {
            rc = cmd_write(ctx, REG_LED_COMMAND, strcmp(args[0], "on") == 0 ? 1 : 0);
        }
    } else if (strcmp(cmd, "brightness") == 0) {
        uint16_t val;
        if (rest != 1 || parse_u16(args[0], &val) || val > 255) {
            fprintf(stderr, "사용법: brightness <0-255>\n");
            rc = 2;
        } else {
            rc = cmd_write(ctx, REG_PANEL_BRIGHTNESS, val);
        }
    } else if (strcmp(cmd, "refresh") == 0) {
        uint16_t val;
        if (rest != 1 || parse_u16(args[0], &val) || val < 1 || val > 4) {
            fprintf(stderr, "사용법: refresh <1-4>\n");
            rc = 2;
        } else {
            rc = cmd_write(ctx, REG_PANEL_REFRESH_LEVEL, val);
        }
    } else if (strcmp(cmd, "screen") == 0) {
        uint16_t val;
        if (rest != 1 || parse_u16(args[0], &val) || val > 3) {
            fprintf(stderr, "사용법: screen <0-3>\n");
            rc = 2;
        } else {
            rc = cmd_write(ctx, REG_SCREEN_SELECT, val);
        }
    } else if (strcmp(cmd, "zones") == 0) {
        rc = cmd_zones(ctx, rest, args);
    } else if (strcmp(cmd, "fire") == 0) {
        rc = cmd_fire(ctx, rest, args);
    } else if (strcmp(cmd, "target") == 0) {
        rc = cmd_target(ctx, rest, args);
    } else if (strcmp(cmd, "status") == 0) {
        rc = cmd_status(ctx);
    } else if (strcmp(cmd, "dump") == 0) {
        rc = cmd_dump(ctx);
    } else if (strcmp(cmd, "monitor") == 0) {
        int interval = 500;
        if (rest >= 1) interval = atoi(args[0]);
        if (interval < 20) interval = 20;
        rc = cmd_monitor(ctx, interval);
    } else if (strcmp(cmd, "raw") == 0) {
        rc = cmd_raw(ctx, rest, args);
    } else if (strcmp(cmd, "selftest") == 0) {
        rc = cmd_selftest(ctx);
    } else {
        fprintf(stderr, "알 수 없는 명령: %s (help로 명령 목록 확인)\n", cmd);
        rc = 2;
    }

    return rc;
}

#define SHELL_LINE_MAX  2048
#define SHELL_ARG_MAX   128
#define SHELL_HISTORY_MAX  50

static char shell_history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
static size_t shell_history_count;

static const char *const SHELL_COMMANDS[] = {
    "help", "read", "write", "wmulti", "led", "brightness", "refresh",
    "screen", "zones", "fire", "target", "status", "dump", "monitor",
    "raw", "selftest", "debug", "verbose", "exit", "quit"
};
#define SHELL_COMMAND_COUNT (sizeof(SHELL_COMMANDS) / sizeof(SHELL_COMMANDS[0]))

static void shell_redraw(const char *prompt, const char *line, size_t len, size_t cursor)
{
    printf("\r%s%s\x1b[K", prompt, line);
    if (cursor < len)
        printf("\x1b[%zuD", len - cursor);
    fflush(stdout);
}

static void shell_set_line(char *line, size_t cap, size_t *len, size_t *cursor,
                           const char *text, const char *prompt)
{
    size_t n = strlen(text);
    if (n >= cap)
        n = cap - 1;
    memcpy(line, text, n);
    line[n] = '\0';
    *len = n;
    *cursor = n;
    shell_redraw(prompt, line, *len, *cursor);
}

static int shell_replace_range(char *line, size_t cap, size_t *len, size_t *cursor,
                               size_t start, size_t end, const char *replacement)
{
    size_t repl_len = strlen(replacement);
    size_t new_len = *len - (end - start) + repl_len;
    if (new_len >= cap)
        return -1;

    memmove(line + start + repl_len, line + end, *len - end + 1);
    memcpy(line + start, replacement, repl_len);
    *len = new_len;
    *cursor = start + repl_len;
    return 0;
}

static void shell_complete(char *line, size_t cap, size_t *len, size_t *cursor,
                           const char *prompt)
{
    size_t token_end = 0;
    while (token_end < *len && line[token_end] != ' ' && line[token_end] != '\t')
        token_end++;

    /* 명령어 첫 단어의 끝에서만 자동완성한다. */
    if (*cursor != token_end) {
        fputc('\a', stdout);
        fflush(stdout);
        return;
    }

    const char *matches[SHELL_COMMAND_COUNT];
    size_t match_count = 0;
    for (size_t i = 0; i < SHELL_COMMAND_COUNT; i++) {
        if (strncmp(SHELL_COMMANDS[i], line, token_end) == 0)
            matches[match_count++] = SHELL_COMMANDS[i];
    }

    if (match_count == 0) {
        fputc('\a', stdout);
        fflush(stdout);
        return;
    }

    size_t common = strlen(matches[0]);
    for (size_t i = 1; i < match_count; i++) {
        size_t j = 0;
        while (j < common && matches[0][j] == matches[i][j])
            j++;
        common = j;
    }

    char replacement[SHELL_LINE_MAX];
    if (match_count == 1) {
        snprintf(replacement, sizeof(replacement), "%s%s", matches[0],
                 token_end == *len ? " " : "");
        if (shell_replace_range(line, cap, len, cursor, 0, token_end, replacement) != 0)
            fputc('\a', stdout);
        shell_redraw(prompt, line, *len, *cursor);
        return;
    }

    if (common > token_end) {
        memcpy(replacement, matches[0], common);
        replacement[common] = '\0';
        (void)shell_replace_range(line, cap, len, cursor, 0, token_end, replacement);
        shell_redraw(prompt, line, *len, *cursor);
        return;
    }

    printf("\r\n");
    for (size_t i = 0; i < match_count; i++)
        printf("%-12s", matches[i]);
    printf("\r\n");
    shell_redraw(prompt, line, *len, *cursor);
}

static void shell_add_history(const char *line)
{
    if (line[0] == '\0')
        return;
    if (shell_history_count > 0 &&
        strcmp(shell_history[shell_history_count - 1], line) == 0)
        return;

    if (shell_history_count == SHELL_HISTORY_MAX) {
        memmove(shell_history[0], shell_history[1],
                (SHELL_HISTORY_MAX - 1) * SHELL_LINE_MAX);
        shell_history_count--;
    }
    snprintf(shell_history[shell_history_count], SHELL_LINE_MAX, "%s", line);
    shell_history_count++;
}

/* 반환: 1=한 줄 입력, 0=Ctrl+C/Ctrl+D에 의한 종료, -1=오류. */
static int shell_readline(char *line, size_t cap, const char *prompt)
{
    struct termios saved;
    struct termios raw;
    size_t len = 0;
    size_t cursor = 0;
    size_t history_pos = shell_history_count;
    char scratch[SHELL_LINE_MAX] = "";

    if (tcgetattr(STDIN_FILENO, &saved) != 0)
        return -1;
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return -1;

    line[0] = '\0';
    printf("%s", prompt);
    fflush(stdout);

    for (;;) {
        unsigned char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
            return n == 0 ? 0 : -1;
        }

        if (ch == '\r' || ch == '\n') {
            printf("\r\n");
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
            return 1;
        }

        if (ch == 3) { /* Ctrl+C: 미니쉘 종료 */
            line[0] = '\0';
            printf("^C\r\n");
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
            return 0;
        }

        if (ch == '\t') {
            shell_complete(line, cap, &len, &cursor, prompt);
            continue;
        }

        if (ch == 127 || ch == 8) {
            if (cursor > 0) {
                memmove(line + cursor - 1, line + cursor, len - cursor + 1);
                cursor--;
                len--;
                shell_redraw(prompt, line, len, cursor);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 4) { /* Ctrl+D: 커서 문자 삭제, 빈 줄이면 EOF */
            if (len == 0) {
                printf("\r\n");
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
                return 0;
            }
            if (cursor < len) {
                memmove(line + cursor, line + cursor + 1, len - cursor);
                len--;
                shell_redraw(prompt, line, len, cursor);
            }
            continue;
        }

        if (ch == 1) { cursor = 0; shell_redraw(prompt, line, len, cursor); continue; }
        if (ch == 5) { cursor = len; shell_redraw(prompt, line, len, cursor); continue; }
        if (ch == 21) { len = cursor = 0; line[0] = '\0'; shell_redraw(prompt, line, len, cursor); continue; }

        if (ch == 27) {
            unsigned char seq1, seq2;
            if (read(STDIN_FILENO, &seq1, 1) != 1 || read(STDIN_FILENO, &seq2, 1) != 1)
                continue;
            if (seq1 != '[' && seq1 != 'O')
                continue;

            if (seq2 == 'A') { /* Up */
                if (history_pos == shell_history_count)
                    snprintf(scratch, sizeof(scratch), "%s", line);
                if (history_pos > 0) {
                    history_pos--;
                    shell_set_line(line, cap, &len, &cursor,
                                   shell_history[history_pos], prompt);
                } else {
                    fputc('\a', stdout); fflush(stdout);
                }
            } else if (seq2 == 'B') { /* Down */
                if (history_pos + 1 < shell_history_count) {
                    history_pos++;
                    shell_set_line(line, cap, &len, &cursor,
                                   shell_history[history_pos], prompt);
                } else if (history_pos < shell_history_count) {
                    history_pos = shell_history_count;
                    shell_set_line(line, cap, &len, &cursor, scratch, prompt);
                } else {
                    fputc('\a', stdout); fflush(stdout);
                }
            } else if (seq2 == 'C') { /* Right */
                if (cursor < len) { cursor++; shell_redraw(prompt, line, len, cursor); }
            } else if (seq2 == 'D') { /* Left */
                if (cursor > 0) { cursor--; shell_redraw(prompt, line, len, cursor); }
            } else if (seq2 == 'H') { /* Home */
                cursor = 0; shell_redraw(prompt, line, len, cursor);
            } else if (seq2 == 'F') { /* End */
                cursor = len; shell_redraw(prompt, line, len, cursor);
            } else if (seq1 == '[' && seq2 == '3') { /* Delete: ESC [ 3 ~ */
                unsigned char tilde;
                if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~' && cursor < len) {
                    memmove(line + cursor, line + cursor + 1, len - cursor);
                    len--;
                    shell_redraw(prompt, line, len, cursor);
                }
            }
            continue;
        }

        if (ch >= 32 && ch != 127) {
            if (len + 1 < cap) {
                memmove(line + cursor + 1, line + cursor, len - cursor + 1);
                line[cursor++] = (char)ch;
                len++;
                shell_redraw(prompt, line, len, cursor);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
        }
    }
}

static int run_shell(modbus_ctx_t *ctx, const modbus_cfg_t *cfg, const char *prog)
{
    char line[SHELL_LINE_MAX];
    int interactive = isatty(STDIN_FILENO);
    int shell_verbose = cfg->verbose ? 1 : 0;

    printf("GuardX Modbus shell - %s, %d bps, Slave %u\n",
           cfg->device, cfg->baud, cfg->slave_id);
    printf("명령 목록은 help, 종료는 exit 또는 quit\n");

    for (;;) {
        int input_ok;
        if (interactive)
            input_ok = shell_readline(line, sizeof(line), "modbus> ");
        else
            input_ok = fgets(line, sizeof(line), stdin) != NULL ? 1 : 0;

        if (input_ok <= 0) {
            if (interactive)
                printf("\n");
            break;
        }
        if (interactive)
            shell_add_history(line);

        char *shell_argv[SHELL_ARG_MAX];
        int shell_argc = 0;
        char *tok = strtok(line, " \t\r\n");
        while (tok != NULL && shell_argc < SHELL_ARG_MAX) {
            shell_argv[shell_argc++] = tok;
            tok = strtok(NULL, " \t\r\n");
        }
        if (shell_argc == 0)
            continue;
        if (tok != NULL) {
            fprintf(stderr, "인자가 너무 많습니다.\n");
            continue;
        }

        int command_verbose = shell_verbose;
        for (int i = 1; i < shell_argc; ) {
            if (strcmp(shell_argv[i], "-v") == 0) {
                command_verbose = 1;
                for (int j = i; j + 1 < shell_argc; j++)
                    shell_argv[j] = shell_argv[j + 1];
                shell_argc--;
            } else {
                i++;
            }
        }

        const char *cmd = shell_argv[0];
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0)
            break;
        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            usage(prog);
            continue;
        }
        if (strcmp(cmd, "debug") == 0 || strcmp(cmd, "verbose") == 0) {
            if (shell_argc == 1) {
                shell_verbose = !shell_verbose;
            } else if (shell_argc == 2 && strcmp(shell_argv[1], "on") == 0) {
                shell_verbose = 1;
            } else if (shell_argc == 2 && strcmp(shell_argv[1], "off") == 0) {
                shell_verbose = 0;
            } else if (!(shell_argc == 2 && strcmp(shell_argv[1], "status") == 0)) {
                fprintf(stderr, "사용법: debug [on|off|status]\n");
                continue;
            }
            modbus_set_verbose(ctx, shell_verbose);
            printf("packet debug: %s\n", shell_verbose ? "ON" : "OFF");
            continue;
        }

        monitor_stop = 0;
        void (*old_handler)(int) = signal(SIGINT, monitor_sigint);
        modbus_set_verbose(ctx, command_verbose);
        (void)dispatch_command(ctx, cmd, shell_argc - 1, shell_argv + 1);
        modbus_set_verbose(ctx, shell_verbose);
        signal(SIGINT, old_handler);
    }

    return 0;
}

int main(int argc, char **argv)
{
    modbus_cfg_t cfg = {
        .device = modbus_default_device(),
        .baud = 115200,
        .slave_id = GX_MODBUS_SLAVE_ID,
        .timeout_ms = 200,
        .retries = 2,
        .verbose = 0,
    };

    int opt;
    while ((opt = getopt(argc, argv, "d:b:s:t:r:vh")) != -1) {
        switch (opt) {
            case 'd': cfg.device = optarg; break;
            case 'b': cfg.baud = atoi(optarg); break;
            case 's': cfg.slave_id = (uint8_t)atoi(optarg); break;
            case 't': cfg.timeout_ms = atoi(optarg); break;
            case 'r': cfg.retries = atoi(optarg); break;
            case 'v': cfg.verbose = 1; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }
    modbus_ctx_t *ctx = modbus_open(&cfg);
    if (ctx == NULL) {
        fprintf(stderr, "시리얼 열기 실패: %s\n", cfg.device);
        /* 사유에 따라 볼 곳이 완전히 다르다. 예전엔 항상 "누가 쓰는 중"이라고
         * 안내해서, 장치가 아예 없을 때(변환기를 뽑았거나 ttyUSB 번호가 바뀐
         * 흔한 경우) 엉뚱하게 프로세스를 뒤지게 만들었다.
         *
         * errno 대신 access 로 다시 본다 - modbus_open 이 실패 로그를 찍는
         * 사이 errno 가 덮일 수 있어서, 여기서 직접 확인하는 편이 확실하다. */
        if (access(cfg.device, F_OK) != 0) {
            fprintf(stderr,
                    "  장치가 없습니다. USB 변환기가 빠졌거나 번호가 바뀐 것입니다.\n"
                    "        ls -l /dev/guardx-rs485 /dev/ttyUSB*\n"
                    "        dmesg | tail            # ch341 attach 줄 확인\n");
        } else if (access(cfg.device, R_OK | W_OK) != 0) {
            fprintf(stderr,
                    "  권한이 없습니다. dialout 그룹에 속해 있는지 보세요.\n"
                    "        sudo usermod -aG dialout $USER   # 재로그인 필요\n");
        } else {
            /* rpic_subscriber 가 LED 매트릭스 송출 때문에 같은 포트를 상시
             * 잡고 있다. 그걸 모르면 배선이나 raspi-config 를 뒤지게 된다. */
            fprintf(stderr,
                    "  힌트: rpic_subscriber 가 이 포트를 쓰는 중일 수 있습니다.\n"
                    "        sudo systemctl stop rpic_subscriber   # 확인 후 다시 start\n");
        }
        return 1;
    }

    int rc;
    if (optind >= argc) {
        rc = run_shell(ctx, &cfg, argv[0]);
    } else {
        const char *cmd = argv[optind++];
        rc = dispatch_command(ctx, cmd, argc - optind, argv + optind);
    }

    modbus_close(ctx);
    return rc;
}
