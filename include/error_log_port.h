#ifndef ERROR_LOG_PORT_H
#define ERROR_LOG_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t error_log_port_key_t;

/* Exclude all callers of the FIFO, restoring the exact saved state on exit. */
error_log_port_key_t error_log_port_enter(void);
void error_log_port_exit(error_log_port_key_t key);

/* Called inside the critical section. Read a ready, ISR-safe clock or return 0. */
uint32_t error_log_port_tick(void);

#ifdef __cplusplus
}
#endif

#endif
