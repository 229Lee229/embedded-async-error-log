#include "error_log.h"
#include "error_log_uart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    error_log_uart_start_result_t start_result;
    error_log_uart_status_t tx_status;
    size_t start_calls;
    size_t status_calls;
    const uint8_t *borrowed;
    size_t length;
    char snapshot[128];
} fake_uart_t;

static error_log_uart_start_result_t fake_start(void *context,
    const uint8_t *data, size_t length)
{
    fake_uart_t *uart = context;

    CHECK(uart->borrowed == NULL);
    CHECK(length > 0U && length < sizeof uart->snapshot);
    ++uart->start_calls;
    memcpy(uart->snapshot, data, length);
    uart->snapshot[length] = '\0';
    uart->length = length;
    if (uart->start_result == ERROR_LOG_UART_STARTED) {
        uart->borrowed = data;
    }
    return uart->start_result;
}

static error_log_uart_status_t fake_status(void *context)
{
    fake_uart_t *uart = context;

    CHECK(uart->borrowed != NULL);
    CHECK(memcmp(uart->snapshot, uart->borrowed, uart->length) == 0);
    ++uart->status_calls;
    if (uart->tx_status != ERROR_LOG_UART_TX_ACTIVE) {
        uart->borrowed = NULL;
    }
    return uart->tx_status;
}

static void check_sentinel(const error_log_record_t *record)
{
    CHECK(record->sequence == 1U);
    CHECK(record->tick == 2U);
    CHECK(record->file_id == 3U);
    CHECK(record->line == 4U);
    CHECK(record->error_id == 5U);
    CHECK(record->param1 == 6U);
    CHECK(record->param2 == 7U);
}

#if ERROR_LOG_ENABLED

#include "error_log_port.h"

#define LOG_FILE_ID 17U

static error_log_port_key_t fake_irq_mask;
static unsigned enter_calls;
static unsigned exit_calls;
static uint32_t fake_tick;
static unsigned tick_reads;

error_log_port_key_t error_log_port_enter(void)
{
    const error_log_port_key_t previous_mask = fake_irq_mask;

    ++enter_calls;
    fake_irq_mask = 1U;
    return previous_mask;
}

void error_log_port_exit(error_log_port_key_t key)
{
    CHECK(fake_irq_mask == 1U);
    ++exit_calls;
    fake_irq_mask = key;
}

uint32_t error_log_port_tick(void)
{
    CHECK(fake_irq_mask == 1U);
    ++tick_reads;
    return fake_tick;
}

static void test_fifo(void)
{
    error_log_record_t record = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
    error_log_stats_t stats;
    uint32_t kind = 31U;
    uint32_t first = 43U;
    int32_t second = -8;

    CHECK(error_log_try_pop(&record) == ERROR_LOG_QUEUE_EMPTY);
    check_sentinel(&record);
    CHECK(fake_irq_mask == 0U);
    CHECK(error_log_save2(0U, 1U, 1U, 0U, 0U) == ERROR_LOG_INVALID_ARGUMENT);
    CHECK(error_log_save2(1U, 0U, 1U, 0U, 0U) == ERROR_LOG_INVALID_ARGUMENT);
    CHECK(error_log_save2(1U, 1U, 0U, 0U, 0U) == ERROR_LOG_INVALID_ARGUMENT);
    CHECK(error_log_try_pop(NULL) == ERROR_LOG_INVALID_ARGUMENT);
    CHECK(error_log_get_stats(NULL) == ERROR_LOG_INVALID_ARGUMENT);
    CHECK(tick_reads == 0U);

    /* Model a caller that has already disabled interrupts. */
    fake_irq_mask = 1U;
    fake_tick = UINT32_MAX;
    const uint32_t macro_line = __LINE__ + 1U;
    CHECK(LOG_SAVE_ERR2(kind++, first++, second++) == ERROR_LOG_OK);
    CHECK(fake_irq_mask == 1U);
    CHECK(kind == 32U && first == 44U && second == -7);
    kind = 0U;
    first = 0U;
    second = 0;
    fake_tick = 0U;

    CHECK(error_log_get_stats(&stats) == ERROR_LOG_OK);
    CHECK(fake_irq_mask == 1U);
    CHECK(stats.queue_capacity == ERROR_LOG_QUEUE_CAPACITY);
    CHECK(stats.queued_count == 1U && stats.peak_queued_count == 1U);
    CHECK(stats.accepted_count == 1U && stats.invalid_argument_count == 3U);
    CHECK(error_log_try_pop(&record) == ERROR_LOG_OK);
    CHECK(fake_irq_mask == 1U);
    CHECK(record.sequence == 1U && record.tick == UINT32_MAX);
    CHECK(record.file_id == LOG_FILE_ID && record.line == macro_line);
    CHECK(record.error_id == 31U && record.param1 == 43U && record.param2 == UINT32_MAX - 7U);
    fake_irq_mask = 0U;

    for (uint32_t index = 0U; index < ERROR_LOG_QUEUE_CAPACITY; ++index) {
        fake_tick = index;
        CHECK(error_log_save2(3U, 5U, 7U, index, index + 1U) == ERROR_LOG_OK);
        CHECK(fake_irq_mask == 0U);
    }
    CHECK(error_log_save2(3U, 5U, 7U, 0U, 0U) == ERROR_LOG_QUEUE_FULL);
    CHECK(tick_reads == ERROR_LOG_QUEUE_CAPACITY + 1U);

    /* Refill one freed slot to exercise wrapping without reordering the FIFO. */
    CHECK(error_log_try_pop(&record) == ERROR_LOG_OK);
    CHECK(record.sequence == 2U && record.param1 == 0U && record.param2 == 1U);
    fake_tick = ERROR_LOG_QUEUE_CAPACITY;
    CHECK(error_log_save2(3U, 5U, 7U, fake_tick, fake_tick + 1U) == ERROR_LOG_OK);
    for (uint32_t index = 1U; index <= ERROR_LOG_QUEUE_CAPACITY; ++index) {
        CHECK(error_log_try_pop(&record) == ERROR_LOG_OK);
        CHECK(record.sequence == index + 2U && record.tick == index);
        CHECK(record.file_id == 3U && record.line == 5U && record.error_id == 7U);
        CHECK(record.param1 == index && record.param2 == index + 1U);
    }

    CHECK(error_log_get_stats(&stats) == ERROR_LOG_OK);
    CHECK(stats.queue_capacity == ERROR_LOG_QUEUE_CAPACITY && stats.queued_count == 0U);
    CHECK(stats.peak_queued_count == ERROR_LOG_QUEUE_CAPACITY);
    CHECK(stats.accepted_count == ERROR_LOG_QUEUE_CAPACITY + 2U);
    CHECK(stats.dequeued_count == stats.accepted_count);
    CHECK(stats.dropped_full_count == 1U && stats.invalid_argument_count == 3U);
    CHECK(fake_irq_mask == 0U && enter_calls == exit_calls);
}

static void test_uart(void)
{
    error_log_uart_t output = {0};
    fake_uart_t uart = { .start_result = ERROR_LOG_UART_START_BUSY };
    const error_log_uart_driver_t driver = { &uart, fake_start, fake_status };
    const error_log_uart_driver_t invalid_driver = { &uart, fake_start, NULL };
    char expected[128];
    const unsigned long first_sequence = (unsigned long)ERROR_LOG_QUEUE_CAPACITY + 3UL;

    fake_tick = 42U;
    CHECK(error_log_save2(2U, 100U, 3U, 0x1234ABCDU, UINT32_MAX) == ERROR_LOG_OK);
    error_log_uart_poll(NULL, &driver);
    error_log_uart_poll(&output, NULL);
    error_log_uart_poll(&output, &invalid_driver);
    CHECK(uart.start_calls == 0U);
    CHECK(output.length == 0U && !output.active);

    (void)snprintf(expected, sizeof expected,
        "seq=%lu tick=42 file=2 line=100 err=3 p1=0x1234ABCD p2=0xFFFFFFFF\r\n",
        first_sequence);
    error_log_uart_poll(&output, &driver);
    CHECK(uart.start_calls == 1U && uart.status_calls == 0U);
    CHECK(strcmp(uart.snapshot, expected) == 0);
    CHECK(!output.active && output.length == strlen(expected));
    CHECK(output.dropped_count == 0U && uart.borrowed == NULL);

    fake_tick = 43U;
    CHECK(error_log_save2(2U, 101U, 4U, 0U, 1U) == ERROR_LOG_OK);
    error_log_uart_poll(&output, &driver);
    CHECK(uart.start_calls == 2U && strcmp(uart.snapshot, expected) == 0);

    uart.start_result = ERROR_LOG_UART_STARTED;
    error_log_uart_poll(&output, &driver);
    CHECK(uart.start_calls == 3U && strcmp(uart.snapshot, expected) == 0);
    CHECK(output.active && uart.borrowed == (const uint8_t *)output.line);

    uart.tx_status = ERROR_LOG_UART_TX_ACTIVE;
    error_log_uart_poll(&output, &driver);
    CHECK(uart.status_calls == 1U && uart.start_calls == 3U);
    CHECK(output.active && output.sent_count == 0U && output.dropped_count == 0U);
    CHECK(strcmp(output.line, expected) == 0);

    uart.tx_status = ERROR_LOG_UART_TX_COMPLETE;
    uart.start_result = ERROR_LOG_UART_START_ERROR;
    error_log_uart_poll(&output, &driver);
    (void)snprintf(expected, sizeof expected,
        "seq=%lu tick=43 file=2 line=101 err=4 p1=0x00000000 p2=0x00000001\r\n",
        first_sequence + 1UL);
    CHECK(uart.status_calls == 2U && uart.start_calls == 4U);
    CHECK(strcmp(uart.snapshot, expected) == 0);
    CHECK(!output.active && output.length == 0U && uart.borrowed == NULL);
    CHECK(output.sent_count == 1U && output.dropped_count == 1U);

    CHECK(error_log_save2(2U, 102U, 4U, 0U, 1U) == ERROR_LOG_OK);
    uart.start_result = ERROR_LOG_UART_STARTED;
    error_log_uart_poll(&output, &driver);
    CHECK(output.active && uart.borrowed != NULL);

    uart.tx_status = ERROR_LOG_UART_TX_ERROR;
    error_log_uart_poll(&output, &driver);
    CHECK(!output.active && output.length == 0U && uart.borrowed == NULL);
    CHECK(output.sent_count == 1U && output.dropped_count == 2U);

    fake_tick = UINT32_MAX;
    CHECK(error_log_save2(UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, UINT32_MAX) == ERROR_LOG_OK);
    error_log_uart_poll(&output, &driver);
    (void)snprintf(expected, sizeof expected,
        "seq=%lu tick=4294967295 file=4294967295 line=4294967295 err=4294967295 "
        "p1=0xFFFFFFFF p2=0xFFFFFFFF\r\n", first_sequence + 3UL);
    CHECK(strcmp(uart.snapshot, expected) == 0);
    CHECK(output.active && uart.length == strlen(expected));
    uart.tx_status = ERROR_LOG_UART_TX_COMPLETE;
    error_log_uart_poll(&output, &driver);
    error_log_uart_poll(&output, &driver);
    CHECK(uart.start_calls == 6U && uart.status_calls == 4U);
    CHECK(!output.active && output.length == 0U && uart.borrowed == NULL);
    CHECK(output.sent_count == 2U && output.dropped_count == 2U);
    CHECK(fake_irq_mask == 0U && enter_calls == exit_calls);
}

#else

static void test_disabled(void)
{
    error_log_record_t record = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
    error_log_stats_t stats = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
    error_log_uart_t output = {
        .line = "sentinel", .length = 8U, .active = true,
        .sent_count = 5U, .dropped_count = 6U
    };
    fake_uart_t uart = {0};
    const error_log_uart_driver_t driver = { &uart, fake_start, fake_status };
    unsigned arg1 = 1U;
    unsigned arg2 = 2U;

    /* The disabled macro needs neither a file ID nor evaluable arguments. */
    CHECK(LOG_SAVE_ERR2(undefined_error_id, ++arg1, ++arg2) == ERROR_LOG_DISABLED);
    CHECK(arg1 == 1U && arg2 == 2U);
    CHECK(error_log_save2(1U, 1U, 1U, 0U, 0U) == ERROR_LOG_DISABLED);
    CHECK(error_log_try_pop(&record) == ERROR_LOG_DISABLED);
    check_sentinel(&record);
    CHECK(error_log_try_pop(NULL) == ERROR_LOG_INVALID_ARGUMENT);
    CHECK(error_log_get_stats(NULL) == ERROR_LOG_INVALID_ARGUMENT);
    CHECK(error_log_get_stats(&stats) == ERROR_LOG_DISABLED);
    CHECK(stats.queue_capacity == 0U && stats.queued_count == 0U);
    CHECK(stats.peak_queued_count == 0U && stats.accepted_count == 0U);
    CHECK(stats.dequeued_count == 0U && stats.dropped_full_count == 0U);
    CHECK(stats.invalid_argument_count == 0U);

    error_log_uart_poll(&output, &driver);
    CHECK(uart.start_calls == 0U && uart.status_calls == 0U);
    CHECK(output.active && output.length == 8U && strcmp(output.line, "sentinel") == 0);
    CHECK(output.sent_count == 5U && output.dropped_count == 6U);
}

#endif

int main(void)
{
#if ERROR_LOG_ENABLED
    test_fifo();
    test_uart();
    printf("PASS: logging enabled, queue capacity %u\n", (unsigned)ERROR_LOG_QUEUE_CAPACITY);
#else
    test_disabled();
    puts("PASS: logging disabled");
#endif
    return EXIT_SUCCESS;
}
