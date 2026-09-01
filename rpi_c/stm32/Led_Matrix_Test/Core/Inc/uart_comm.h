//uart_comm.h
#ifndef __UART_COMM_H
#define __UART_COMM_H

#include "main.h"
extern uint8_t rx_buffer[256];
void Uart_Comm_RxCallback(uint16_t size);
void Uart_Comm_Process();
#endif
