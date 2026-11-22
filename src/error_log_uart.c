#include "error_log_uart.h"
#include "error_log.h"

#if ERROR_LOG_ENABLED
#include <stdio.h>

static void increment_count(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        ++(*counter);
    }
}

void error_log_uart_poll(error_log_uart_t *output, const error_log_uart_driver_t *driver)
{
    if (output == NULL || driver == NULL || driver->start == NULL || driver->status == NULL) {
        return;
    }

    if (output->active) {
        const error_log_uart_status_t status = driver->status(driver->context);
        if (status == ERROR_LOG_UART_TX_ACTIVE) {
            return;
        }
        if (status == ERROR_LOG_UART_TX_COMPLETE) {
            increment_count(&output->sent_count);
        } else {
            increment_count(&output->dropped_count);
        }
        output->active = false;
        output->length = 0U;
    }

    if (output->length == 0U) {
        error_log_record_t record;
        if (error_log_try_pop(&record) != ERROR_LOG_OK) {
            return;
        }
        const int length = snprintf(output->line, sizeof output->line,
            "seq=%lu tick=%lu file=%lu line=%lu err=%lu p1=0x%08lX p2=0x%08lX\r\n",
            (unsigned long)record.sequence, (unsigned long)record.tick,
            (unsigned long)record.file_id, (unsigned long)record.line,
            (unsigned long)record.error_id, (unsigned long)record.param1,
            (unsigned long)record.param2);
        if (length < 0 || (size_t)length >= sizeof output->line) {
            increment_count(&output->dropped_count);
            return;
        }
        output->length = (size_t)length;
    }

    switch (driver->start(driver->context, (const uint8_t *)output->line, output->length)) {
    case ERROR_LOG_UART_STARTED:
        output->active = true;
        break;
    case ERROR_LOG_UART_START_BUSY:
        break;
    case ERROR_LOG_UART_START_ERROR:
    default:
        increment_count(&output->dropped_count);
        output->length = 0U;
        break;
    }
}

#else

void error_log_uart_poll(error_log_uart_t *output, const error_log_uart_driver_t *driver)
{
    (void)output;
    (void)driver;
}

#endif
