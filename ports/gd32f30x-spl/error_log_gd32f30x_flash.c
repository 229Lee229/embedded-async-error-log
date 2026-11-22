#include "error_log_gd32f30x_flash.h"

#if ERROR_LOG_ENABLED && ERROR_LOG_FLASH_ENABLED
#include "gd32f30x_fmc.h"

#define LOG_FLASH_BANK0_BASE UINT32_C(0x08000000)
#define LOG_FLASH_PAGE_SIZE  UINT32_C(2048)

static bool region_contains(const error_log_gd32f30x_flash_region_t *region,
    uint32_t offset, size_t length)
{
    return offset <= region->size && length <= region->size - offset;
}

static bool enter_region(const error_log_gd32f30x_flash_region_t *region)
{
    return __get_IPSR() == 0U &&
        (region->lock == NULL || region->lock(region->lock_context));
}

static void leave_region(const error_log_gd32f30x_flash_region_t *region)
{
    if (region->unlock != NULL) {
        region->unlock(region->lock_context);
    }
}

static bool read_region(void *context, uint32_t offset, uint8_t *data, size_t length)
{
    const error_log_gd32f30x_flash_region_t *region = context;
    if (data == NULL || !region_contains(region, offset, length) ||
        !enter_region(region)) {
        return false;
    }

    const bool ready = fmc_flag_get(FMC_FLAG_BANK0_BUSY) == RESET;
    if (ready) {
        const uint32_t address = region->base + offset;
        for (size_t i = 0U; i < length; ++i) {
            data[i] = REG8(address + (uint32_t)i);
        }
    }
    leave_region(region);
    return ready;
}

/* The whole chunk must be erased before any word is programmed. */
static bool chunk_is_erased(uint32_t address)
{
    for (uint32_t i = 0U; i < ERROR_LOG_FLASH_BLOCK_SIZE; i += 4U) {
        if (REG32(address + i) != UINT32_MAX) {
            return false;
        }
    }
    return true;
}

static uint32_t read_le_word(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static bool program_erased_chunk(uint32_t address, const uint8_t *data)
{
    if (!chunk_is_erased(address)) {
        return false;
    }

    fmc_bank0_unlock();
    if ((FMC_CTL0 & FMC_CTL0_LK) != 0U) {
        return false;
    }
    fmc_flag_clear(FMC_FLAG_BANK0_END);
    fmc_flag_clear(FMC_FLAG_BANK0_PGERR);
    fmc_flag_clear(FMC_FLAG_BANK0_WPERR);

    bool ok = true;
    for (uint32_t i = 0U; ok && i < ERROR_LOG_FLASH_BLOCK_SIZE; i += 4U) {
        const uint32_t word = read_le_word(&data[i]);
        if (word != UINT32_MAX) {
            ok = fmc_word_program(address + i, word) == FMC_READY &&
                REG32(address + i) == word;
        }
    }

    fmc_bank0_lock();
    return ok;
}

static bool program_region(void *context, uint32_t offset, const uint8_t *data, size_t length)
{
    const error_log_gd32f30x_flash_region_t *region = context;
    if (data == NULL || length != ERROR_LOG_FLASH_BLOCK_SIZE ||
        offset % ERROR_LOG_FLASH_BLOCK_SIZE != 0U ||
        !region_contains(region, offset, length) || !enter_region(region)) {
        return false;
    }

    const bool ok = fmc_flag_get(FMC_FLAG_BANK0_BUSY) == RESET &&
        program_erased_chunk(region->base + offset, data);
    leave_region(region);
    return ok;
}

bool error_log_gd32f30x_flash_make_driver(error_log_flash_driver_t *driver,
    error_log_gd32f30x_flash_region_t *region)
{
    if (driver == NULL || region == NULL || region->base < LOG_FLASH_BANK0_BASE ||
        region->base % LOG_FLASH_PAGE_SIZE != 0U || region->size == 0U ||
        region->size % LOG_FLASH_PAGE_SIZE != 0U ||
        (region->lock == NULL) != (region->unlock == NULL)) {
        return false;
    }

    /* The factory signature is in KiB and Bank0 spans at most 512 KiB. */
    const uint32_t installed_kib = FMC_SIZE;
    if (installed_kib == 0U || installed_kib == UINT16_MAX) {
        return false;
    }
    const uint32_t bank0_kib = installed_kib < FMC_BANK0_SIZE ? installed_kib : FMC_BANK0_SIZE;
    const uint32_t bank0_size = bank0_kib * UINT32_C(1024);
    const uint32_t base_offset = region->base - LOG_FLASH_BANK0_BASE;
    if (base_offset > bank0_size || region->size > bank0_size - base_offset) {
        return false;
    }

    *driver = (error_log_flash_driver_t){
        .context = region,
        .region_size = region->size,
        .read = read_region,
        .program = program_region
    };
    return true;
}
#endif
