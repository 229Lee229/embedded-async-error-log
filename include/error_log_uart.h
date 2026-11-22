#ifndef ERROR_LOG_UART_H
#define ERROR_LOG_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ERROR_LOG_UART_STARTED,
    ERROR_LOG_UART_START_BUSY,
    ERROR_LOG_UART_START_ERROR
} error_log_uart_start_result_t;

typedef enum {
    ERROR_LOG_UART_TX_ACTIVE,
    ERROR_LOG_UART_TX_COMPLETE,
    ERROR_LOG_UART_TX_ERROR
} error_log_uart_status_t;

typedef struct {
    void *context;

    /* BUSY and ERROR leave bytes with the caller. STARTED borrows the data. */
    error_log_uart_start_result_t (*start)(void *context, const uint8_t *data, size_t length);

    /* COMPLETE or ERROR releases the data. COMPLETE also means UART wire idle. */
    error_log_uart_status_t (*status)(void *context);
} error_log_uart_driver_t;

typedef struct {
    char line[128];
    size_t length;
    bool active;
    uint32_t sent_count;
    uint32_t dropped_count;
} error_log_uart_t;

/*
 * Zero-initialize output before its first use. Keep its address and driver
 * fixed until all transfers complete. Call regularly from one main loop or
 * worker only. Stats are owned by that caller. Adapters must not wait for TX.
 */
void error_log_uart_poll(error_log_uart_t *output, const error_log_uart_driver_t *driver);

#ifdef __cplusplus
}
#endif

#endif
