#ifndef ERROR_LOG_FLASH_H
#define ERROR_LOG_FLASH_H

#include "error_log.h"

#ifndef ERROR_LOG_FLASH_ENABLED
#define ERROR_LOG_FLASH_ENABLED 0
#endif
#if ERROR_LOG_FLASH_ENABLED != 0 && ERROR_LOG_FLASH_ENABLED != 1
#error "ERROR_LOG_FLASH_ENABLED must be 0 or 1"
#endif

#if ERROR_LOG_ENABLED && ERROR_LOG_FLASH_ENABLED
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ERROR_LOG_FLASH_SLOT_SIZE 64U
#define ERROR_LOG_FLASH_BLOCK_SIZE 32U

typedef enum {
    ERROR_LOG_FLASH_OK,
    ERROR_LOG_FLASH_EMPTY,
    ERROR_LOG_FLASH_FULL,
    ERROR_LOG_FLASH_INVALID_ARGUMENT,
    ERROR_LOG_FLASH_IO_ERROR,
    ERROR_LOG_FLASH_NOT_READY
} error_log_flash_result_t;

/*
 * Relative offsets into an exclusively reserved region with erased bytes 0xFF.
 * read() copies a full slot. program() synchronously programs one aligned
 * 32-byte block using the target's programming units, then releases the buffer.
 * Call this module from one startup or background context with interrupts
 * available, never from an ISR. Serialize shared Flash controller access
 * inside the platform callbacks.
 */
typedef struct {
    void *context;
    uint32_t region_size;
    bool (*read)(void *context, uint32_t offset, uint8_t *data, size_t length);
    bool (*program)(void *context, uint32_t offset, const uint8_t *data, size_t length);
} error_log_flash_driver_t;

typedef struct {
    error_log_flash_driver_t driver;
    uint32_t firmware_id;
    uint32_t capacity;       /* Slots in the reserved region. */
    uint32_t next_slot;      /* Zero-based runtime append position. */
    uint32_t boot_occupied;  /* One past the last non-erased slot at init. */
    uint32_t boot_valid;     /* Valid records at init. */
    uint32_t boot_bad;       /* Non-erased records that failed validation at init. */
    bool ready;
} error_log_flash_t;

typedef struct {
    uint32_t slot;  /* One-based physical slot number. */
    uint32_t firmware_id;
    error_log_record_t record;
} error_log_flash_entry_t;

/* Scan the reserved region without changing it. An unsuccessful scan disables append. */
error_log_flash_result_t error_log_flash_init(error_log_flash_t *flash,
    const error_log_flash_driver_t *driver, uint32_t firmware_id);

/* Append one record. After selecting a slot, any I/O failure skips it this boot. */
error_log_flash_result_t error_log_flash_append(error_log_flash_t *flash,
    const error_log_record_t *record);

/*
 * Read the next valid record from the boot snapshot. Start with *cursor = 0.
 * EMPTY means end of snapshot. Entry is only changed on OK. On I/O failure
 * the cursor is unchanged so the caller can retry the same part of the scan.
 */
error_log_flash_result_t error_log_flash_read_next(const error_log_flash_t *flash,
    uint32_t *cursor, error_log_flash_entry_t *entry);

/*
 * In Flash mode, call from one background worker instead of error_log_uart_poll().
 * Takes at most one queued record. FULL and IO_ERROR discard that record.
 */
error_log_flash_result_t error_log_flash_poll(error_log_flash_t *flash);

/*
 * Replay the boot snapshot without changing Flash. The writer runs synchronously
 * with a caller-defined bound and must release the line before returning.
 * Return false from the writer on timeout or failure.
 */
error_log_flash_result_t error_log_flash_replay(const error_log_flash_t *flash,
    bool (*write)(void *context, const char *line, size_t length), void *context);

#ifdef __cplusplus
}
#endif
#endif

#endif
