#ifndef ERROR_LOG_STM32F4_FLASH_H
#define ERROR_LOG_STM32F4_FLASH_H

#include "error_log_flash.h"

#if ERROR_LOG_ENABLED && ERROR_LOG_FLASH_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t base;
    uint32_t size;
    /* Both callbacks or neither. Share this lock with other Flash users. */
    bool (*lock)(void *context);
    void (*unlock)(void *context);
    void *lock_context;
} error_log_stm32f4_flash_region_t;

/*
 * STM32F411xC/xE with CubeF4 HAL. The region must be dedicated to logging,
 * excluded from all firmware images, and consist of complete sectors.
 * Keep region and any lock_context alive and fixed while using the driver.
 * This port never erases. Run its callbacks only from thread/main context.
 * Byte programming requires VDD between 1.7 V and 3.6 V per device datasheet.
 * Without lock callbacks, the application must serialize all Flash access.
 */
bool error_log_stm32f4_flash_make_driver(error_log_flash_driver_t *driver,
    error_log_stm32f4_flash_region_t *region);

#ifdef __cplusplus
}
#endif

#endif
#endif
