#include "error_log.h"

#include <stddef.h>

#if ERROR_LOG_ENABLED
#include "error_log_port.h"

static error_log_record_t queue[ERROR_LOG_QUEUE_CAPACITY];
static uint32_t write_index;
static uint32_t read_index;
static uint32_t next_sequence = 1U;
static error_log_stats_t stats = { .queue_capacity = ERROR_LOG_QUEUE_CAPACITY };

static void increment_count(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        ++(*counter);
    }
}

static uint32_t advance_index(uint32_t index)
{
    ++index;
    return index == ERROR_LOG_QUEUE_CAPACITY ? 0U : index;
}

error_log_result_t error_log_save2(uint32_t file_id, uint32_t line,
    uint32_t error_id, uint32_t param1, uint32_t param2)
{
    const error_log_port_key_t key = error_log_port_enter();
    if (file_id == 0U || line == 0U || error_id == 0U) {
        increment_count(&stats.invalid_argument_count);
        error_log_port_exit(key);
        return ERROR_LOG_INVALID_ARGUMENT;
    }
    if (stats.queued_count == ERROR_LOG_QUEUE_CAPACITY) {
        increment_count(&stats.dropped_full_count);
        error_log_port_exit(key);
        return ERROR_LOG_QUEUE_FULL;
    }

    error_log_record_t *record = &queue[write_index];
    record->sequence = next_sequence;
    record->tick = error_log_port_tick();
    record->file_id = file_id;
    record->line = line;
    record->error_id = error_id;
    record->param1 = param1;
    record->param2 = param2;
    if (++next_sequence == 0U) {
        next_sequence = 1U;
    }

    write_index = advance_index(write_index);
    ++stats.queued_count;
    if (stats.queued_count > stats.peak_queued_count) {
        stats.peak_queued_count = stats.queued_count;
    }
    increment_count(&stats.accepted_count);
    error_log_port_exit(key);
    return ERROR_LOG_OK;
}

error_log_result_t error_log_try_pop(error_log_record_t *record)
{
    if (record == NULL) {
        return ERROR_LOG_INVALID_ARGUMENT;
    }
    const error_log_port_key_t key = error_log_port_enter();
    if (stats.queued_count == 0U) {
        error_log_port_exit(key);
        return ERROR_LOG_QUEUE_EMPTY;
    }

    *record = queue[read_index];
    read_index = advance_index(read_index);
    --stats.queued_count;
    increment_count(&stats.dequeued_count);
    error_log_port_exit(key);
    return ERROR_LOG_OK;
}

error_log_result_t error_log_get_stats(error_log_stats_t *out)
{
    if (out == NULL) {
        return ERROR_LOG_INVALID_ARGUMENT;
    }
    const error_log_port_key_t key = error_log_port_enter();
    *out = stats;
    error_log_port_exit(key);
    return ERROR_LOG_OK;
}

#else

error_log_result_t error_log_save2(uint32_t file_id, uint32_t line,
    uint32_t error_id, uint32_t param1, uint32_t param2)
{
    (void)file_id;
    (void)line;
    (void)error_id;
    (void)param1;
    (void)param2;
    return ERROR_LOG_DISABLED;
}

error_log_result_t error_log_try_pop(error_log_record_t *record)
{
    return record == NULL ? ERROR_LOG_INVALID_ARGUMENT : ERROR_LOG_DISABLED;
}

error_log_result_t error_log_get_stats(error_log_stats_t *out)
{
    if (out == NULL) {
        return ERROR_LOG_INVALID_ARGUMENT;
    }
    *out = (error_log_stats_t){0};
    return ERROR_LOG_DISABLED;
}

#endif
