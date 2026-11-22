#include "error_log_gd32f30x_uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gd32f30x.h"
#include "gd32f30x_dma.h"
#include "gd32f30x_gpio.h"
#include "gd32f30x_rcu.h"
#include "gd32f30x_usart.h"

#define LOG_USART      USART1
#define LOG_DMA        DMA0
#define LOG_DMA_CH     DMA_CH6
#define LOG_BAUD       115200U

typedef enum {
    GD32_TX_IDLE,
    GD32_TX_DMA,
    GD32_TX_DRAIN
} gd32_tx_state_t;

/* Only the logging consumer calls start and status. No ISR shares this state. */
static gd32_tx_state_t tx_state;
static bool initialized;

void error_log_gd32f30x_uart_init(void)
{
    dma_parameter_struct config;

    if (initialized) {
        return;
    }

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_AF);
    rcu_periph_clock_enable(RCU_USART1);
    rcu_periph_clock_enable(RCU_DMA0);

    gpio_pin_remap_config(GPIO_USART1_REMAP, DISABLE);
    gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_2);

    usart_deinit(LOG_USART);
    usart_baudrate_set(LOG_USART, LOG_BAUD);
    usart_word_length_set(LOG_USART, USART_WL_8BIT);
    usart_stop_bit_set(LOG_USART, USART_STB_1BIT);
    usart_parity_config(LOG_USART, USART_PM_NONE);
    usart_hardware_flow_rts_config(LOG_USART, USART_RTS_DISABLE);
    usart_hardware_flow_cts_config(LOG_USART, USART_CTS_DISABLE);
    usart_receive_config(LOG_USART, USART_RECEIVE_DISABLE);
    usart_transmit_config(LOG_USART, USART_TRANSMIT_ENABLE);

    dma_deinit(LOG_DMA, LOG_DMA_CH);
    dma_struct_para_init(&config);
    config.periph_addr = (uint32_t)(uintptr_t)&USART_DATA(LOG_USART);
    config.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    config.memory_width = DMA_MEMORY_WIDTH_8BIT;
    config.priority = DMA_PRIORITY_MEDIUM;
    config.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    config.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    config.direction = DMA_MEMORY_TO_PERIPHERAL;
    dma_init(LOG_DMA, LOG_DMA_CH, &config);
    dma_circulation_disable(LOG_DMA, LOG_DMA_CH);
    dma_memory_to_memory_disable(LOG_DMA, LOG_DMA_CH);

    usart_enable(LOG_USART);
    initialized = true;
}

static error_log_uart_start_result_t start(void *context,
                                            const uint8_t *data,
                                            size_t size)
{
    (void)context;
    if (!initialized || (data == NULL) || (size == 0U) || (size > UINT16_MAX)) {
        return ERROR_LOG_UART_START_ERROR;
    }
    if ((tx_state != GD32_TX_IDLE) ||
        ((DMA_CHCTL(LOG_DMA, LOG_DMA_CH) & DMA_CHXCTL_CHEN) != 0U)) {
        return ERROR_LOG_UART_START_BUSY;
    }

    dma_flag_clear(LOG_DMA, LOG_DMA_CH, DMA_FLAG_G);
    usart_flag_clear(LOG_USART, USART_FLAG_TC);
    dma_memory_address_config(LOG_DMA, LOG_DMA_CH, (uint32_t)(uintptr_t)data);
    dma_transfer_number_config(LOG_DMA, LOG_DMA_CH, (uint32_t)size);
    tx_state = GD32_TX_DMA;
    __DMB();
    dma_channel_enable(LOG_DMA, LOG_DMA_CH);
    usart_dma_transmit_config(LOG_USART, USART_TRANSMIT_DMA_ENABLE);
    return ERROR_LOG_UART_STARTED;
}

static bool stop_dma(void)
{
    usart_dma_transmit_config(LOG_USART, USART_TRANSMIT_DMA_DISABLE);
    dma_channel_disable(LOG_DMA, LOG_DMA_CH);
    return (DMA_CHCTL(LOG_DMA, LOG_DMA_CH) & DMA_CHXCTL_CHEN) == 0U;
}

static error_log_uart_status_t status(void *context)
{
    (void)context;
    if (tx_state == GD32_TX_IDLE) {
        return ERROR_LOG_UART_TX_ERROR;
    }

    if (tx_state == GD32_TX_DMA) {
        const bool failed = dma_flag_get(LOG_DMA, LOG_DMA_CH, DMA_FLAG_ERR) != RESET;
        const bool finished = dma_flag_get(LOG_DMA, LOG_DMA_CH, DMA_FLAG_FTF) != RESET;

        if ((!failed && !finished) || !stop_dma()) {
            return ERROR_LOG_UART_TX_ACTIVE;
        }
        dma_flag_clear(LOG_DMA, LOG_DMA_CH, DMA_FLAG_G);
        if (failed) {
            tx_state = GD32_TX_IDLE;
            return ERROR_LOG_UART_TX_ERROR;
        }
        tx_state = GD32_TX_DRAIN;
    }

    if (usart_flag_get(LOG_USART, USART_FLAG_TC) != RESET) {
        tx_state = GD32_TX_IDLE;
        return ERROR_LOG_UART_TX_COMPLETE;
    }
    return ERROR_LOG_UART_TX_ACTIVE;
}

const error_log_uart_driver_t error_log_gd32f30x_uart_driver = {
    .context = NULL,
    .start = start,
    .status = status
};
