#include "error_log_flash.h"

#if ERROR_LOG_ENABLED && ERROR_LOG_FLASH_ENABLED

#include <stdio.h>
#include <string.h>

#define SLOT_MAGIC UINT32_C(0x314C4645)        /* EFL1 */
#define SLOT_COMMIT UINT32_C(0x314D4F43)       /* COM1 */
#define SLOT_FIRMWARE_OFFSET 32U
#define SLOT_CRC_OFFSET 36U
#define SLOT_RESERVED_OFFSET 40U
#define SLOT_COMMIT_OFFSET 60U

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static bool is_erased(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static uint32_t slot_crc32(const uint8_t *bytes)
{
    uint32_t crc = UINT32_MAX;

    for (size_t i = 0; i < SLOT_CRC_OFFSET; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ UINT32_C(0xEDB88320);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ UINT32_MAX;
}

static bool slot_is_valid(const uint8_t *slot)
{
    return read_le32(slot) == SLOT_MAGIC
        && read_le32(slot + SLOT_COMMIT_OFFSET) == SLOT_COMMIT
        && read_le32(slot + 12U) != 0U       /* file_id */
        && read_le32(slot + 16U) != 0U       /* line */
        && read_le32(slot + 20U) != 0U       /* error_id */
        && is_erased(slot + SLOT_RESERVED_OFFSET, SLOT_COMMIT_OFFSET - SLOT_RESERVED_OFFSET)
        && read_le32(slot + SLOT_CRC_OFFSET) == slot_crc32(slot);
}

static void encode_slot(uint8_t *slot, const error_log_record_t *record, uint32_t firmware_id)
{
    memset(slot, 0xFF, ERROR_LOG_FLASH_SLOT_SIZE);
    write_le32(slot, SLOT_MAGIC);
    write_le32(slot + 4U, record->sequence);
    write_le32(slot + 8U, record->tick);
    write_le32(slot + 12U, record->file_id);
    write_le32(slot + 16U, record->line);
    write_le32(slot + 20U, record->error_id);
    write_le32(slot + 24U, record->param1);
    write_le32(slot + 28U, record->param2);
    write_le32(slot + SLOT_FIRMWARE_OFFSET, firmware_id);
    write_le32(slot + SLOT_CRC_OFFSET, slot_crc32(slot));
    write_le32(slot + SLOT_COMMIT_OFFSET, SLOT_COMMIT);
}

static void decode_slot(error_log_flash_entry_t *entry, const uint8_t *slot, uint32_t index)
{
    entry->slot = index + 1U;
    entry->firmware_id = read_le32(slot + SLOT_FIRMWARE_OFFSET);
    entry->record.sequence = read_le32(slot + 4U);
    entry->record.tick = read_le32(slot + 8U);
    entry->record.file_id = read_le32(slot + 12U);
    entry->record.line = read_le32(slot + 16U);
    entry->record.error_id = read_le32(slot + 20U);
    entry->record.param1 = read_le32(slot + 24U);
    entry->record.param2 = read_le32(slot + 28U);
}

static bool read_slot(const error_log_flash_t *flash, uint32_t index, uint8_t *slot)
{
    return flash->driver.read(flash->driver.context, index * ERROR_LOG_FLASH_SLOT_SIZE,
        slot, ERROR_LOG_FLASH_SLOT_SIZE);
}

error_log_flash_result_t error_log_flash_init(error_log_flash_t *flash,
    const error_log_flash_driver_t *driver, uint32_t firmware_id)
{
    uint8_t slot[ERROR_LOG_FLASH_SLOT_SIZE];
    uint32_t occupied = 0;
    uint32_t valid = 0;
    uint32_t bad = 0;

    if (flash == NULL) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }
    flash->ready = false;
    if (driver == NULL || driver->read == NULL || driver->program == NULL
        || driver->region_size < ERROR_LOG_FLASH_SLOT_SIZE
        || driver->region_size % ERROR_LOG_FLASH_SLOT_SIZE != 0U) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }

    flash->driver = *driver;
    flash->firmware_id = firmware_id;
    flash->capacity = driver->region_size / ERROR_LOG_FLASH_SLOT_SIZE;
    flash->next_slot = 0;
    flash->boot_occupied = 0;
    flash->boot_valid = 0;
    flash->boot_bad = 0;

    for (uint32_t index = 0; index < flash->capacity; ++index) {
        if (!read_slot(flash, index, slot)) {
            return ERROR_LOG_FLASH_IO_ERROR;
        }
        if (is_erased(slot, sizeof slot)) {
            continue;
        }
        occupied = index + 1U;
        if (slot_is_valid(slot)) {
            ++valid;
        } else {
            ++bad;
        }
    }

    flash->next_slot = occupied;
    flash->boot_occupied = occupied;
    flash->boot_valid = valid;
    flash->boot_bad = bad;
    flash->ready = true;
    return ERROR_LOG_FLASH_OK;
}

error_log_flash_result_t error_log_flash_append(error_log_flash_t *flash,
    const error_log_record_t *record)
{
    uint8_t encoded[ERROR_LOG_FLASH_SLOT_SIZE];
    uint8_t observed[ERROR_LOG_FLASH_SLOT_SIZE];

    if (flash == NULL || record == NULL || record->file_id == 0U
        || record->line == 0U || record->error_id == 0U) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }
    if (!flash->ready) {
        return ERROR_LOG_FLASH_NOT_READY;
    }
    if (flash->next_slot >= flash->capacity) {
        return ERROR_LOG_FLASH_FULL;
    }

    const uint32_t index = flash->next_slot++;
    const uint32_t offset = index * ERROR_LOG_FLASH_SLOT_SIZE;
    const error_log_flash_driver_t *driver = &flash->driver;

    if (!read_slot(flash, index, observed) || !is_erased(observed, sizeof observed)) {
        return ERROR_LOG_FLASH_IO_ERROR;
    }
    encode_slot(encoded, record, flash->firmware_id);

    if (!driver->program(driver->context, offset, encoded, ERROR_LOG_FLASH_BLOCK_SIZE)
        || !read_slot(flash, index, observed)
        || memcmp(encoded, observed, ERROR_LOG_FLASH_BLOCK_SIZE) != 0
        || !is_erased(observed + ERROR_LOG_FLASH_BLOCK_SIZE, ERROR_LOG_FLASH_BLOCK_SIZE)) {
        return ERROR_LOG_FLASH_IO_ERROR;
    }

    if (!driver->program(driver->context, offset + ERROR_LOG_FLASH_BLOCK_SIZE,
            encoded + ERROR_LOG_FLASH_BLOCK_SIZE, ERROR_LOG_FLASH_BLOCK_SIZE)
        || !read_slot(flash, index, observed)
        || memcmp(encoded, observed, sizeof encoded) != 0) {
        return ERROR_LOG_FLASH_IO_ERROR;
    }

    return ERROR_LOG_FLASH_OK;
}

error_log_flash_result_t error_log_flash_read_next(const error_log_flash_t *flash,
    uint32_t *cursor, error_log_flash_entry_t *entry)
{
    uint8_t slot[ERROR_LOG_FLASH_SLOT_SIZE];

    if (flash == NULL || cursor == NULL || entry == NULL) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }
    if (!flash->ready) {
        return ERROR_LOG_FLASH_NOT_READY;
    }

    for (uint32_t index = *cursor; index < flash->boot_occupied; ++index) {
        if (!read_slot(flash, index, slot)) {
            return ERROR_LOG_FLASH_IO_ERROR;
        }
        if (slot_is_valid(slot)) {
            decode_slot(entry, slot, index);
            *cursor = index + 1U;
            return ERROR_LOG_FLASH_OK;
        }
    }

    *cursor = flash->boot_occupied;
    return ERROR_LOG_FLASH_EMPTY;
}

error_log_flash_result_t error_log_flash_poll(error_log_flash_t *flash)
{
    error_log_record_t record;

    if (flash == NULL) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }
    if (!flash->ready) {
        return ERROR_LOG_FLASH_NOT_READY;
    }
    if (error_log_try_pop(&record) != ERROR_LOG_OK) {
        return ERROR_LOG_FLASH_EMPTY;
    }
    return error_log_flash_append(flash, &record);
}

error_log_flash_result_t error_log_flash_replay(const error_log_flash_t *flash,
    bool (*write)(void *context, const char *line, size_t length), void *context)
{
    char line[160];
    uint32_t cursor = 0;
    uint32_t shown = 0;
    error_log_flash_entry_t entry;
    int length;

    if (flash == NULL || write == NULL) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }
    if (!flash->ready) {
        return ERROR_LOG_FLASH_NOT_READY;
    }

    length = snprintf(line, sizeof line, "[ErrLog] BEGIN valid=%lu occupied=%lu/%lu bad=%lu\r\n",
        (unsigned long)flash->boot_valid, (unsigned long)flash->boot_occupied,
        (unsigned long)flash->capacity, (unsigned long)flash->boot_bad);
    if (length < 0 || (size_t)length >= sizeof line || !write(context, line, (size_t)length)) {
        return ERROR_LOG_FLASH_IO_ERROR;
    }

    for (;;) {
        const error_log_flash_result_t result = error_log_flash_read_next(flash, &cursor, &entry);
        if (result == ERROR_LOG_FLASH_EMPTY) {
            break;
        }
        if (result != ERROR_LOG_FLASH_OK) {
            return result;
        }

        length = snprintf(line, sizeof line,
            "[ErrLog] slot=%lu fw=0x%08lX seq=%lu tick=%lu file=%lu line=%lu err=%lu p1=0x%08lX p2=0x%08lX\r\n",
            (unsigned long)entry.slot, (unsigned long)entry.firmware_id,
            (unsigned long)entry.record.sequence, (unsigned long)entry.record.tick,
            (unsigned long)entry.record.file_id, (unsigned long)entry.record.line,
            (unsigned long)entry.record.error_id, (unsigned long)entry.record.param1,
            (unsigned long)entry.record.param2);
        if (length < 0 || (size_t)length >= sizeof line || !write(context, line, (size_t)length)) {
            return ERROR_LOG_FLASH_IO_ERROR;
        }
        ++shown;
    }

    length = snprintf(line, sizeof line, "[ErrLog] END shown=%lu full=%u\r\n",
        (unsigned long)shown, flash->boot_occupied == flash->capacity ? 1U : 0U);
    if (length < 0 || (size_t)length >= sizeof line || !write(context, line, (size_t)length)) {
        return ERROR_LOG_FLASH_IO_ERROR;
    }
    return ERROR_LOG_FLASH_OK;
}

#endif
