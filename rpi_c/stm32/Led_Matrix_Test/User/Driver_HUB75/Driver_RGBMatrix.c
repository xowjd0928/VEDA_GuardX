#include "Driver_RGBMatrix.h"

#if (HUB75_SCAN_ROWS == 0)
#error "HUB75_SCAN_ROWS must be greater than 0"
#endif
#if ((HUB75_PANEL_HEIGHT % 2u) != 0u)
#error "HUB75_PANEL_HEIGHT must be even"
#endif
#if (HUB75_SCAN_ROWS > (HUB75_PANEL_HEIGHT / 2u))
#error "HUB75_SCAN_ROWS must not exceed HUB75_PANEL_HEIGHT / 2"
#endif
#if !HUB75_PANEL_IS_SM5368 && (HUB75_SCAN_ROWS > 32u)
#error "HUB75_SCAN_ROWS must not exceed 32 with A/B/C/D/E address lines"
#endif

volatile uint16_t display_tick = 0;   /* ponytail: 진단용 임시 복구 */
HUB75_port RGB_Matrix;
uint8_t hub75_blink = 0;
uint8_t hub75_color = HUB75_Color_Pink;
static uint16_t hub75_oe_ticks;
uint8_t hub75_buff[HUB75_PANEL_WIDTH/8 * HUB75_PANEL_HEIGHT];
uint8_t hub75_panel_buff[HUB75_PANEL_WIDTH * HUB75_PANEL_HEIGHT / 2 * 3];

#define HUB75_DATA_PORTA_MASK    (R1_Pin | B2_Pin)
#define HUB75_DATA_PORTB_MASK    (G2_Pin | G1_Pin | R2_Pin | B1_Pin)
#define HUB75_ADDR_PORTA_MASK    (A_Pin | D_Pin | E_Pin)
#define HUB75_ADDR_PORTB_MASK    (C_Pin)
#define HUB75_ADDR_PORTC_MASK    (B_Pin)

#if HUB75_PANEL_IS_SM5368
#define HUB75_PACK_TOP_R_BIT       (7u)
#define HUB75_PACK_TOP_G_BIT       (6u)
#define HUB75_PACK_TOP_B_BIT       (5u)
#define HUB75_PACK_BOTTOM_R_BIT    (4u)
#define HUB75_PACK_BOTTOM_G_BIT    (3u)
#define HUB75_PACK_BOTTOM_B_BIT    (2u)
#else
#define HUB75_PACK_TOP_R_BIT       (5u)
#define HUB75_PACK_TOP_G_BIT       (6u)
#define HUB75_PACK_TOP_B_BIT       (7u)
#define HUB75_PACK_BOTTOM_R_BIT    (2u)
#define HUB75_PACK_BOTTOM_G_BIT    (3u)
#define HUB75_PACK_BOTTOM_B_BIT    (4u)
#endif

static inline void HUB75_AssignPackedBit(uint8_t *plane, uint8_t bit, uint8_t enabled)
{
  uint8_t mask = (uint8_t)(1u << bit);
  *plane = (enabled != 0u) ? (uint8_t)(*plane | mask) : (uint8_t)(*plane & (uint8_t)~mask);
}

static inline void HUB75_SetAddressBinaryFast(uint16_t line)
{
  uint32_t bsrr_a = (uint32_t)HUB75_ADDR_PORTA_MASK << 16u;
  uint32_t bsrr_b = (uint32_t)HUB75_ADDR_PORTB_MASK << 16u;
  uint32_t bsrr_c = (uint32_t)HUB75_ADDR_PORTC_MASK << 16u;

  if((line & 0x0001u) != 0u)
    bsrr_a |= A_Pin;
  if((line & 0x0002u) != 0u)
    bsrr_c |= B_Pin;
  if((line & 0x0004u) != 0u)
    bsrr_b |= C_Pin;
  if((line & 0x0008u) != 0u)
    bsrr_a |= D_Pin;
  if((line & 0x0010u) != 0u)
    bsrr_a |= E_Pin;

  GPIOA->BSRR = bsrr_a;
  GPIOB->BSRR = bsrr_b;
  GPIOC->BSRR = bsrr_c;
}

static inline void HUB75_SetAddressSm5368Phase(uint8_t row_data, uint8_t row_clk)
{
  uint32_t bsrr_a = (uint32_t)HUB75_ADDR_PORTA_MASK << 16u;
  uint32_t bsrr_b = (uint32_t)HUB75_ADDR_PORTB_MASK << 16u;
  uint32_t bsrr_c = ((uint32_t)HUB75_ADDR_PORTC_MASK << 16u) | B_Pin;

  if(row_clk != 0u)
    bsrr_a |= A_Pin;
  if(row_data != 0u)
    bsrr_b |= C_Pin;

  GPIOA->BSRR = bsrr_a;
  GPIOB->BSRR = bsrr_b;
  GPIOC->BSRR = bsrr_c;
}

static inline void HUB75_SetScanLineFast(uint16_t line)
{
#if HUB75_PANEL_IS_SM5368
  uint8_t row_data = (line == 0u) ? 1u : 0u;

  HUB75_SetAddressSm5368Phase(row_data, 0u);
  Delay_us(HUB75_SM5368_PHASE_DELAY_US);
  HUB75_SetAddressSm5368Phase(row_data, 1u);
  Delay_us(HUB75_SM5368_PHASE_DELAY_US);
  HUB75_SetAddressSm5368Phase(row_data, 0u);
  Delay_us(HUB75_SM5368_PHASE_DELAY_US);
#else
  HUB75_SetAddressBinaryFast(line);
#endif
}

static inline void HUB75_LatchRowFast(uint16_t scan_row)
{
  HUB75_OE_H();
  HUB75_LAT_H();
  HUB75_SetScanLineFast(scan_row);
  HUB75_LAT_L();
  HUB75_OE_L();
}
/**
 * Initialization routine.
 * You might need to enable access to DWT registers on Cortex-M7
 *   DWT->LAR = 0xC5ACCE55
 */
void DWT_Init(void)
{
    if (!(CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk)) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

/**
 * Delay routine itself.
 * Time is in microseconds (1/1000000th of a second), not to be
 * confused with millisecond (1/1000th).
 *
 * No need to check an overflow. Let it just tick :)
 *
 * @param uint32_t us  Number of microseconds to delay for
 */
void Delay_us(uint32_t us) // microseconds
{
    uint32_t startTick = DWT->CYCCNT,
             delayTicks = us * (SystemCoreClock/1000000);

    while (DWT->CYCCNT - startTick < delayTicks);
}


void HUB75_Init(void)
{
  HUB75_OE_H();
  HUB75_CLK_L();
  HUB75_LAT_L();

  HUB75_DR1_L();
  HUB75_DG1_L();
  HUB75_DB1_L();
  HUB75_DR2_L();
  HUB75_DG2_L();
  HUB75_DB2_L();

  HUB75_A_L();
  HUB75_B_L();
  HUB75_C_L();
  HUB75_D_L();
  HUB75_E_L();

  hub75_oe_ticks = 0U;
}

// Set refresh rate of the matrix
// level: 1-4, 1: highest, 4: lowest
void HUB75_SetRefreshRate(uint8_t level)
{
  uint32_t arr = 0;
  switch(level)
  {
    case 1:
      arr = 8U;
      break;
    case 2:
      arr = 12U;
      break;
    case 3:
      arr = 16U;
      break;
    case 4:
    default:
      arr = 20U;
      break;
  }
  __HAL_TIM_SET_COUNTER(&htim1, 0);
  __HAL_TIM_SET_AUTORELOAD(&htim1, arr);
  HAL_TIM_Base_Start_IT(&htim1);
}

/*
 * @brief  Set brightness of the matrix
 * @param brightness Brightness value (0-255)
 * @return None
 */
void HUB75_SetBrightness(uint8_t brightness)
{
  hub75_oe_ticks = (uint16_t)brightness;
}

// Change the screen display time by controlling the OE pin
static void HUB75_OE_Window(void)
{
  uint16_t ticks = hub75_oe_ticks;
  if(ticks == 0)
  {
    HUB75_OE_H();
    return;
  }
  while(ticks--)
    __NOP();
  HUB75_OE_H();
}

void send_scan_line(unsigned char line)
{
  HUB75_SetScanLineFast((uint16_t)line);
}

void HUB75_WriteByte(uint8_t p_buff[], uint8_t color)
{
  static uint16_t row = 0;
  uint16_t scan_row = HUB75_MAP_SCAN_ROW(row);
  uint8_t data_r1, data_g1, data_b1, data_r2, data_g2, data_b2 = 0;
  HUB75_OE_H();

  /* write column data */
  for(uint16_t i=0; i<(HUB75_PANEL_WIDTH/8); i++)
  {
    data_r1 = data_g1 = data_b1 = 0;
    data_r2 = data_g2 = data_b2 = 0;

    /* color red */
    if(color & 0x01)
    {
      data_b1 = p_buff[row*(HUB75_PANEL_WIDTH/8)+i];
      data_b2 = p_buff[row*(HUB75_PANEL_WIDTH/8)+(HUB75_PANEL_WIDTH/8*HUB75_PANEL_HEIGHT/2)+i];
    }

    /* color green */
    if(color & 0x02)
    {
      data_r1 = p_buff[row*(HUB75_PANEL_WIDTH/8)+i];
      data_r2 = p_buff[row*(HUB75_PANEL_WIDTH/8)+(HUB75_PANEL_WIDTH/8*HUB75_PANEL_HEIGHT/2)+i];
    }

    /* color blue */
    if(color & 0x04)
    {
      data_g1 = p_buff[row*(HUB75_PANEL_WIDTH/8)+i];
      data_g2 = p_buff[row*(HUB75_PANEL_WIDTH/8)+(HUB75_PANEL_WIDTH/8*HUB75_PANEL_HEIGHT/2)+i];
    }

    /* horizontal 8 bits data */
    for(uint8_t k=0; k<8; k++)
    {
      ((data_r1>>(7-k)) & 0x01) ? HUB75_DR1_H() : HUB75_DR1_L();
      ((data_g1>>(7-k)) & 0x01) ? HUB75_DG1_H() : HUB75_DG1_L();
      ((data_b1>>(7-k)) & 0x01) ? HUB75_DB1_H() : HUB75_DB1_L();
      ((data_r2>>(7-k)) & 0x01) ? HUB75_DR2_H() : HUB75_DR2_L();
      ((data_g2>>(7-k)) & 0x01) ? HUB75_DG2_H() : HUB75_DG2_L();
      ((data_b2>>(7-k)) & 0x01) ? HUB75_DB2_H() : HUB75_DB2_L();

      HUB75_CLK_L();
      HUB75_CLK_H();
    }
  }

  HUB75_OE_H();
  HUB75_LatchRowFast(scan_row);
  HUB75_OE_Window();

  if(++row >= HUB75_SCAN_ROWS){
    row = 0;
  }
}

void HUB75_WritePixel(uint8_t p_buff[])
{
  static const uint8_t plane_repeat[3] = {4U, 2U, 1U};
  static uint16_t plane = 0;
  static uint16_t row = 0;
  static uint8_t repeat_left = 4U;
  uint16_t scan_row = HUB75_MAP_SCAN_ROW(row);
  uint16_t row_offset = (uint16_t)(row * HUB75_PANEL_WIDTH);
  uint16_t plane_stride = (uint16_t)(HUB75_PANEL_WIDTH * (HUB75_PANEL_HEIGHT / 2u));
  const uint8_t *block0 = &p_buff[row_offset];
  const uint8_t *block1 = &p_buff[row_offset + plane_stride];
  const uint8_t *block2 = &p_buff[row_offset + (uint16_t)(plane_stride * 2u)];
  const uint8_t *plane_data = (plane == 0U) ? block0 : ((plane == 1U) ? block1 : block2);

  HUB75_OE_H();

  for(uint16_t i = 0; i < HUB75_PANEL_WIDTH; i++)
  {
    uint8_t byte = plane_data[i];
    uint32_t bsrr_a = (uint32_t)HUB75_DATA_PORTA_MASK << 16u;
    uint32_t bsrr_b = (uint32_t)HUB75_DATA_PORTB_MASK << 16u;

    if((byte & (1u << 5)) != 0u)
      bsrr_a |= R1_Pin;
    if((byte & (1u << 4)) != 0u)
      bsrr_a |= B2_Pin;

    if((byte & (1u << 3)) != 0u)
      bsrr_b |= G2_Pin;
    if((byte & (1u << 6)) != 0u)
      bsrr_b |= G1_Pin;
    if((byte & (1u << 2)) != 0u)
      bsrr_b |= R2_Pin;
    if((byte & (1u << 7)) != 0u)
      bsrr_b |= B1_Pin;

    GPIOA->BSRR = bsrr_a;
    GPIOB->BSRR = bsrr_b;
    HUB75_CLK_L();
    HUB75_CLK_H();
  }

  HUB75_LatchRowFast(scan_row);
  HUB75_OE_Window();

  if(++row == HUB75_SCAN_ROWS)
  {
    row = 0;

    if(repeat_left > 1U)
    {
      repeat_left--;
    }
    else
    {
      plane++;
      if(plane >= 3U)
        plane = 0U;
      repeat_left = plane_repeat[plane];
    }
  }
}

void HUB75_WritePanel(uint8_t R, uint8_t G, uint8_t B, uint16_t row, uint16_t column)
{
  uint8_t *p_plane1 = NULL, *p_plane2 = NULL, *p_plane3 = NULL;

  if((row >= HUB75_PANEL_HEIGHT) || (column >= HUB75_PANEL_WIDTH))
    return;

  p_plane1 = &hub75_panel_buff[row%(HUB75_PANEL_HEIGHT/2)*HUB75_PANEL_WIDTH+HUB75_PANEL_WIDTH*HUB75_PANEL_HEIGHT/2*0+column];
  p_plane2 = &hub75_panel_buff[row%(HUB75_PANEL_HEIGHT/2)*HUB75_PANEL_WIDTH+HUB75_PANEL_WIDTH*HUB75_PANEL_HEIGHT/2*1+column];
  p_plane3 = &hub75_panel_buff[row%(HUB75_PANEL_HEIGHT/2)*HUB75_PANEL_WIDTH+HUB75_PANEL_WIDTH*HUB75_PANEL_HEIGHT/2*2+column];

  if(row < (HUB75_PANEL_HEIGHT/2))      //Top
  {
    HUB75_AssignPackedBit(p_plane1, HUB75_PACK_TOP_R_BIT, (uint8_t)((R >> 6) & 0x01u));
    HUB75_AssignPackedBit(p_plane2, HUB75_PACK_TOP_R_BIT, (uint8_t)((R >> 5) & 0x01u));
    HUB75_AssignPackedBit(p_plane3, HUB75_PACK_TOP_R_BIT, (uint8_t)((R >> 4) & 0x01u));

    HUB75_AssignPackedBit(p_plane1, HUB75_PACK_TOP_G_BIT, (uint8_t)((G >> 6) & 0x01u));
    HUB75_AssignPackedBit(p_plane2, HUB75_PACK_TOP_G_BIT, (uint8_t)((G >> 5) & 0x01u));
    HUB75_AssignPackedBit(p_plane3, HUB75_PACK_TOP_G_BIT, (uint8_t)((G >> 4) & 0x01u));

    HUB75_AssignPackedBit(p_plane1, HUB75_PACK_TOP_B_BIT, (uint8_t)((B >> 6) & 0x01u));
    HUB75_AssignPackedBit(p_plane2, HUB75_PACK_TOP_B_BIT, (uint8_t)((B >> 5) & 0x01u));
    HUB75_AssignPackedBit(p_plane3, HUB75_PACK_TOP_B_BIT, (uint8_t)((B >> 4) & 0x01u));
  }
  else                                  //Bottom
  {
    HUB75_AssignPackedBit(p_plane1, HUB75_PACK_BOTTOM_R_BIT, (uint8_t)((R >> 6) & 0x01u));
    HUB75_AssignPackedBit(p_plane2, HUB75_PACK_BOTTOM_R_BIT, (uint8_t)((R >> 5) & 0x01u));
    HUB75_AssignPackedBit(p_plane3, HUB75_PACK_BOTTOM_R_BIT, (uint8_t)((R >> 4) & 0x01u));

    HUB75_AssignPackedBit(p_plane1, HUB75_PACK_BOTTOM_G_BIT, (uint8_t)((G >> 6) & 0x01u));
    HUB75_AssignPackedBit(p_plane2, HUB75_PACK_BOTTOM_G_BIT, (uint8_t)((G >> 5) & 0x01u));
    HUB75_AssignPackedBit(p_plane3, HUB75_PACK_BOTTOM_G_BIT, (uint8_t)((G >> 4) & 0x01u));

    HUB75_AssignPackedBit(p_plane1, HUB75_PACK_BOTTOM_B_BIT, (uint8_t)((B >> 6) & 0x01u));
    HUB75_AssignPackedBit(p_plane2, HUB75_PACK_BOTTOM_B_BIT, (uint8_t)((B >> 5) & 0x01u));
    HUB75_AssignPackedBit(p_plane3, HUB75_PACK_BOTTOM_B_BIT, (uint8_t)((B >> 4) & 0x01u));
  }
}

#define HUB75_PACK_AT(level, shift, bit)  ((uint8_t)((((level) >> (shift)) & 1u) << (bit)))

/* RGB565 픽셀에서 채널당 3비트 레벨을 뽑는다.
 * WritePanel은 8비트로 늘린 값의 비트 6/5/4만 읽는데, r8=(r5<<3)|(r5>>2)이므로
 * 그 세 비트는 결국 (r5>>1)&7 이다. g는 (g6>>2)&7. */
static inline void HUB75_Rgb565Levels(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b)
{
  *r = (uint8_t)(((px >> 11) & 0x1Fu) >> 1);
  *g = (uint8_t)(((px >> 5) & 0x3Fu) >> 2);
  *b = (uint8_t)((px & 0x1Fu) >> 1);
}

/*
 * @note   Writes every plane byte exactly once and keeps the scan running while
 *         it works. The previous version memset the whole buffer and then did
 *         9 read-modify-write calls per pixel (18k calls per frame); at -O0
 *         that blanked the panel for ~10ms on every redraw, which reads as a
 *         hard flicker. Nothing here may early-return: the unconditional write
 *         of all three planes is what replaces the memset.
 */
void HUB75_LoadRGB565Frame(const uint16_t *frame, uint16_t width, uint16_t height)
{
  const uint16_t half = (uint16_t)(HUB75_PANEL_HEIGHT / 2u);
  const uint16_t stride = (uint16_t)(HUB75_PANEL_WIDTH * half);

  if((frame == NULL) || (width == 0u) || (height == 0u))
    return;

  for(uint16_t y = 0u; y < half; y++)
  {
    uint8_t *plane0 = &hub75_panel_buff[(uint32_t)y * HUB75_PANEL_WIDTH];
    uint8_t *plane1 = plane0 + stride;
    uint8_t *plane2 = plane1 + stride;
    uint16_t y_bot = (uint16_t)(y + half);

    for(uint16_t x = 0u; x < HUB75_PANEL_WIDTH; x++)
    {
      uint8_t tr = 0u, tg = 0u, tb = 0u;
      uint8_t br = 0u, bg = 0u, bb = 0u;

      if(x < width)
      {
        if(y < height)
          HUB75_Rgb565Levels(frame[(uint32_t)y * width + x], &tr, &tg, &tb);
        if(y_bot < height)
          HUB75_Rgb565Levels(frame[(uint32_t)y_bot * width + x], &br, &bg, &bb);
      }

      /* 플레인 0/1/2 = 레벨의 비트 2/1/0. 노출 반복 4:2:1이라 0번이 MSB */
      plane0[x] = (uint8_t)(HUB75_PACK_AT(tr, 2, HUB75_PACK_TOP_R_BIT)
                          | HUB75_PACK_AT(tg, 2, HUB75_PACK_TOP_G_BIT)
                          | HUB75_PACK_AT(tb, 2, HUB75_PACK_TOP_B_BIT)
                          | HUB75_PACK_AT(br, 2, HUB75_PACK_BOTTOM_R_BIT)
                          | HUB75_PACK_AT(bg, 2, HUB75_PACK_BOTTOM_G_BIT)
                          | HUB75_PACK_AT(bb, 2, HUB75_PACK_BOTTOM_B_BIT));

      plane1[x] = (uint8_t)(HUB75_PACK_AT(tr, 1, HUB75_PACK_TOP_R_BIT)
                          | HUB75_PACK_AT(tg, 1, HUB75_PACK_TOP_G_BIT)
                          | HUB75_PACK_AT(tb, 1, HUB75_PACK_TOP_B_BIT)
                          | HUB75_PACK_AT(br, 1, HUB75_PACK_BOTTOM_R_BIT)
                          | HUB75_PACK_AT(bg, 1, HUB75_PACK_BOTTOM_G_BIT)
                          | HUB75_PACK_AT(bb, 1, HUB75_PACK_BOTTOM_B_BIT));

      plane2[x] = (uint8_t)(HUB75_PACK_AT(tr, 0, HUB75_PACK_TOP_R_BIT)
                          | HUB75_PACK_AT(tg, 0, HUB75_PACK_TOP_G_BIT)
                          | HUB75_PACK_AT(tb, 0, HUB75_PACK_TOP_B_BIT)
                          | HUB75_PACK_AT(br, 0, HUB75_PACK_BOTTOM_R_BIT)
                          | HUB75_PACK_AT(bg, 0, HUB75_PACK_BOTTOM_G_BIT)
                          | HUB75_PACK_AT(bb, 0, HUB75_PACK_BOTTOM_B_BIT));
    }

    /* 스캔은 이제 TIM1 ISR이 알아서 도니 여기서 펌프할 필요 없음.
     * 변환 도중에도 ISR이 끼어들어 계속 스캔하므로 암전은 여전히 없음.
     * 다만 이 두 줄이 갱신되는 도중에 ISR이 끼면 그 줄만 한 프레임 티어링 가능 - 기존과 동일한 트레이드오프 */
  }
}

/*
 * @brief  Call from the TIM1 update ISR. Just counts pending scan steps.
 * @note   HAL_TIM_PeriodElapsedCallback()은 프로젝트에 단 하나만 존재할 수 있는
 *         전역 콜백이라 FreeRTOS 타임베이스(TIM2)용으로 CubeMX가 main.c에 만든
 *         것과 공존할 수 없다 - main.c 쪽 콜백의 USER CODE 영역에서 이 함수를
 *         호출하는 방식으로 합쳤다.
 *         WritePixel()을 이 자리에서 직접 호출하는 버전을 시도했으나 실물에서
 *         패널이 아예 안 켜졌다(원인 미확정). 카운터만 올리고 HUB75_Display()가
 *         메인 루프/태스크에서 소진하는 이 방식이 실물로 확인된 안전한 버전.
 * @retval None
 */
void HUB75_OnScanTick(void)
{
  if(display_tick < 0xFFFFu)
    display_tick++;
}

/*
 * @brief  Execute pending HUB75 display refresh steps gated by TIM1.
 * @note   Drains at most HUB75_SCAN_ROWS steps per call so a slow caller can
 *         never starve the main loop (that budget is what fixed the earlier
 *         deadlock). Must be called often and unconditionally - RTOS로 옮기면
 *         이 호출을 가장 높은 우선순위 태스크에 둘 것.
 * @retval None
 */
void HUB75_Display(void)
{
  uint16_t budget = HUB75_SCAN_ROWS;

  while((display_tick != 0u) && (budget != 0u))
  {
    budget--;

    __disable_irq();
    if(display_tick == 0u)
    {
      __enable_irq();
      break;
    }
    display_tick--;
    __enable_irq();

    HUB75_WritePixel(hub75_panel_buff);
  }
}
