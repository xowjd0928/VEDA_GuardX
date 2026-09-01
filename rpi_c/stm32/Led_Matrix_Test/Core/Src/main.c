/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "Driver_RGBMatrix.h"
#include "modbus_slave.h"
#include "uart_comm.h"
#include <stdbool.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* 센서값 한 곳. 그리기 코드는 값의 출처를 모른다 */
typedef struct
{
  uint16_t t_x10;   /* 섭씨 x10. SENSOR_T_NA = 무응답 */
  uint8_t  h_pct;   /* 상대습도 %. SENSOR_H_NA = 무응답 */
} ZoneSensor;

typedef struct
{
  UWORD x;
  UWORD y;
} Pt;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 64x32 HUB75 1장. 화면 1(온습도 리드아웃) / 2(평면도 히트맵) / 3(화재)
 * 그리기는 전부 프레임버퍼 직접 쓰기. GUI_Paint 경로는 쓰지 않는다 */
#define APP_COLOR_TEST      0        /* 1이면 RGBW 세로 띠만 띄운다(배선 점검용) */
#define SENSOR_T_NA         0xFFFFu
#define SENSOR_H_NA         0xFFu

/* 좌표 검산. 규격이 경고한 "오른쪽 끝 64 / 아래 끝 32 초과"를 빌드 시점에 잡는다 */
_Static_assert(33 + 17 + 11 <= 64, "screen1 face overflows width");
_Static_assert(17 +  2 + 11 <= 32, "screen1 face overflows height");
_Static_assert(48 + 11      <= 64, "screen2 margin number overflows width");
_Static_assert(17 +  8 +  5 <= 32, "screen2 margin humidity overflows height");
_Static_assert(18 + 27      <= 64, "floor plan overflows width");
_Static_assert( 2 + 27      <= 32, "floor plan overflows height");
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static UWORD gui_framebuffer[HUB75_PANEL_WIDTH * HUB75_PANEL_HEIGHT];

/* Z1,Z2,Z3,Z4 = 좌상, 우상, 좌하, 우하 */
static ZoneSensor zone_sensor[4];
static ZoneSensor display_zone_sensor[4];
static Modbus_IntruderTrack intruder_track;
static Modbus_IntruderTrack display_intruder_track;

/* USART6 수신 오류가 나면 HAL이 DMA 수신을 종료한다. 오류 콜백에서 즉시
 * 재시작하되, HAL/DMA가 아직 READY가 아니면 Modbus 태스크가 다시 시도한다. */
static volatile uint8_t  uart6_rx_restart_pending;
static volatile uint32_t uart6_rx_error_count;
static volatile uint32_t uart6_rx_last_error;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */
void Modbus_Rtos_QueueFrameFromISR(const uint8_t *data, uint16_t len);
void Modbus_Uart_PollRecovery(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USART6 Modbus 수신을 IDLE DMA 모드로 시작한다. 오류 플래그는 SR/DR 읽기
 * 순서로 한꺼번에 지워 다음 시작 직후 같은 오류가 다시 걸리지 않게 한다. */
static HAL_StatusTypeDef Modbus_Uart_StartRx(void)
{
  HAL_StatusTypeDef status;

  __HAL_UART_CLEAR_OREFLAG(&huart6);
  status = HAL_UARTEx_ReceiveToIdle_DMA(&huart6, rx_buffer, sizeof(rx_buffer));
  if(status == HAL_OK)
  {
    __HAL_DMA_DISABLE_IT(huart6.hdmarx, DMA_IT_HT);
    uart6_rx_restart_pending = 0U;
  }
  else
  {
    uart6_rx_restart_pending = 1U;
  }

  return status;
}

/* 오류 IRQ 안에서 DMA 재시작이 실패했을 때 태스크 문맥에서 안전하게 정리한
 * 뒤 재시도한다. Modbus 태스크가 100 ms마다 호출하므로 RESET 없이 복구된다. */
void Modbus_Uart_PollRecovery(void)
{
  if(uart6_rx_restart_pending == 0U)
    return;

  (void)HAL_UART_AbortReceive(&huart6);
  (void)Modbus_Uart_StartRx();
}

/* --- 색 --------------------------------------------------------------- */

/* 채널당 0~7 레벨로 색을 만든다.
 * 드라이버가 각 채널의 비트 6/5/4만 읽고 비트7은 버리므로(HUB75_WritePanel),
 * RGB565를 눈대중으로 넣으면 엉뚱하게 어두워지거나 아예 검정이 된다. */
static UWORD app_rgb3(UWORD r, UWORD g, UWORD b)
{
  return (UWORD)(((r * 2U) << 11) | ((g * 4U) << 5) | (b * 2U));
}

static UWORD app_rgb3v(const uint8_t v[3])
{
  return app_rgb3(v[0], v[1], v[2]);
}

/* --- 프레임버퍼 --------------------------------------------------------- */

static void fb_fill(UWORD x, UWORD y, UWORD w, UWORD h, UWORD color)
{
  for(UWORD j = 0U; j < h; j++)
  {
    UWORD yy = (UWORD)(y + j);
    if(yy >= HUB75_PANEL_HEIGHT)
      break;
    for(UWORD i = 0U; i < w; i++)
    {
      UWORD xx = (UWORD)(x + i);
      if(xx >= HUB75_PANEL_WIDTH)
        break;
      gui_framebuffer[yy * HUB75_PANEL_WIDTH + xx] = color;
    }
  }
}

static void fb_flush(void)
{
  HUB75_LoadRGB565Frame(gui_framebuffer, HUB75_PANEL_WIDTH, HUB75_PANEL_HEIGHT);
}

/* --- 폰트 --------------------------------------------------------------- */

/* 5x7 굵은 숫자. 한 행의 하위 5비트, 왼쪽 픽셀이 bit4. 세로획 2px */
static const uint8_t font5x7[10][7] =
{
  {0x0E, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x0E},   /* 0 */
  {0x06, 0x0E, 0x06, 0x06, 0x06, 0x06, 0x0F},   /* 1 */
  {0x0E, 0x1B, 0x03, 0x06, 0x0C, 0x18, 0x1F},   /* 2 */
  {0x1F, 0x03, 0x0E, 0x03, 0x03, 0x1B, 0x0E},   /* 3 */
  {0x03, 0x07, 0x0F, 0x1B, 0x1F, 0x03, 0x03},   /* 4 */
  {0x1F, 0x18, 0x1E, 0x03, 0x03, 0x1B, 0x0E},   /* 5 */
  {0x06, 0x0C, 0x18, 0x1E, 0x1B, 0x1B, 0x0E},   /* 6 */
  {0x1F, 0x03, 0x03, 0x06, 0x06, 0x0C, 0x0C},   /* 7 */
  {0x0E, 0x1B, 0x1B, 0x0E, 0x1B, 0x1B, 0x0E},   /* 8 */
  {0x0E, 0x1B, 0x1B, 0x0F, 0x03, 0x06, 0x0C}    /* 9 */
};
static const uint8_t font5x7_dash[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};

/* 3x5 숫자. 한 행의 하위 3비트, 왼쪽 픽셀이 bit2 */
static const uint8_t font3x5[10][5] =
{
  {0x7U, 0x5U, 0x5U, 0x5U, 0x7U},   /* 0 */
  {0x2U, 0x6U, 0x2U, 0x2U, 0x7U},   /* 1 */
  {0x7U, 0x1U, 0x7U, 0x4U, 0x7U},   /* 2 */
  {0x7U, 0x1U, 0x7U, 0x1U, 0x7U},   /* 3 */
  {0x5U, 0x5U, 0x7U, 0x1U, 0x1U},   /* 4 */
  {0x7U, 0x4U, 0x7U, 0x1U, 0x7U},   /* 5 */
  {0x7U, 0x4U, 0x7U, 0x5U, 0x7U},   /* 6 */
  {0x7U, 0x1U, 0x2U, 0x2U, 0x2U},   /* 7 */
  {0x7U, 0x5U, 0x7U, 0x5U, 0x7U},   /* 8 */
  {0x7U, 0x5U, 0x7U, 0x1U, 0x7U}    /* 9 */
};
static const uint8_t font3x5_dash[5] = {0x0U, 0x0U, 0x7U, 0x0U, 0x0U};

/* rows[r]의 하위 w비트, 왼쪽 픽셀이 bit(w-1) */
static void fb_glyph(UWORD x, UWORD y, const uint8_t *rows, UWORD w, UWORD h, UWORD color)
{
  for(UWORD r = 0U; r < h; r++)
  {
    for(UWORD c = 0U; c < w; c++)
    {
      if(((rows[r] >> (w - 1U - c)) & 1U) != 0U)
        fb_fill((UWORD)(x + c), (UWORD)(y + r), 1U, 1U, color);
    }
  }
}

/* 두 자리. 0~99 밖이거나 무응답이면 규격대로 "--" 3·3·3. 클램프하지 않는다 */
static void fb_num2_5x7(UWORD x, UWORD y, int value, UWORD color)
{
  bool na = (value < 0) || (value > 99);
  UWORD col = na ? app_rgb3(3U, 3U, 3U) : color;

  fb_glyph(x,             y, na ? font5x7_dash : font5x7[(value / 10) % 10], 5U, 7U, col);
  fb_glyph((UWORD)(x + 6U), y, na ? font5x7_dash : font5x7[value % 10],      5U, 7U, col);
}

static void fb_num2_3x5(UWORD x, UWORD y, int value, UWORD color)
{
  bool na = (value < 0) || (value > 99);
  UWORD col = na ? app_rgb3(3U, 3U, 3U) : color;

  fb_glyph(x,             y, na ? font3x5_dash : font3x5[(value / 10) % 10], 3U, 5U, col);
  fb_glyph((UWORD)(x + 4U), y, na ? font3x5_dash : font3x5[value % 10],      3U, 5U, col);
}

/* --- 불쾌지수 얼굴 ------------------------------------------------------ */

/* 11x11, 하위 11비트, 왼쪽 픽셀이 bit10.
 * 모양과 색이 함께 바뀐다 - 색만이면 색약 사용자가 못 읽고,
 * 모양만이면 원거리에서 11px가 뭉개진다 */
static const uint16_t face11[4][11] =
{
  /* 쾌적: 웃는 입 */
  {0x0F8, 0x306, 0x202, 0x489, 0x489, 0x401, 0x505, 0x4F9, 0x202, 0x306, 0x0F8},
  /* 약간불쾌: 일자 입 */
  {0x0F8, 0x306, 0x202, 0x489, 0x489, 0x401, 0x401, 0x4F9, 0x202, 0x306, 0x0F8},
  /* 불쾌: 찡그린 입 */
  {0x0F8, 0x306, 0x202, 0x489, 0x489, 0x401, 0x4F9, 0x505, 0x202, 0x306, 0x0F8},
  /* 매우불쾌: 찡그린 입 + 찌푸린 눈썹 */
  {0x0F8, 0x306, 0x202, 0x58D, 0x489, 0x401, 0x4F9, 0x505, 0x202, 0x306, 0x0F8}
};
static const uint8_t di_rgb[4][3] = {{0U,6U,0U}, {6U,6U,0U}, {7U,4U,0U}, {7U,1U,0U}};

static void fb_face11(UWORD x, UWORD y, const uint16_t rows[11], UWORD color)
{
  for(UWORD r = 0U; r < 11U; r++)
  {
    for(UWORD c = 0U; c < 11U; c++)
    {
      if(((rows[r] >> (10U - c)) & 1U) != 0U)
        fb_fill((UWORD)(x + c), (UWORD)(y + r), 1U, 1U, color);
    }
  }
}

/* DI = 0.81T + 0.01H(0.99T - 14.3) + 46.3 */
static uint8_t app_di_level(uint16_t t_x10, uint8_t h_pct)
{
  float t = (float)t_x10 / 10.0f;
  float di = (0.81f * t) + (0.01f * (float)h_pct * ((0.99f * t) - 14.3f)) + 46.3f;

  if(di < 70.0f)
    return 0U;
  if(di < 75.0f)
    return 1U;
  if(di < 80.0f)
    return 2U;
  return 3U;
}

/* --- 센서 접근 (그리기 코드는 여기로만 읽는다) --------------------------- */

static int zone_temp_c(uint8_t z)
{
  uint16_t t = display_zone_sensor[z].t_x10;
  return (t == SENSOR_T_NA) ? -1 : (int)(t / 10U);
}

static int zone_humid(uint8_t z)
{
  uint8_t h = display_zone_sensor[z].h_pct;
  return (h == SENSOR_H_NA) ? -1 : (int)h;
}

static bool zone_ok(uint8_t z)
{
  return (zone_temp_c(z) >= 0) && (zone_humid(z) >= 0);
}

/* Modbus 가상 레지스터(100~107)를 화면용 센서 상태로 반영한다.
 * 값이 실제로 바뀐 경우에만 true를 반환해 Render 태스크가 다시 그리게 한다. */
bool App_UpdateSensorsFromModbus(void)
{
  static const uint16_t temp_addr[4] = {
    REG_ZONE1_TEMP_X10, REG_ZONE2_TEMP_X10,
    REG_ZONE3_TEMP_X10, REG_ZONE4_TEMP_X10
  };
  static const uint16_t humid_addr[4] = {
    REG_ZONE1_HUMIDITY, REG_ZONE2_HUMIDITY,
    REG_ZONE3_HUMIDITY, REG_ZONE4_HUMIDITY
  };
  bool changed = false;

  for(uint8_t z = 0U; z < 4U; z++)
  {
    int ok_temp = 0;
    int ok_humid = 0;
    uint16_t temp = Modbus_Slave_GetRegister(temp_addr[z], &ok_temp);
    uint16_t humid_raw = Modbus_Slave_GetRegister(humid_addr[z], &ok_humid);
    uint8_t humid = (ok_humid && (humid_raw <= 100U))
                  ? (uint8_t)humid_raw : SENSOR_H_NA;

    if(!ok_temp)
      temp = SENSOR_T_NA;

    if((zone_sensor[z].t_x10 != temp) || (zone_sensor[z].h_pct != humid))
    {
      zone_sensor[z].t_x10 = temp;
      zone_sensor[z].h_pct = humid;
      changed = true;
    }
  }

  return changed;
}

/* 침입자 현재점 A와 방향점 B를 원자적 스냅샷으로 가져온다. */
bool App_UpdateIntruderFromModbus(void)
{
  Modbus_IntruderTrack next;

  if(!Modbus_Slave_GetIntruderTrack(&next))
    return false;
  if(memcmp(&intruder_track, &next, sizeof(next)) == 0)
    return false;

  intruder_track = next;
  return true;
}

/* 화면 1과 이어지는 화면 2가 반드시 같은 시점의 데이터를 사용하도록 고정한다. */
void App_CaptureDisplayState(void)
{
  memcpy(display_zone_sensor, zone_sensor, sizeof(display_zone_sensor));
  display_intruder_track = intruder_track;
}

/* --- 화면 1: 온습도 리드아웃 -------------------------------------------- */

static const Pt cell_origin[4] = {{0U, 0U}, {33U, 0U}, {0U, 17U}, {33U, 17U}};

void App_DrawScreen1(void)
{
  const UWORD col_grid = app_rgb3(1U, 1U, 1U);

  memset(gui_framebuffer, 0, sizeof(gui_framebuffer));

  /* 격자: x31-32 세로 전체, y15-16 가로 전체 */
  fb_fill(31U, 0U, 2U, HUB75_PANEL_HEIGHT, col_grid);
  fb_fill(0U, 15U, HUB75_PANEL_WIDTH, 2U, col_grid);

  for(uint8_t z = 0U; z < 4U; z++)
  {
    UWORD ox = cell_origin[z].x;
    UWORD oy = cell_origin[z].y;

    fb_num2_5x7((UWORD)(ox + 2U), (UWORD)(oy + 1U), zone_temp_c(z), app_rgb3(7U, 7U, 7U));
    fb_num2_3x5((UWORD)(ox + 2U), (UWORD)(oy + 9U), zone_humid(z),  app_rgb3(3U, 4U, 5U));

    if(zone_ok(z))
    {
      uint8_t d = app_di_level(display_zone_sensor[z].t_x10,
                               display_zone_sensor[z].h_pct);
      fb_face11((UWORD)(ox + 17U), (UWORD)(oy + 2U), face11[d], app_rgb3v(di_rgb[d]));
    }
  }

  fb_flush();
}

/* --- 화면 2/3 공용 평면도 ------------------------------------------------ */

static const Pt plan_zone[4] = {{19U, 3U}, {32U, 3U}, {19U, 16U}, {32U, 16U}};

/* 외벽 + 십자벽 + 출입구 틈. 구역을 칠한 뒤에 그린다 */
static void fb_floor_walls(void)
{
  const UWORD w = app_rgb3(6U, 6U, 7U);

  fb_fill(18U,  2U, 27U,  1U, w);   /* 외벽 상 */
  fb_fill(18U, 28U, 27U,  1U, w);   /* 외벽 하 */
  fb_fill(18U,  2U,  1U, 27U, w);   /* 외벽 좌 */
  fb_fill(44U,  2U,  1U, 27U, w);   /* 외벽 우 */
  fb_fill(31U,  2U,  1U, 27U, w);   /* 십자 세로 */
  fb_fill(18U, 15U, 27U,  1U, w);   /* 십자 가로 */

  /* 출입구: 벽 픽셀을 실제로 소등해서 틈을 낸다 */
  fb_fill(36U,  2U, 5U, 1U, 0U);    /* 상단 우측 */
  fb_fill(23U, 28U, 5U, 1U, 0U);    /* 하단 좌측 */
}

/* 출입구 표식 5x1. 벽과 1px 여백 */
static void fb_door_marks(UWORD color)
{
  fb_fill(36U,  0U, 5U, 1U, color);
  fb_fill(23U, 30U, 5U, 1U, color);
}

/* --- 화면 2: 구역별 환경 상태(불쾌지수) ----------------------------------- */

/* 0~1000 좌표를 평면도 안쪽의 3x3 점 좌상단으로 바꾼다.
 * 입력 (0,0)=좌상단, (1000,1000)=우하단이다. */
static Pt app_intruder_dot(uint16_t x, uint16_t y)
{
  Pt p;
  p.x = (UWORD)(19U + ((uint32_t)x * 22U) / MODBUS_COORD_MAX);
  p.y = (UWORD)( 3U + ((uint32_t)y * 22U) / MODBUS_COORD_MAX);
  return p;
}

static void fb_intruder_dots(void)
{
  if((display_intruder_track.status & MODBUS_INTRUDER_DIRECTION_VALID) != 0U)
  {
    Pt b = app_intruder_dot(display_intruder_track.direction_x,
                            display_intruder_track.direction_y);
    fb_fill(b.x, b.y, 3U, 3U, app_rgb3(0U, 7U, 7U));  /* B: 방향점, 청록 */
  }

  if((display_intruder_track.status & MODBUS_INTRUDER_CURRENT_VALID) != 0U)
  {
    Pt a = app_intruder_dot(display_intruder_track.current_x,
                            display_intruder_track.current_y);
    fb_fill(a.x, a.y, 3U, 3U, app_rgb3(7U, 7U, 7U));  /* A: 현재점, 흰색 */
  }
}

/* Zone 1 우측 상단의 화재 셔터. 상단 출입문과 같은 높이로 표시한다. */
static void fb_fire_shutter(void)
{
  fb_fill(26U, 2U, 5U, 1U, 0U);
  fb_fill(26U, 0U, 5U, 1U, app_rgb3(7U, 2U, 0U));
}

void App_DrawScreen2(void)
{
  memset(gui_framebuffer, 0, sizeof(gui_framebuffer));

  for(uint8_t z = 0U; z < 4U; z++)
  {
    UWORD col;
    UWORD label_col;

    if(!zone_ok(z))
    {
      col = app_rgb3(1U, 1U, 1U);   /* 무응답 구역은 눌러둔다 */
      label_col = app_rgb3(6U, 6U, 7U);
    }
    else
    {
      uint8_t d = app_di_level(display_zone_sensor[z].t_x10,
                               display_zone_sensor[z].h_pct);
      col = app_rgb3v(di_rgb[d]);
      label_col = 0U;
    }

    fb_fill(plan_zone[z].x, plan_zone[z].y, 12U, 12U, col);
    fb_glyph((UWORD)(plan_zone[z].x + 3U), (UWORD)(plan_zone[z].y + 2U),
             font5x7[z + 1U], 5U, 7U, label_col);
  }

  fb_floor_walls();
  fb_door_marks(app_rgb3(0U, 5U, 0U));
  fb_intruder_dots();
  fb_fire_shutter();

  fb_flush();
}

/* --- 화면 3: 화재 -------------------------------------------------------- */

/* ponytail: 출입구 두 곳을 항상 함께 점멸시킨다. 화재가 문 쪽 구역에서 났을 때
 * 위험한 문을 끄는 규칙은 미결이라 넣지 않았다 */
void App_DrawScreen3(uint8_t fire_zone, bool blink_on)
{
  memset(gui_framebuffer, 0, sizeof(gui_framebuffer));

  for(uint8_t z = 0U; z < 4U; z++)
  {
    UWORD col;

    if(z == fire_zone)
      col = blink_on ? app_rgb3(7U, 0U, 0U) : app_rgb3(3U, 0U, 0U);
    else
      col = app_rgb3(1U, 1U, 1U);   /* 시선을 뺏지 않게 눌러둔다 */

    fb_fill(plan_zone[z].x, plan_zone[z].y, 12U, 12U, col);
  }

  fb_floor_walls();

  /* 출입구는 화재 구역과 반대 위상.
   * 같은 위상이면 "어디로 가라"가 아니라 화면 전체가 깜빡이는 걸로 보인다 */
  fb_door_marks(blink_on ? app_rgb3(0U, 2U, 0U) : app_rgb3(0U, 7U, 0U));

  /* 화재 구역 안에만 구역번호 한 자리. 규격의 (23,7)은 Z1 기준 +4,+4 */
  fb_glyph((UWORD)(plan_zone[fire_zone].x + 4U), (UWORD)(plan_zone[fire_zone].y + 4U),
           font5x7[(fire_zone + 1U) % 10U], 5U, 7U, app_rgb3(7U, 7U, 7U));

  fb_flush();
}

#if APP_COLOR_TEST
/* 채널별 배선 점검용. 세로 띠라 위/아래 절반(R1G1B1 / R2G2B2)을 동시에 지난다 */
static void App_DrawColorTest(void)
{
  memset(gui_framebuffer, 0, sizeof(gui_framebuffer));
  fb_fill(0U,  0U, 16U, HUB75_PANEL_HEIGHT, app_rgb3(7U, 0U, 0U));
  fb_fill(16U, 0U, 16U, HUB75_PANEL_HEIGHT, app_rgb3(0U, 7U, 0U));
  fb_fill(32U, 0U, 16U, HUB75_PANEL_HEIGHT, app_rgb3(0U, 0U, 7U));
  fb_fill(48U, 0U, 16U, HUB75_PANEL_HEIGHT, app_rgb3(7U, 7U, 7U));
  fb_flush();
}
#endif
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM4_Init();
  MX_TIM1_Init();
  MX_USART6_UART_Init();
  /* USER CODE BEGIN 2 */
  /* CubeMX NVIC 화면은 FreeRTOS가 켜지면 5보다 급한(숫자가 작은) 우선순위를
   * 못 고르게 막는다 - RTOS API를 부르는 ISR이 실수로 커널 관리 밖에 놓이는
   * 걸 막기 위한 안전장치. TIM1 스캔 ISR은 정확히 그 "관리 밖"에 있어야
   * 하고 RTOS API를 전혀 안 부르니 여기서 GUI값(5) 위에 덮어쓴다.
   * MX_TIM1_Init()이 이미 위에서 5로 설정한 뒤라 순서상 이 자리여야 함 */
  HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 0, 0);

  Modbus_Slave_Init();

  DWT_Init();
  HUB75_Init();
  HUB75_SetBrightness(255);
  /* 1 = 가장 빠름. 낮은 값일수록 스캔이 잦아 깜빡임이 준다.
   * 4는 초당 프레임을 61Hz까지 떨어뜨려 오히려 더 깜빡인다 */
  HUB75_SetRefreshRate(1);

  App_UpdateSensorsFromModbus();
  App_CaptureDisplayState();
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Queue가 만들어진 뒤 DMA 수신을 시작해야 첫 프레임도 유실되지 않는다. */
  if(Modbus_Uart_StartRx() != HAL_OK)
  {
    Error_Handler();
  }

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* osKernelStart()는 리턴하지 않는다. 화면/Modbus 로직은 freertos.c의
   * Render, UI, Modbus 태스크에서 각각 실행된다. */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  /* USER CODE END WHILE */

  /* USER CODE BEGIN 3 */
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if(huart->Instance != USART6)
    return;

  /* IRQ에서는 프레임을 큐에 복사하고 즉시 복귀한다. CRC 검증과 응답 송신은
   * Modbus 태스크에서 처리하므로 LED 스캔과 다른 태스크를 오래 막지 않는다. */
  Modbus_Rtos_QueueFrameFromISR(rx_buffer, Size);

  (void)Modbus_Uart_StartRx();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if(huart->Instance != USART6)
    return;

  /* DMA 모드의 ORE/FE/NE는 HAL 내부에서 blocking error로 처리되어 RxState가
   * READY로 돌아온 뒤 이 콜백에 도착한다. 오류를 기록하고 즉시 재수신한다. */
  uart6_rx_last_error = huart->ErrorCode;
  uart6_rx_error_count++;
  uart6_rx_restart_pending = 1U;
  (void)Modbus_Uart_StartRx();
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM2 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */
  if (htim->Instance == TIM1)
  {
    HUB75_OnScanTick();
    return;
  }
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM2)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
