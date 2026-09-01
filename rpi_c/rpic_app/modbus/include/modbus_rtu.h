/*
 * modbus_rtu.h - GuardX Raspberry Pi C 측 Modbus RTU Client(Master) 라이브러리
 *
 * 설계 기준서 v0.2: Raspberry Pi = Client(Master), STM32 = Server(Slave ID 1).
 * 물리계층은 RS-485 반이중(RPi C는 USB 변환기 /dev/ttyUSB0), 115200 · 8N1 ·
 * flow control 없음. 3.3V TTL 점대점에서 옮겨왔지만 RTU 프레임/CRC 는 그대로다.
 *
 * DE/RE 방향전환 코드가 없는 것은 빠뜨린 것이 아니다 - 양 끝 변환기가 자동
 * 흐름제어형(TX를 감지해 스스로 전환)이라 소프트웨어가 관여하지 않는다.
 * 수동 모듈로 바꾸게 되면 write 전후에 그 처리를 끼워야 하고, STM32 쪽은
 * F401 USART에 하드웨어 DE가 없어 TC 인터럽트로 직접 토글해야 한다
 * (modbus/README.md 1절 "코드가 안 바뀌는 이유" 참조).
 *
 * 이 파일은 프로토콜 전용이라 GuardX 레지스터 맵을 알지 못한다. 레지스터
 * 이름/범위는 guardx_modbus_regs.h 와 상위 도구(modbus_test.c)가 다룬다.
 */
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>
#include <stddef.h>

/* 반환 코드. 0=성공, 음수=실패. (guardx_err_t 와 별개인 프로토콜 전용 코드) */
typedef enum {
    MB_OK          =  0,
    MB_ERR_PARAM   = -1,   /* 잘못된 인자                                  */
    MB_ERR_OPEN    = -2,   /* 시리얼 open/termios 실패                      */
    MB_ERR_IO      = -3,   /* read/write 실패                              */
    MB_ERR_TIMEOUT = -4,   /* 응답 타임아웃(무응답)                         */
    MB_ERR_CRC     = -5,   /* 응답 CRC 불일치                              */
    MB_ERR_FRAME   = -6,   /* 응답 길이/구조 오류, Slave/FC 불일치          */
    MB_ERR_EXCEPTION = -7  /* Slave 가 예외 응답 반환(exc_code 참조)        */
} mb_status_t;

/* 마스터 설정. device/baud/slave_id 는 필수, 나머지는 0 이면 기본값 사용. */
typedef struct {
    const char *device;      /* 예: "/dev/ttyUSB0"                          */
    int         baud;        /* 예: 115200                                  */
    uint8_t     slave_id;    /* 대상 Slave ID (기본 1)                      */
    int         timeout_ms;  /* 응답 대기 시간(기본 200 ms, 설계 초기값)    */
    int         retries;     /* 타임아웃/CRC 시 재시도 횟수(기본 2)         */
    int         verbose;     /* 1 이면 TX/RX 프레임 hex 를 stderr 로 출력   */
} modbus_cfg_t;

typedef struct modbus_ctx modbus_ctx_t;   /* 불투명 핸들 */

/* 기본 시리얼 장치 경로를 고른다.
 *
 * udev 별칭 /dev/guardx-rs485 가 있으면 그걸, 없으면 /dev/ttyUSB0 를 준다.
 * USB 변환기는 뽑았다 꽂을 때마다 ttyUSB 번호가 올라가서(0->1->0) 고정 경로를
 * 박아두면 "No such file or directory" 로 실패한다. 별칭 규칙은
 * udev/99-guardx-rs485.rules 에 있고 install.sh 가 깔아준다.
 *
 * 규칙이 없는 장비에서도 종전대로 동작하도록 폴백을 남긴다 - 그래서 별칭을
 * 기본값으로 "바꾸는" 것이 아니라 "있으면 우선"이다.
 *
 * 반환값은 정적 문자열이라 free 하지 않는다. */
const char *modbus_default_device(void);

/* 시리얼 포트를 열고 8N1/baud 로 설정한다. 실패 시 NULL. */
modbus_ctx_t *modbus_open(const modbus_cfg_t *cfg);
void          modbus_close(modbus_ctx_t *ctx);

/* 문맥의 대상 Slave ID 를 바꾼다(같은 포트로 여러 슬레이브 시험용). */
void          modbus_set_slave(modbus_ctx_t *ctx, uint8_t slave_id);

/* TX/RX 프레임 hex 출력을 실행 중에 켜거나 끈다. */
void          modbus_set_verbose(modbus_ctx_t *ctx, int enabled);

/* FC 0x03 Read Holding Registers. out 에 count 개(빅엔디안 해석된 uint16) 채움.
 * 예외 응답이면 MB_ERR_EXCEPTION 을 반환하고 exc_code(!=NULL)에 코드를 담는다. */
mb_status_t modbus_read_holding(modbus_ctx_t *ctx, uint16_t addr, uint16_t count,
                                uint16_t *out, uint8_t *exc_code);

/* FC 0x06 Write Single Register. slave_id==0(broadcast)이면 응답을 기다리지 않고
 * MB_OK 반환. */
mb_status_t modbus_write_single(modbus_ctx_t *ctx, uint16_t addr, uint16_t value,
                                uint8_t *exc_code);

/* FC 0x10 Write Multiple Registers. broadcast 처리는 write_single 과 동일. */
mb_status_t modbus_write_multiple(modbus_ctx_t *ctx, uint16_t addr, uint16_t count,
                                  const uint16_t *values, uint8_t *exc_code);

/* 저수준: 완성된 요청 프레임(CRC 포함)을 "있는 그대로" 전송하고 응답 원시
 * 바이트를 읽는다. 재시도/해석 없음. 반환값 >=0 = 수신 바이트 수(0=무응답),
 * <0 = mb_status_t. 시험 도구가 설계 기준서의 정확한 Hex 프레임(고의 CRC
 * 오류 T10 포함)을 그대로 쏘고 응답을 바이트 단위로 비교하는 데 쓴다. */
int modbus_transceive_raw(modbus_ctx_t *ctx, const uint8_t *req, int req_len,
                          uint8_t *resp, int resp_cap);

/* Modbus CRC-16 (poly 0xA001, init 0xFFFF). 반환값 하위바이트가 먼저 전송된다. */
uint16_t    modbus_crc16(const uint8_t *data, size_t len);

/* 사람이 읽는 문자열. */
const char *modbus_strerror(mb_status_t s);
const char *modbus_exc_str(uint8_t exc_code);

#endif /* MODBUS_RTU_H */
