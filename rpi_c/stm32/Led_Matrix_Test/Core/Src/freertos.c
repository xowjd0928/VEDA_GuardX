/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "Driver_RGBMatrix.h"
#include "modbus_slave.h"
#include <stdbool.h>
#include <string.h>

/* main.c의 화면 데이터 반영/그리기 함수 */
bool App_UpdateSensorsFromModbus(void);
bool App_UpdateIntruderFromModbus(void);
void App_CaptureDisplayState(void);
void Modbus_Uart_PollRecovery(void);
void App_DrawScreen1(void);
void App_DrawScreen2(void);
void App_DrawScreen3(uint8_t fire_zone, bool blink_on);

#define SCREEN_PERIOD_MS    5000U
#define MODBUS_FRAME_MAX    256U
#define MODBUS_QUEUE_DEPTH  4U
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct
{
  uint16_t len;
  uint8_t data[MODBUS_FRAME_MAX];
} ModbusFrame;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* Render가 읽고 UI가 쓰는 단순 스칼라 상태. */
static volatile uint8_t  g_screen_idx  = 0U;   /* 0=화면1, 1=화면2, 2=화면3 */
static volatile bool     g_fire        = false;
static volatile uint8_t  g_fire_zone   = 0U;
static volatile uint32_t g_data_revision = 1U;

static osMessageQueueId_t modbusQueueHandle;

static osThreadId_t renderTaskHandle;
static const osThreadAttr_t renderTask_attributes = {
  .name = "Render",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,   /* 스캔 펌프를 쥐고 있어 최우선 */
};

static osThreadId_t uiTaskHandle;
static const osThreadAttr_t uiTask_attributes = {
  .name = "UI",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "Modbus",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void StartRenderTask(void *argument);
void StartUiTask(void *argument);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  modbusQueueHandle = osMessageQueueNew(MODBUS_QUEUE_DEPTH,
                                        sizeof(ModbusFrame), NULL);
  if(modbusQueueHandle == NULL)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  renderTaskHandle = osThreadNew(StartRenderTask, NULL, &renderTask_attributes);
  uiTaskHandle = osThreadNew(StartUiTask, NULL, &uiTask_attributes);
  if((defaultTaskHandle == NULL) || (renderTaskHandle == NULL)
      || (uiTaskHandle == NULL))
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  ModbusFrame frame;

  /* UART IRQ가 큐에 넣은 프레임을 태스크 문맥에서 검증하고 응답한다. */
  for(;;)
  {
    if(osMessageQueueGet(modbusQueueHandle, &frame, NULL, 100U) == osOK)
    {
      Modbus_Slave_OnFrame(frame.data, frame.len);
    }
    Modbus_Slave_Poll();
    Modbus_Uart_PollRecovery();
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/*
 * @brief  최우선순위. 스캔 펌프를 쥐고 있고, UI가 써둔 공유 상태를
 *         읽어서 바뀐 게 있을 때만 다시 그린다.
 * @note   osDelay(1)로 매 루프 블로킹하는 게 핵심 - 안 그러면 이 태스크가
 *         Normal 우선순위인 UI/Modbus를 영원히 못 돌게 굶긴다. 1ms 주기는
 *         TIM1 틱(~62.5us)이 그 사이 16개 쌓이는 계산이라 HUB75_Display()의
 *         예산(HUB75_SCAN_ROWS=16)과 정확히 맞아떨어진다.
 * @retval None
 */
void StartRenderTask(void *argument)
{
  uint32_t sig_last = 0xFFFFFFFFu;

  for(;;)
  {
    HUB75_Display();

    bool    blink_on   = ((HAL_GetTick() / 500U) % 2U) == 0U;
    uint8_t screen_idx = g_screen_idx;
    bool    fire        = g_fire;
    uint8_t fire_zone   = g_fire_zone;
    uint32_t revision = g_data_revision;
    bool screen3_active = fire || (screen_idx == 2U);

    uint32_t sig = (uint32_t)screen_idx
                 | ((uint32_t)fire << 2)
                 | ((uint32_t)(screen3_active && blink_on) << 3)
                 | ((uint32_t)fire_zone << 4)
                 | (revision << 8);

    if(sig != sig_last)
    {
      sig_last = sig;

      if(screen3_active)
        App_DrawScreen3(fire_zone, blink_on);
      else if(screen_idx == 0U)
        App_DrawScreen1();
      else
        App_DrawScreen2();
    }

    osDelay(1);
  }
}

/*
 * @brief  Modbus 레지스터를 화면 상태로 옮긴다. SCREEN_SELECT=0이면
 *         화면1/2 자동 순환, 1~3이면 해당 화면 고정이다. 화재 bitmap은
 *         화면 선택보다 우선하며, 여러 bit가 켜지면 가장 낮은 Zone을 표시한다.
 * @retval None
 */
void StartUiTask(void *argument)
{
  uint32_t screen_last = HAL_GetTick();
  uint8_t auto_screen_idx = 0U;
  uint8_t previous_screen_idx = 0xFFU;

  for(;;)
  {
    uint32_t now = HAL_GetTick();
    int ok_screen = 0;
    int ok_fire = 0;
    uint16_t screen_select = Modbus_Slave_GetRegister(REG_SCREEN_SELECT, &ok_screen);
    uint16_t fire_bitmap = Modbus_Slave_GetRegister(REG_FIRE_ZONE_BITMAP, &ok_fire);

    (void)App_UpdateSensorsFromModbus();
    (void)App_UpdateIntruderFromModbus();

    if(!ok_screen || (screen_select > 3U))
      screen_select = 0U;

    if(screen_select == 0U)
    {
      if((now - screen_last) >= SCREEN_PERIOD_MS)
      {
        screen_last = now;
        auto_screen_idx ^= 1U;
      }
      g_screen_idx = auto_screen_idx;
    }
    else
    {
      g_screen_idx = (uint8_t)(screen_select - 1U);
      screen_last = now;
    }

    /* 현재 화면은 표시되는 동안 고정하고, 화면 전환 때만 최신 데이터로 다시 그린다. */
    if(g_screen_idx != previous_screen_idx)
    {
      /* 화면 1과 바로 다음 화면 2가 하나의 센서 스냅샷을 공유한다. */
      if((previous_screen_idx == 0xFFU) || (g_screen_idx == 0U)
          || ((g_screen_idx == 1U) && (previous_screen_idx != 0U)))
        App_CaptureDisplayState();

      previous_screen_idx = g_screen_idx;
      g_data_revision++;
    }

    fire_bitmap = ok_fire ? (uint16_t)(fire_bitmap & 0x000FU) : 0U;
    g_fire = (fire_bitmap != 0U);

    if(fire_bitmap != 0U)
    {
      for(uint8_t zone = 0U; zone < 4U; zone++)
      {
        if((fire_bitmap & (uint16_t)(1U << zone)) != 0U)
        {
          g_fire_zone = zone;
          break;
        }
      }
    }

    osDelay(100);
  }
}

/* USART6/DMA IRQ 전용 진입점. IRQ 우선순위는 FreeRTOS API 허용 경계인 5다. */
void Modbus_Rtos_QueueFrameFromISR(const uint8_t *data, uint16_t len)
{
  ModbusFrame frame;

  if((modbusQueueHandle == NULL) || (data == NULL)
      || (len < 4U) || (len > MODBUS_FRAME_MAX))
  {
    return;
  }

  frame.len = len;
  memcpy(frame.data, data, len);
  (void)osMessageQueuePut(modbusQueueHandle, &frame, 0U, 0U);
}

/* USER CODE END Application */

