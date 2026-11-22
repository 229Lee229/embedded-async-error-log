#ifndef ERROR_LOG_PORT_BOARD_H
#define ERROR_LOG_PORT_BOARD_H

#include "gd32f30x.h"
#include <stdint.h>

/* Application-provided ISR-safe tick source. See README.md in this directory. */
uint32_t error_log_board_tick(void);
#define ERROR_LOG_PORT_READ_TICK() error_log_board_tick()

#endif
