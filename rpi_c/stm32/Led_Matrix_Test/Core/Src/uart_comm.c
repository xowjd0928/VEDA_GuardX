//uart_comm.c
#include "uart_comm.h"
#include <string.h>

#define BUF_SIZE 256

static uint8_t cmd_buffer[256] = {0};
static uint16_t cmd_idx = 0;

uint8_t rx_buffer[256];
extern UART_HandleTypeDef huart6;

//static volatile uint8_t rx_flag = 0;
static void LED_ON(void) { HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);}
static void LED_OFF(void) { HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);}

void Uart_Comm_RxCallback(uint16_t size)
{
    for (uint16_t i = 0; i < size; i++)
    {
        uint8_t data = rx_buffer[i];

        if (data == '\r' || data == '\n') {
            if (cmd_idx > 0) {
                uint8_t newline[2] = {'\r', '\n'};
                HAL_UART_Transmit(&huart6, newline, 2, 100);

                cmd_buffer[cmd_idx] = '\0';

                if (strcmp((char *)cmd_buffer, "LED_ON") == 0)
                    LED_ON();
                else if (strcmp((char *)cmd_buffer, "LED_OFF") == 0)
                    LED_OFF();

                cmd_idx = 0;
            }
        } else {
            if (cmd_idx < BUF_SIZE - 1) {
                cmd_buffer[cmd_idx++] = data;
                HAL_UART_Transmit(&huart6, &data, 1, 100);
            }
        }
    }
}

void Uart_Comm_Process(void)
{
    static uint32_t last_tx = 0;
    static const uint8_t msg[] = "UART6 OK\r\n";

    if (HAL_GetTick() - last_tx >= 1000U) {
        last_tx = HAL_GetTick();

        HAL_UART_Transmit(
            &huart6,
            (uint8_t *)msg,
            sizeof(msg) - 1U,
            100
        );
    }
}
