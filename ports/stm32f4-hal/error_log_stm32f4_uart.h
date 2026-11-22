#ifndef ERROR_LOG_STM32F4_UART_H
#define ERROR_LOG_STM32F4_UART_H

#include "error_log_uart.h"
#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const error_log_uart_driver_t error_log_stm32f4_uart_driver;

/* Bind one dedicated TX-only UART once, after its UART and DMA initialization. */
HAL_StatusTypeDef error_log_stm32f4_uart_init(UART_HandleTypeDef *uart);

/* Forward the selected UART's HAL callbacks here. These functions run in ISRs. */
void error_log_stm32f4_uart_tx_complete(UART_HandleTypeDef *uart);
void error_log_stm32f4_uart_error(UART_HandleTypeDef *uart);

#ifdef __cplusplus
}
#endif

#endif
