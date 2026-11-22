#include "error_log_port.h"
#include "error_log_port_board.h"

error_log_port_key_t error_log_port_enter(void)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return (error_log_port_key_t)primask;
}

void error_log_port_exit(error_log_port_key_t key)
{
    __DMB();
    __set_PRIMASK((uint32_t)key);
}

uint32_t error_log_port_tick(void)
{
    return (uint32_t)ERROR_LOG_PORT_READ_TICK();
}
