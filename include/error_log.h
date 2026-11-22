#ifndef ERROR_LOG_H
#define ERROR_LOG_H

#include <stdint.h>

#ifndef ERROR_LOG_ENABLED
#define ERROR_LOG_ENABLED 1
#endif
#ifndef ERROR_LOG_QUEUE_CAPACITY
#define ERROR_LOG_QUEUE_CAPACITY 16U
#endif
#if ERROR_LOG_ENABLED != 0 && ERROR_LOG_ENABLED != 1
#error "ERROR_LOG_ENABLED must be 0 or 1"
#endif
#if ERROR_LOG_QUEUE_CAPACITY < 1 || ERROR_LOG_QUEUE_CAPACITY > 256
#error "ERROR_LOG_QUEUE_CAPACITY must be 1..256"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ERROR_LOG_OK = 0,
    ERROR_LOG_QUEUE_EMPTY = 1,
    ERROR_LOG_INVALID_ARGUMENT = -1,
    ERROR_LOG_QUEUE_FULL = -2,
    ERROR_LOG_DISABLED = -3
} error_log_result_t;

/* Numeric values copied into RAM. This structure is not a storage format. */
typedef struct {
    uint32_t sequence;
    uint32_t tick;
    uint32_t file_id;
    uint32_t line;
    uint32_t error_id;
    uint32_t param1;
    uint32_t param2;
} error_log_record_t;

typedef struct {
    uint32_t queue_capacity;
    uint32_t queued_count;
    uint32_t peak_queued_count;
    uint32_t accepted_count;
    uint32_t dequeued_count;
    uint32_t dropped_full_count;
    uint32_t invalid_argument_count;
} error_log_stats_t;

/* OK means queued in RAM. A full FIFO retains previously accepted records. */
error_log_result_t error_log_save2(uint32_t file_id, uint32_t line,
    uint32_t error_id, uint32_t param1, uint32_t param2);

/* The single consumer owns the record on OK. Otherwise output is unchanged. */
error_log_result_t error_log_try_pop(error_log_record_t *record);

/* Coherent snapshot. Lifetime counters saturate at UINT32_MAX. */
error_log_result_t error_log_get_stats(error_log_stats_t *stats);

#ifdef __cplusplus
}
#endif

#if ERROR_LOG_ENABLED
#define LOG_SAVE_ERR2(error_id, param1, param2) \
    error_log_save2((uint32_t)(LOG_FILE_ID), (uint32_t)__LINE__, \
        (uint32_t)(error_id), (uint32_t)(param1), (uint32_t)(param2))
#else
/* Disabled submissions do not evaluate any argument. */
#define LOG_SAVE_ERR2(error_id, param1, param2) (ERROR_LOG_DISABLED)
#endif

#endif
