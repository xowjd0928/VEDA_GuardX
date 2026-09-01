/*
 * modbus_slave.h - GuardX STM32 Modbus RTU Server (Slave)
 *
 * "GuardX STM32 <-> Raspberry Pi Modbus RTU 설계 기준서" (v0.2 Prototype)를
 * 그대로 구현한 슬레이브. STM32F401RE 가 Server(Slave ID 1), Raspberry Pi 가
 * Client(Master) 다.
 *
 *   물리계층 : USART6 · 115200 bps · 8N1 · Flow control 없음 (PC6=TX, PA12=RX)
 *   프레임끝 : USART IDLE 이벤트(HAL_UARTEx_ReceiveToIdle_DMA) 로 판정
 *   기능코드 : 0x03 Read / 0x06 Write Single / 0x10 Write Multiple
 *   예외코드 : 0x01 Illegal Function / 0x02 Illegal Data Address /
 *              0x03 Illegal Data Value
 *
 * 수신 프레임은 main.c의 UART 콜백에서 RTOS 큐로 복사되고 freertos.c의
 * Modbus 태스크가 Modbus_Slave_OnFrame()을 호출한다. 이 파일은 CRC 검증 →
 * 주소/기능/값 검증 → 응답(또는 예외) 송신까지 처리한다.
 *
 * !!! 이 레지스터 맵은 설계 엑셀 "레지스터 맵" 시트를 그대로 옮긴 것이며,
 *     rpic_app/modbus/include/guardx_modbus_regs.h 와 반드시 동기화할 것 !!!
 */
#ifndef __MODBUS_SLAVE_H
#define __MODBUS_SLAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 프로토콜 상수
 * ------------------------------------------------------------------------- */
#define MODBUS_SLAVE_ID          1u      /* 배선·설정 시트: STM32 Slave ID = 1  */
#define MODBUS_BROADCAST_ID      0u      /* Slave ID 0 = Broadcast(응답 없음)   */

/* 지원 기능 코드 */
#define MODBUS_FC_READ_HOLDING   0x03u   /* Read Holding Registers   (1~125)   */
#define MODBUS_FC_WRITE_SINGLE   0x06u   /* Write Single Register    (1)       */
#define MODBUS_FC_WRITE_MULTIPLE 0x10u   /* Write Multiple Registers (1~123)   */
#define MODBUS_EXCEPTION_FLAG    0x80u   /* 예외 응답 Function = 요청FC | 0x80 */

/* 예외 코드 */
#define MODBUS_EX_ILLEGAL_FUNCTION 0x01u /* 지원하지 않는 기능 코드            */
#define MODBUS_EX_ILLEGAL_ADDRESS  0x02u /* 정의되지 않은 주소 또는 RO 쓰기    */
#define MODBUS_EX_ILLEGAL_VALUE    0x03u /* 수량/길이/값 범위 오류             */

/* 0x03/0x10 한 요청당 수량 제한(표준 Modbus RTU) */
#define MODBUS_MAX_READ_QTY      125u
#define MODBUS_MAX_WRITE_QTY     123u

/* ---------------------------------------------------------------------------
 * Holding Register PDU 주소 (설계 엑셀 "레지스터 맵" 시트)
 *   표시 주소(4xxxx) = PDU + 40001. 코드는 PDU 주소만 쓴다.
 * ------------------------------------------------------------------------- */
enum {
  /* --- 제어 (RW, 구현) --- */
  REG_LED_COMMAND         = 0,    /* 40001 0=OFF 1=ON   PA4 LED 즉시 제어       */
  REG_PANEL_BRIGHTNESS    = 1,    /* 40002 0~255 level  (matrix 연결 후 반영)   */
  REG_PANEL_REFRESH_LEVEL = 2,    /* 40003 1=최고~4=최저 (matrix 연결 후 반영)  */
  /* --- 상태 (RO, 구현) --- */
  REG_DEVICE_STATUS       = 3,    /* 40004 bit0=UART bit1=HUB75                 */
  REG_UPTIME_SECONDS      = 4,    /* 40005 s, 65535 이후 순환                   */
  /* --- 진단 (RO, 구현) --- */
  REG_RX_FRAME_COUNT      = 5,    /* 40006 정상 CRC + 대상 주소 프레임 수       */
  REG_CRC_ERROR_COUNT     = 6,    /* 40007 CRC 불일치 프레임 수                 */
  REG_EXCEPTION_COUNT     = 7,    /* 40008 예외 응답 생성 횟수                  */
  /* --- 센서 (RW, 향후 예약) 0xFFFF/0x00FF = 데이터 없음 --- */
  REG_ZONE1_TEMP_X10      = 100,  /* 40101 0.1 ℃                               */
  REG_ZONE1_HUMIDITY      = 101,  /* 40102 %RH                                 */
  REG_ZONE2_TEMP_X10      = 102,  /* 40103 0.1 ℃                               */
  REG_ZONE2_HUMIDITY      = 103,  /* 40104 %RH                                 */
  REG_ZONE3_TEMP_X10      = 104,  /* 40105 0.1 ℃                               */
  REG_ZONE3_HUMIDITY      = 105,  /* 40106 %RH                                 */
  REG_ZONE4_TEMP_X10      = 106,  /* 40107 0.1 ℃                               */
  REG_ZONE4_HUMIDITY      = 107,  /* 40108 %RH                                 */
  /* --- 경보/침입 추적 (RW) --- */
  REG_FIRE_ZONE_BITMAP       = 120, /* 40121 bit0~3 = 화재 Zone1~4              */
  REG_INTRUDER_TRACK_STATUS  = 121, /* 40122 bit0=현재점 A, bit1=방향점 B 유효  */
  REG_INTRUDER_CURRENT_X     = 122, /* 40123 현재점 A X, 0~1000                 */
  REG_INTRUDER_CURRENT_Y     = 123, /* 40124 현재점 A Y, 0~1000                 */
  REG_INTRUDER_DIRECTION_X   = 124, /* 40125 방향점 B X, 0~1000                 */
  REG_INTRUDER_DIRECTION_Y   = 125, /* 40126 방향점 B Y, 0~1000                 */
  /* --- 화면 (RW, 향후 예약) --- */
  REG_SCREEN_SELECT       = 130,  /* 40131 0=자동, 1~3=고정                     */
  /* --- 시스템 (RO, 향후 예약) --- */
  REG_PROTOCOL_VERSION    = 200   /* 40201 major.minor, 0x0002 = v0.2          */
};

/* DEVICE_STATUS 비트. 프로토타입은 고정 0x0003(UART+HUB75 정상 가정).
 * matrix(HUB75) 연결 후 bit1 을 실제 초기화 결과로 바꾼다. */
#define MODBUS_STATUS_UART_OK    (1u << 0)
#define MODBUS_STATUS_HUB75_OK   (1u << 1)
#define MODBUS_DEVICE_STATUS_INIT (MODBUS_STATUS_UART_OK | MODBUS_STATUS_HUB75_OK)

/* PROTOCOL_VERSION 값 (major<<8 | minor) = 0x0002 = v0.2 */
#define MODBUS_PROTOCOL_VERSION  0x0002u

/* 침입자 좌표 규약. 원점은 평면도 좌상단, X는 오른쪽, Y는 아래쪽이다. */
#define MODBUS_COORD_MAX                   1000u
#define MODBUS_INTRUDER_CURRENT_VALID      (1u << 0)
#define MODBUS_INTRUDER_DIRECTION_VALID    (1u << 1)
#define MODBUS_INTRUDER_BOTH_VALID         (MODBUS_INTRUDER_CURRENT_VALID | \
                                             MODBUS_INTRUDER_DIRECTION_VALID)
#define MODBUS_INTRUDER_TIMEOUT_MS         10000u

typedef struct
{
  uint16_t status;
  uint16_t current_x;
  uint16_t current_y;
  uint16_t direction_x;
  uint16_t direction_y;
} Modbus_IntruderTrack;

/* ---------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* 레지스터를 초기값으로 리셋하고 진단 카운터를 0 으로 둔다. */
void Modbus_Slave_Init(void);

/* 수신한 RTU 프레임 한 개를 처리한다(CRC/주소/기능/값 검증 + 응답 송신).
 * freertos.c의 Modbus 태스크 문맥에서 호출하며 응답은
 * HAL_UART_Transmit(블로킹)으로 송신한다. */
void Modbus_Slave_OnFrame(const uint8_t *frame, uint16_t len);

/* 메인 루프 주기 훅. 현재는 사용처가 없고(UPTIME 은 읽을 때 계산),
 * 향후 센서/이벤트 갱신 자리로 예약. */
void Modbus_Slave_Poll(void);

/* 앱 연동용 접근자. addr 이 정의된 레지스터면 값을 반환하고 *ok=1,
 * 아니면 0 을 반환하고 *ok=0 (ok 는 NULL 허용). */
uint16_t Modbus_Slave_GetRegister(uint16_t addr, int *ok);

/* 좌표 다섯 값을 한 스냅샷으로 복사한다. out==NULL이면 0, 성공하면 1. */
int Modbus_Slave_GetIntruderTrack(Modbus_IntruderTrack *out);

#ifdef __cplusplus
}
#endif

#endif /* __MODBUS_SLAVE_H */
