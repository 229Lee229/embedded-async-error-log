#ifndef ERROR_LOG_GD32F30X_FLASH_H
#define ERROR_LOG_GD32F30X_FLASH_H

#include "error_log_flash.h"

#if ERROR_LOG_ENABLED && ERROR_LOG_FLASH_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t base;
    uint32_t size;
    bool (*lock)(void *context);
    void (*unlock)(void *context);
    void *lock_context;
} error_log_gd32f30x_flash_region_t;

/*
 * Supply whole, reserved 2 KiB pages in Bank0. Keep region unchanged and
 * alive while the driver uses it. The adapter never erases Flash. Calls
 * belong to startup or the one log worker, outside interrupt context.
 *
 * If any other code uses the FMC, use the same controller lock for every
 * user. Omit both lock callbacks only if the log is the sole FMC owner.
 */
bool error_log_gd32f30x_flash_make_driver(error_log_flash_driver_t *driver,
    error_log_gd32f30x_flash_region_t *region);

#ifdef __cplusplus
}
#endif

#endif
#endif
