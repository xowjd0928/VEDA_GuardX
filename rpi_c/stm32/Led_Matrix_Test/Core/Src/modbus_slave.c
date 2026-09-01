/*
 * modbus_slave.c - GuardX STM32 Modbus RTU Server (Slave) 구현
 *
 * 설계 근거: "GuardX STM32 <-> Raspberry Pi Modbus RTU 설계 기준서" v0.2.
 * 프레임 구조/시험 항목(T01~T16)의 Hex 예시와 바이트 단위로 일치한다.
 *
 * 처리 순서 (Modbus_Slave_OnFrame):
 *   1) 길이/CRC 검증   - CRC 불일치는 CRC_ERROR_COUNT 만 올리고 무응답(폐기)
 *   2) Slave ID 판정   - 우리(1)/Broadcast(0) 아니면 무시(무응답)
 *   3) 기능코드 분기   - 0x03/0x06/0x10, 그 외는 예외 0x01
 *   4) 응답 송신       - Broadcast 요청에는 응답하지 않는다
 *
 * FreeRTOS 통합 단계: PANEL_BRIGHTNESS/REFRESH 는 HUB75 드라이버에 즉시
 * 반영한다. LED_COMMAND 는 matrix가 아닌 PA4 단일 GPIO LED를 제어한다.
 */
#include "modbus_slave.h"
#include "main.h"        /* LED_Pin/LED_GPIO_Port, HAL_GetTick, HAL_GPIO_* */
#include "usart.h"       /* huart6 */
#include "Driver_RGBMatrix.h"
#include "FreeRTOS.h"
#include "task.h"

/* USART IDLE 이벤트 응답 송신 타임아웃(ms). 프레임이 작아 넉넉하다. */
#define MODBUS_TX_TIMEOUT_MS   100u

/* 요청/응답 최대 길이. 0x03 응답 = 5 + 2*125 = 255 bytes 가 최대. */
#define MODBUS_FRAME_MAX       256u

/* ---------------------------------------------------------------------------
 * 레지스터 뱅크
 *   설계 엑셀 "레지스터 맵" 시트를 그대로 옮긴 표. min/max 는 쓰기 유효 범위.
 *   init 이 max 를 넘는 항목(센서 0xFFFF/습도 0x00FF)은 "데이터 없음" 센티넬로,
 *   리셋값으로만 저장되고 클라이언트 쓰기는 min~max 로 검증된다.
 * ------------------------------------------------------------------------- */
typedef enum { MB_RO = 0, MB_RW = 1 } mb_access_t;

typedef struct {
  uint16_t     addr;      /* PDU 주소            */
  mb_access_t  access;    /* MB_RO / MB_RW       */
  uint16_t     min;       /* 쓰기 허용 최소      */
  uint16_t     max;       /* 쓰기 허용 최대      */
  uint16_t     init;      /* 리셋 초기값         */
  uint16_t     value;     /* 현재값(런타임)      */
} mb_reg_t;

static mb_reg_t s_regs[] = {
  /*  addr                       access  min  max     init          */
  { REG_LED_COMMAND,             MB_RW,   0u,   1u,   0u,        0u },
  { REG_PANEL_BRIGHTNESS,        MB_RW,   0u, 255u, 255u,        0u },
  { REG_PANEL_REFRESH_LEVEL,     MB_RW,   1u,   4u,   1u,        0u },
  { REG_DEVICE_STATUS,           MB_RO,   0u, 0xFFFFu, MODBUS_DEVICE_STATUS_INIT, 0u },
  { REG_UPTIME_SECONDS,          MB_RO,   0u, 0xFFFFu, 0u,       0u },
  { REG_RX_FRAME_COUNT,          MB_RO,   0u, 0xFFFFu, 0u,       0u },
  { REG_CRC_ERROR_COUNT,         MB_RO,   0u, 0xFFFFu, 0u,       0u },
  { REG_EXCEPTION_COUNT,         MB_RO,   0u, 0xFFFFu, 0u,       0u },
  { REG_ZONE1_TEMP_X10,          MB_RW,   0u, 65534u, 0xFFFFu,   0u },
  { REG_ZONE1_HUMIDITY,          MB_RW,   0u, 100u,   0x00FFu,   0u },
  { REG_ZONE2_TEMP_X10,          MB_RW,   0u, 65534u, 0xFFFFu,   0u },
  { REG_ZONE2_HUMIDITY,          MB_RW,   0u, 100u,   0x00FFu,   0u },
  { REG_ZONE3_TEMP_X10,          MB_RW,   0u, 65534u, 0xFFFFu,   0u },
  { REG_ZONE3_HUMIDITY,          MB_RW,   0u, 100u,   0x00FFu,   0u },
  { REG_ZONE4_TEMP_X10,          MB_RW,   0u, 65534u, 0xFFFFu,   0u },
  { REG_ZONE4_HUMIDITY,          MB_RW,   0u, 100u,   0x00FFu,   0u },
  { REG_FIRE_ZONE_BITMAP,        MB_RW,   0u,  15u,   0u,        0u },
  { REG_INTRUDER_TRACK_STATUS,   MB_RW,   0u,   3u,   0u,        0u },
  { REG_INTRUDER_CURRENT_X,      MB_RW,   0u, MODBUS_COORD_MAX, 0u, 0u },
  { REG_INTRUDER_CURRENT_Y,      MB_RW,   0u, MODBUS_COORD_MAX, 0u, 0u },
  { REG_INTRUDER_DIRECTION_X,    MB_RW,   0u, MODBUS_COORD_MAX, 0u, 0u },
  { REG_INTRUDER_DIRECTION_Y,    MB_RW,   0u, MODBUS_COORD_MAX, 0u, 0u },
  { REG_SCREEN_SELECT,           MB_RW,   0u,   3u,   0u,        0u },
  { REG_PROTOCOL_VERSION,        MB_RO,   0u, 0xFFFFu, MODBUS_PROTOCOL_VERSION, 0u },
};
#define MB_REG_COUNT  (sizeof(s_regs) / sizeof(s_regs[0]))

/* 진단 카운터 접근용 캐시(Init 에서 채운다). */
static mb_reg_t *s_rx_count  = NULL;
static mb_reg_t *s_crc_count = NULL;
static mb_reg_t *s_exc_count = NULL;

/* status가 0이 아닌 마지막 완전 좌표 묶음 수신 시각. */
static uint32_t s_intruder_last_update_ms = 0u;

/* 응답 조립 버퍼. Modbus 태스크 하나만 사용하므로 static으로 둔다. */
static uint8_t s_resp[MODBUS_FRAME_MAX];

/* ---------------------------------------------------------------------------
 * 유틸
 * ------------------------------------------------------------------------- */

/* Modbus CRC-16 (poly 0xA001, init 0xFFFF). 반환값 하위바이트가 먼저 전송된다. */
static uint16_t mb_crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFFu;
  for (uint16_t i = 0u; i < len; i++)
  {
    crc ^= data[i];
    for (uint8_t b = 0u; b < 8u; b++)
    {
      if ((crc & 1u) != 0u)
        crc = (uint16_t)((crc >> 1) ^ 0xA001u);
      else
        crc >>= 1;
    }
  }
  return crc;
}

static mb_reg_t *mb_find(uint16_t addr)
{
  for (uint16_t i = 0u; i < MB_REG_COUNT; i++)
  {
    if (s_regs[i].addr == addr)
      return &s_regs[i];
  }
  return NULL;
}

static int mb_is_intruder_addr(uint16_t addr)
{
  return (addr >= REG_INTRUDER_TRACK_STATUS) &&
         (addr <= REG_INTRUDER_DIRECTION_Y);
}

static int mb_range_overlaps_intruder(uint16_t start, uint16_t qty)
{
  uint32_t end = (uint32_t)start + (uint32_t)qty - 1u;
  return ((uint32_t)start <= REG_INTRUDER_DIRECTION_Y) &&
         (end >= REG_INTRUDER_TRACK_STATUS);
}

static int mb_intruder_status_valid(uint16_t status)
{
  return (status == 0u) ||
         (status == MODBUS_INTRUDER_CURRENT_VALID) ||
         (status == MODBUS_INTRUDER_BOTH_VALID);
}

/* uint16_t 카운터 증가(자연스러운 65536 순환). */
static void mb_bump(mb_reg_t *r)
{
  if (r != NULL)
    r->value = (uint16_t)(r->value + 1u);
}

/* 읽는 순간에 계산되는 레지스터를 최신화한다(UPTIME). */
static void mb_sync_dynamic(void)
{
  mb_reg_t *r = mb_find(REG_UPTIME_SECONDS);
  if (r != NULL)
    r->value = (uint16_t)(HAL_GetTick() / 1000u);   /* 65535초 이후 순환 */
}

/* CRC 를 붙여 프레임을 송신한다(len 은 CRC 제외 길이). */
static void mb_send(uint8_t *buf, uint16_t len)
{
  uint16_t crc = mb_crc16(buf, len);
  buf[len]     = (uint8_t)(crc & 0xFFu);          /* CRC Low 먼저 */
  buf[len + 1u]= (uint8_t)((crc >> 8) & 0xFFu);   /* CRC High 다음 */
  HAL_UART_Transmit(&huart6, buf, (uint16_t)(len + 2u), MODBUS_TX_TIMEOUT_MS);
}

/* 예외 응답(5 bytes). Broadcast 면 응답도 카운트도 하지 않는다. */
static void mb_send_exception(uint8_t fc, uint8_t code, int broadcast)
{
  if (broadcast)
    return;
  s_resp[0] = MODBUS_SLAVE_ID;
  s_resp[1] = (uint8_t)(fc | MODBUS_EXCEPTION_FLAG);
  s_resp[2] = code;
  mb_send(s_resp, 3u);
  mb_bump(s_exc_count);   /* 예외 응답 생성 횟수 */
}

/* 레지스터 쓰기의 실제 장치 반영. */
static void mb_apply_effect(uint16_t addr, uint16_t value)
{
  switch (addr)
  {
    case REG_LED_COMMAND:
      /* PA4 단일 GPIO LED (matrix 아님). 0=OFF, 1=ON 즉시 반영. */
      HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin,
                        (value != 0u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
      break;

    case REG_PANEL_BRIGHTNESS:
      HUB75_SetBrightness((uint8_t)value);
      break;

    case REG_PANEL_REFRESH_LEVEL:
      HUB75_SetRefreshRate((uint8_t)value);
      break;

    default:
      /* 센서/경보/화면 레지스터는 UI 태스크가 주기적으로 반영한다. */
      break;
  }
}

/* ---------------------------------------------------------------------------
 * 기능 코드 핸들러
 * ------------------------------------------------------------------------- */

/* 0x03 Read Holding Registers. 요청 8 bytes 고정. 응답 5 + 2N. */
static void mb_handle_read(const uint8_t *frame, uint16_t len, int broadcast)
{
  if (len != 8u)   /* id fc addr(2) qty(2) crc(2) */
  {
    mb_send_exception(MODBUS_FC_READ_HOLDING, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }

  uint16_t start = (uint16_t)((frame[2] << 8) | frame[3]);
  uint16_t qty   = (uint16_t)((frame[4] << 8) | frame[5]);

  if ((qty < 1u) || (qty > MODBUS_MAX_READ_QTY))
  {
    mb_send_exception(MODBUS_FC_READ_HOLDING, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }

  /* 요청 구간의 모든 주소가 정의돼 있어야 한다(불연속 구간은 예외). */
  for (uint16_t i = 0u; i < qty; i++)
  {
    if (mb_find((uint16_t)(start + i)) == NULL)
    {
      mb_send_exception(MODBUS_FC_READ_HOLDING, MODBUS_EX_ILLEGAL_ADDRESS, broadcast);
      return;
    }
  }

  /* Broadcast 읽기는 의미가 없으므로 응답하지 않는다(무해). */
  if (broadcast)
    return;

  mb_sync_dynamic();

  s_resp[0] = MODBUS_SLAVE_ID;
  s_resp[1] = MODBUS_FC_READ_HOLDING;
  s_resp[2] = (uint8_t)(2u * qty);   /* Byte Count */
  for (uint16_t i = 0u; i < qty; i++)
  {
    uint16_t v = mb_find((uint16_t)(start + i))->value;
    s_resp[3u + 2u * i] = (uint8_t)((v >> 8) & 0xFFu);   /* Hi 먼저 */
    s_resp[4u + 2u * i] = (uint8_t)(v & 0xFFu);          /* Lo 다음 */
  }
  mb_send(s_resp, (uint16_t)(3u + 2u * qty));
}

/* 0x06 Write Single Register. 요청 8 bytes 고정. 정상 응답 = 요청 Echo. */
static void mb_handle_write_single(const uint8_t *frame, uint16_t len, int broadcast)
{
  if (len != 8u)   /* id fc addr(2) value(2) crc(2) */
  {
    mb_send_exception(MODBUS_FC_WRITE_SINGLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }

  uint16_t addr = (uint16_t)((frame[2] << 8) | frame[3]);
  uint16_t val  = (uint16_t)((frame[4] << 8) | frame[5]);

  mb_reg_t *r = mb_find(addr);
  if (r == NULL)
  {
    mb_send_exception(MODBUS_FC_WRITE_SINGLE, MODBUS_EX_ILLEGAL_ADDRESS, broadcast);
    return;
  }
  if (r->access == MB_RO)   /* RO 쓰기 → Illegal Data Address(0x02) */
  {
    mb_send_exception(MODBUS_FC_WRITE_SINGLE, MODBUS_EX_ILLEGAL_ADDRESS, broadcast);
    return;
  }
  if ((val < r->min) || (val > r->max))   /* 값 범위 초과 → 0x03 */
  {
    mb_send_exception(MODBUS_FC_WRITE_SINGLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }

  /* 추적 데이터는 좌표 네 개가 섞이지 않도록 FC10 전체 묶음만 허용한다.
   * 예외적으로 status=0은 target off 용도로 즉시 허용한다. */
  if (mb_is_intruder_addr(addr) &&
      !((addr == REG_INTRUDER_TRACK_STATUS) && (val == 0u)))
  {
    mb_send_exception(MODBUS_FC_WRITE_SINGLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }

  if (addr == REG_INTRUDER_TRACK_STATUS)
  {
    taskENTER_CRITICAL();
    r->value = val;
    taskEXIT_CRITICAL();
  }
  else
  {
    r->value = val;
  }
  mb_apply_effect(addr, val);

  if (broadcast)   /* Broadcast(Slave 0) 쓰기: 반영만, 응답 없음 */
    return;

  /* 정상 응답 = 요청 프레임 Echo(id fc addr value + CRC 재계산). */
  s_resp[0] = MODBUS_SLAVE_ID;
  s_resp[1] = MODBUS_FC_WRITE_SINGLE;
  s_resp[2] = frame[2];
  s_resp[3] = frame[3];
  s_resp[4] = frame[4];
  s_resp[5] = frame[5];
  mb_send(s_resp, 6u);
}

/* 0x10 Write Multiple Registers. 요청 9 + 2N. 정상 응답 8 bytes 고정.
 * "전체 검증 후 일괄 반영": 한 주소라도 실패하면 아무것도 쓰지 않는다. */
static void mb_handle_write_multiple(const uint8_t *frame, uint16_t len, int broadcast)
{
  if (len < 11u)   /* 최소 N=1: 9 + 2 = 11 */
  {
    mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }

  uint16_t start = (uint16_t)((frame[2] << 8) | frame[3]);
  uint16_t qty   = (uint16_t)((frame[4] << 8) | frame[5]);
  uint8_t  bc    = frame[6];

  if ((qty < 1u) || (qty > MODBUS_MAX_WRITE_QTY))
  {
    mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }
  if (bc != (uint8_t)(2u * qty))
  {
    mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }
  if (len != (uint16_t)(9u + 2u * qty))   /* id fc start qty bc data(2N) crc */
  {
    mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }


  /* 121~125는 반드시 status+A(x,y)+B(x,y) 다섯 개를 한 프레임으로 쓴다. */
  int intruder_block = (start == REG_INTRUDER_TRACK_STATUS) && (qty == 5u);
  if (mb_range_overlaps_intruder(start, qty) && !intruder_block)
  {
    mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
    return;
  }

  /* 1차: 전체 검증 (주소 존재 + RW + 값 범위). 하나라도 실패면 반영 없음. */
  for (uint16_t i = 0u; i < qty; i++)
  {
    mb_reg_t *r = mb_find((uint16_t)(start + i));
    if ((r == NULL) || (r->access == MB_RO))
    {
      mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_ADDRESS, broadcast);
      return;
    }
    uint16_t v = (uint16_t)((frame[7u + 2u * i] << 8) | frame[8u + 2u * i]);
    if ((v < r->min) || (v > r->max))
    {
      mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
      return;
    }
  }

  if (intruder_block)
  {
    uint16_t status = (uint16_t)((frame[7] << 8) | frame[8]);
    if (!mb_intruder_status_valid(status))
    {
      mb_send_exception(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EX_ILLEGAL_VALUE, broadcast);
      return;
    }
  }

  /* 2차: 일괄 반영. */
  if (intruder_block)
  {
    taskENTER_CRITICAL();
    for (uint16_t i = 0u; i < qty; i++)
    {
      uint16_t addr = (uint16_t)(start + i);
      uint16_t v = (uint16_t)((frame[7u + 2u * i] << 8) | frame[8u + 2u * i]);
      mb_find(addr)->value = v;
    }
    s_intruder_last_update_ms = HAL_GetTick();
    taskEXIT_CRITICAL();
  }
  else
  {
    for (uint16_t i = 0u; i < qty; i++)
    {
      uint16_t addr = (uint16_t)(start + i);
      uint16_t v = (uint16_t)((frame[7u + 2u * i] << 8) | frame[8u + 2u * i]);
      mb_find(addr)->value = v;
      mb_apply_effect(addr, v);
    }
  }

  if (broadcast)
    return;

  /* 정상 응답: id fc start(2) qty(2) + CRC. 쓴 데이터는 Echo 하지 않는다. */
  s_resp[0] = MODBUS_SLAVE_ID;
  s_resp[1] = MODBUS_FC_WRITE_MULTIPLE;
  s_resp[2] = frame[2];
  s_resp[3] = frame[3];
  s_resp[4] = frame[4];
  s_resp[5] = frame[5];
  mb_send(s_resp, 6u);
}

/* ---------------------------------------------------------------------------
 * 공개 API
 * ------------------------------------------------------------------------- */

void Modbus_Slave_Init(void)
{
  for (uint16_t i = 0u; i < MB_REG_COUNT; i++)
    s_regs[i].value = s_regs[i].init;

  s_rx_count  = mb_find(REG_RX_FRAME_COUNT);
  s_crc_count = mb_find(REG_CRC_ERROR_COUNT);
  s_exc_count = mb_find(REG_EXCEPTION_COUNT);
  s_intruder_last_update_ms = HAL_GetTick();
}

void Modbus_Slave_OnFrame(const uint8_t *frame, uint16_t len)
{
  /* 최소 프레임(id + fc + CRC) = 4 bytes. 그보다 짧으면 잡음으로 폐기. */
  if ((frame == NULL) || (len < 4u) || (len > MODBUS_FRAME_MAX))
    return;

  /* 1) CRC 검증. 마지막 2바이트가 CRC(Lo, Hi). 불일치는 무응답 + 카운트. */
  uint16_t crc_calc = mb_crc16(frame, (uint16_t)(len - 2u));
  uint16_t crc_recv = (uint16_t)(frame[len - 2u] | (frame[len - 1u] << 8));
  if (crc_calc != crc_recv)
  {
    mb_bump(s_crc_count);
    return;
  }

  /* 2) Slave ID 판정. 우리(1)도 Broadcast(0)도 아니면 프레임 무시. */
  uint8_t id = frame[0];
  if ((id != MODBUS_SLAVE_ID) && (id != MODBUS_BROADCAST_ID))
    return;

  mb_bump(s_rx_count);   /* 정상 CRC + 대상 주소(Broadcast 포함) 프레임 */

  int broadcast = (id == MODBUS_BROADCAST_ID);
  uint8_t fc = frame[1];

  /* 3) 기능 코드 분기. */
  switch (fc)
  {
    case MODBUS_FC_READ_HOLDING:
      mb_handle_read(frame, len, broadcast);
      break;
    case MODBUS_FC_WRITE_SINGLE:
      mb_handle_write_single(frame, len, broadcast);
      break;
    case MODBUS_FC_WRITE_MULTIPLE:
      mb_handle_write_multiple(frame, len, broadcast);
      break;
    default:
      mb_send_exception(fc, MODBUS_EX_ILLEGAL_FUNCTION, broadcast);
      break;
  }
}

void Modbus_Slave_Poll(void)
{
  mb_reg_t *status = mb_find(REG_INTRUDER_TRACK_STATUS);
  uint32_t now = HAL_GetTick();

  /* 10초 동안 새 묶음이 안 오면 좌표 값은 진단용으로 남기고 유효 비트만
   * 내린다. UI는 다음 100ms 주기에 두 점을 지운다. unsigned 뺄셈이라
   * HAL tick 순환에도 안전하다. */
  taskENTER_CRITICAL();
  if ((status != NULL) && (status->value != 0u) &&
      ((uint32_t)(now - s_intruder_last_update_ms) >= MODBUS_INTRUDER_TIMEOUT_MS))
  {
    status->value = 0u;
  }
  taskEXIT_CRITICAL();
}

uint16_t Modbus_Slave_GetRegister(uint16_t addr, int *ok)
{
  mb_reg_t *r = mb_find(addr);
  if (r == NULL)
  {
    if (ok != NULL)
      *ok = 0;
    return 0u;
  }
  if (addr == REG_UPTIME_SECONDS)
    mb_sync_dynamic();
  if (ok != NULL)
    *ok = 1;
  return r->value;
}

int Modbus_Slave_GetIntruderTrack(Modbus_IntruderTrack *out)
{
  if (out == NULL)
    return 0;

  taskENTER_CRITICAL();
  out->status      = mb_find(REG_INTRUDER_TRACK_STATUS)->value;
  out->current_x   = mb_find(REG_INTRUDER_CURRENT_X)->value;
  out->current_y   = mb_find(REG_INTRUDER_CURRENT_Y)->value;
  out->direction_x = mb_find(REG_INTRUDER_DIRECTION_X)->value;
  out->direction_y = mb_find(REG_INTRUDER_DIRECTION_Y)->value;
  taskEXIT_CRITICAL();
  return 1;
}
