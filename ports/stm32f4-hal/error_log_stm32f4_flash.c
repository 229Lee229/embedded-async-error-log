#include "error_log_stm32f4_flash.h"

#if ERROR_LOG_ENABLED && ERROR_LOG_FLASH_ENABLED

#include "stm32f4xx_hal.h"

#if !defined(STM32F411xE)
#error "This Flash port requires the CubeF4 STM32F411xE device macro (xC and xE)"
#endif
#if !defined(HAL_FLASH_MODULE_ENABLED)
#error "Enable the CubeF4 HAL Flash module for the STM32F411 Flash port"
#endif

/* RM0383 sector boundaries, relative to FLASH_BASE. */
static bool is_sector_boundary(uint32_t offset)
{
    switch (offset) {
    case 0x00000U:
    case 0x04000U:
    case 0x08000U:
    case 0x0C000U:
    case 0x10000U:
    case 0x20000U:
    case 0x40000U:
    case 0x60000U:
    case 0x80000U:
        return true;
    default:
        return false;
    }
}

static bool inside_region(const error_log_stm32f4_flash_region_t *region,
    uint32_t offset, size_t length)
{
    return length > 0U && offset <= region->size &&
        length <= (size_t)(region->size - offset);
}

static bool acquire_region(const error_log_stm32f4_flash_region_t *region)
{
    return __get_IPSR() == 0U &&
        (region->lock == NULL || region->lock(region->lock_context));
}

static void release_region(const error_log_stm32f4_flash_region_t *region)
{
    if (region->unlock != NULL) {
        region->unlock(region->lock_context);
    }
}

static bool read_region(void *context, uint32_t offset, uint8_t *data, size_t length)
{
    const error_log_stm32f4_flash_region_t *region = context;

    if (data == NULL || !inside_region(region, offset, length) || !acquire_region(region)) {
        return false;
    }

    bool read = __HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY) == 0U;
    if (read) {
        const volatile uint8_t *source = (const volatile uint8_t *)(uintptr_t)(region->base + offset);
        for (size_t i = 0; i < length; ++i) {
            data[i] = source[i];
        }
    }
    release_region(region);
    return read;
}

static bool program_erased_block(uint32_t address, const uint8_t *data, size_t length)
{
    const volatile uint8_t *destination = (const volatile uint8_t *)(uintptr_t)address;

    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY) != 0U) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (destination[i] != UINT8_MAX) {
            return false;
        }
    }

    const bool relock = (FLASH->CR & FLASH_CR_LOCK) != 0U;
    if (relock && HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }

    bool programmed = true;
    /* RM0383 allows byte programming. Leave bytes that are already FF alone. */
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != UINT8_MAX &&
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE, address + (uint32_t)i, data[i]) != HAL_OK) {
            programmed = false;
            break;
        }
    }
    if (relock && HAL_FLASH_Lock() != HAL_OK) {
        return false;
    }
    if (!programmed) {
        return false;
    }

    /* On F411, programming updates matching entries in the Flash data cache. */
    for (size_t i = 0; i < length; ++i) {
        if (destination[i] != data[i]) {
            return false;
        }
    }
    return true;
}

static bool program_region(void *context, uint32_t offset, const uint8_t *data, size_t length)
{
    const error_log_stm32f4_flash_region_t *region = context;

    if (data == NULL || length != ERROR_LOG_FLASH_BLOCK_SIZE ||
        offset % ERROR_LOG_FLASH_BLOCK_SIZE != 0U ||
        !inside_region(region, offset, length) || !acquire_region(region)) {
        return false;
    }

    const bool programmed = program_erased_block(region->base + offset, data, length);
    release_region(region);
    return programmed;
}

bool error_log_stm32f4_flash_make_driver(error_log_flash_driver_t *driver,
    error_log_stm32f4_flash_region_t *region)
{
    if (driver == NULL || region == NULL || region->base < FLASH_BASE || region->size == 0U ||
        ((region->lock == NULL) != (region->unlock == NULL))) {
        return false;
    }

    /* The CubeF4 header uses one device macro for both Flash sizes. */
    const uint32_t size_kib = *(const volatile uint16_t *)(uintptr_t)FLASHSIZE_BASE;
    if (size_kib != 256U && size_kib != 512U) {
        return false;
    }
    const uint32_t flash_size = size_kib * 1024U;
    const uint32_t first = region->base - FLASH_BASE;
    if (first >= flash_size || region->size > flash_size - first ||
        !is_sector_boundary(first) || !is_sector_boundary(first + region->size)) {
        return false;
    }

    *driver = (error_log_flash_driver_t){
        .context = region,
        .region_size = region->size,
        .read = read_region,
        .program = program_region,
    };
    return true;
}

#endif
