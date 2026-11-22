#ifndef ERROR_LOG_GD32F30X_UART_H
#define ERROR_LOG_GD32F30X_UART_H

#include "error_log_uart.h"

/* Initializes dedicated USART1/PA2 and DMA0 channel 6 once after board clock setup. */
void error_log_gd32f30x_uart_init(void);
extern const error_log_uart_driver_t error_log_gd32f30x_uart_driver;

#endif
