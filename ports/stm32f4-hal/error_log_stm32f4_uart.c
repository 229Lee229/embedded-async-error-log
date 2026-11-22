#include "error_log_stm32f4_uart.h"

#include <stddef.h>

typedef enum {
    TX_IDLE,
    TX_WAITING,
    TX_COMPLETED,
    TX_FAULT,
    TX_ABORTING
} tx_event_t;

typedef struct {
    UART_HandleTypeDef *uart;
    volatile tx_event_t event;
    uint32_t started_ms;
    uint32_t limit_ms;
} stm32f4_uart_t;

static stm32f4_uart_t output;

HAL_StatusTypeDef error_log_stm32f4_uart_init(UART_HandleTypeDef *uart)
{
    const DMA_HandleTypeDef *dma;

    if (output.uart != NULL || uart == NULL || uart->Instance == NULL ||
        uart->hdmatx == NULL || uart->hdmatx->Instance == NULL) {
        return HAL_ERROR;
    }

    dma = uart->hdmatx;
    if (dma->Parent != uart || uart->Init.Mode != UART_MODE_TX || uart->Init.BaudRate == 0U ||
        uart->Init.WordLength != UART_WORDLENGTH_8B ||
        uart->Init.Parity != UART_PARITY_NONE ||
        uart->Init.StopBits != UART_STOPBITS_1 ||
        uart->Init.HwFlowCtl != UART_HWCONTROL_NONE ||
        dma->Init.Mode != DMA_NORMAL || dma->Init.Direction != DMA_MEMORY_TO_PERIPH ||
        dma->Init.PeriphInc != DMA_PINC_DISABLE || dma->Init.MemInc != DMA_MINC_ENABLE ||
        dma->Init.PeriphDataAlignment != DMA_PDATAALIGN_BYTE ||
        dma->Init.MemDataAlignment != DMA_MDATAALIGN_BYTE) {
        return HAL_ERROR;
    }

    output.uart = uart;
    return HAL_OK;
}

static error_log_uart_start_result_t start(void *context, const uint8_t *data, size_t length)
{
    stm32f4_uart_t *state = context;
    UART_HandleTypeDef *uart = state->uart;
    uint32_t wire_time_ms;
    HAL_StatusTypeDef result;

    if (uart == NULL || data == NULL || length == 0U || length > UINT16_MAX) {
        return ERROR_LOG_UART_START_ERROR;
    }
    if (state->event != TX_IDLE || HAL_DMA_GetState(uart->hdmatx) != HAL_DMA_STATE_READY) {
        return ERROR_LOG_UART_START_BUSY;
    }

    /* A byte takes ten bit times at 8N1. Allow 100 ms for interrupt service. */
    wire_time_ms = ((uint32_t)length * 10000U) / uart->Init.BaudRate;
    state->started_ms = HAL_GetTick();
    state->limit_ms = wire_time_ms + 101U;

    /* The interrupt can run before HAL_UART_Transmit_DMA returns. */
    state->event = TX_WAITING;
    /* Older CubeF4 releases take a non-const pointer for this TX-only buffer. */
    result = HAL_UART_Transmit_DMA(uart, (uint8_t *)data, (uint16_t)length);
    if (result == HAL_OK) {
        return ERROR_LOG_UART_STARTED;
    }

    state->event = TX_IDLE;
    if (result == HAL_BUSY) {
        return ERROR_LOG_UART_START_BUSY;
    }
    return ERROR_LOG_UART_START_ERROR;
}

static error_log_uart_status_t status(void *context)
{
    stm32f4_uart_t *state = context;
    UART_HandleTypeDef *uart = state->uart;
    uint32_t now = HAL_GetTick();
    uint32_t mask = __get_PRIMASK();
    tx_event_t observed;

    /* Resolve a timeout against the completion/error ISR once. */
    __disable_irq();
    observed = state->event;
    if (observed == TX_FAULT ||
        (observed == TX_WAITING && now - state->started_ms >= state->limit_ms)) {
        state->event = TX_ABORTING;
        __set_PRIMASK(mask);
        (void)HAL_UART_AbortTransmit_IT(uart);
        return ERROR_LOG_UART_TX_ACTIVE;
    }
    if (observed == TX_COMPLETED) {
        state->event = TX_IDLE;
        __set_PRIMASK(mask);
        return ERROR_LOG_UART_TX_COMPLETE;
    }
    __set_PRIMASK(mask);

    if (observed == TX_ABORTING &&
        HAL_UART_GetState(uart) == HAL_UART_STATE_READY &&
        HAL_DMA_GetState(uart->hdmatx) == HAL_DMA_STATE_READY &&
        (uart->hdmatx->Instance->CR & DMA_SxCR_EN) == 0U) {
        /* Even the HAL abort request can finish later in the DMA IRQ. */
        state->event = TX_IDLE;
        return ERROR_LOG_UART_TX_ERROR;
    }

    return ERROR_LOG_UART_TX_ACTIVE;
}

void error_log_stm32f4_uart_tx_complete(UART_HandleTypeDef *uart)
{
    if (uart == output.uart && output.event == TX_WAITING) {
        output.event = TX_COMPLETED;
    }
}

void error_log_stm32f4_uart_error(UART_HandleTypeDef *uart)
{
    if (uart == output.uart &&
        (output.event == TX_WAITING || output.event == TX_COMPLETED)) {
        output.event = TX_FAULT;
    }
}

const error_log_uart_driver_t error_log_stm32f4_uart_driver = {
    .context = &output,
    .start = start,
    .status = status,
};
