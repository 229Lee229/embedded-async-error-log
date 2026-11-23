# Embedded Async Error Log

给 MCU 固件用的异步错误日志模块。业务代码只把错误编号和参数写入 RAM 队列，后台再格式化，通过 UART DMA 发出。适用于不方便在关键路径直接调用 `printf` 的场合，例如通信协议处理和中断回调。

| 我想做什么 | 从这里开始 |
| --- | --- |
| 在 STM32F411 上接入 | [STM32F4，CubeMX / HAL / UART TX DMA](ports/stm32f4-hal/README.md) |
| 在 GD32F303 上接入 | [GD32F30x，标准外设库 / USART TX DMA](ports/gd32f30x-spl/README.md) |
| 换成其他 MCU、RTOS 或串口驱动 | [通用移植指南](docs/porting.md) |
| 启用 Flash 保存和上电回放 | [Flash 保存与上电回放](docs/flash.md) |
| 理解固定队列、后台消费和缓冲区所有权 | [设计与实现](docs/design.md) |
| 了解 UART DMA 非阻塞调试打印 | [UART DMA 调试打印笔记](docs/uart-dma-debug-print.md) |

## 最小例子

为调用文件和错误类型分配非零编号，然后在已有的错误分支记录。各编号由你的项目管理，可自建一个 `app_log_ids.h` 统一登记。

```c
#define LOG_FILE_ID 1U
#include "error_log.h"

enum {
    LOG_ERR_SENSOR_READ = 1
};

void report_sensor_failure(uint32_t reg, int result)
{
    if (result < 0) {
        (void)LOG_SAVE_ERR2(LOG_ERR_SENSOR_READ, reg, result);
    }
}
```

初始化板级 UART、DMA 和时钟后，在 `while (1)` 或一个日志线程中定期调用 `error_log_uart_poll()`。对 UART 层做静态零初始化，并选用对应板级驱动。

```c
#include "error_log_uart.h"

static error_log_uart_t log_output;

void log_output_step(void)
{
    error_log_uart_poll(&log_output, &board_uart_driver);
}
```

包含目标端口的头文件，将 `board_uart_driver` 换成该端口的驱动对象，然后在后台定期调用 `log_output_step()`。调用一次 `report_sensor_failure(0x10U, -1)` 后，串口应看到类似结果，具体 Tick 和行号取决于你的工程。

```text
seq=1 tick=123 file=1 line=11 err=1 p1=0x00000010 p2=0xFFFFFFFF
```

## 接入与配置

1. 工程加入 `src/error_log.c`、`src/error_log_uart.c`，头文件路径加 `include/`。使用 C99 或更新的 C 模式。
2. 提供 `error_log_port.h` 声明的三个函数。单核 Cortex-M 可加 `ports/cortex-m/error_log_port.c` 和所选板级头文件路径。其他平台依照通用移植指南实现。
3. 按板级指南配置 UART、TX DMA、时钟和引脚，加入对应的驱动 C 文件。只保留一个实际 UART 消费者。
4. 先触发一条日志，再触发几条连续日志。依次检查 `accepted_count`、`dequeued_count` 和 UART 消费者的 `sent_count`。计数从复位后开始。

| 编译参数 | 默认值 | 用途 |
| --- | ---: | --- |
| `ERROR_LOG_ENABLED` | `1` | 全工程设为 `0` 后关闭日志。宏不求值，不分配日志 FIFO。 |
| `ERROR_LOG_QUEUE_CAPACITY` | `16` | 可设 1 到 256。满时丢弃新记录，保留已经接纳的记录。 |

默认队列的记录区在常见的 32 位 MCU 上为 448 B，另有队列元数据和一个 128 B 的 UART 行缓冲区。生产者无需堆，临界区内只处理固定大小的记录。参数按 32 位数值复制，十六进制输出保留负数的原始位模式。队列可以由同一核的普通线程和可屏蔽 ISR 提交，前提是端口满足同步约定。

## 可选 Flash 保存

需要重启后找回错误记录时，设 `ERROR_LOG_FLASH_ENABLED=1`，配置专用 Flash 区。启动时先扫描并回放，再由后台把新记录追加到 Flash。STM32F411 和 GD32F303CGT6 提供板级 Flash 适配。详情和串口输出样板见 [Flash 保存与上电回放](docs/flash.md#4-上电输出样板)。

## 主机验证

```sh
python tests/run_tests.py
```

主机测试需要 Python 3 和 GCC 或 Clang。Windows 默认使用 `gcc`，其他系统默认使用 `cc`。可用环境变量 `CC` 或 `--cc` 选项指定。测试覆盖 16 槽、1 槽、关闭日志以及 Flash 选项，并用模拟 Flash 验证半写记录、CRC、写满和上电回放。STM32F411 和 GD32F303 适配已分别用厂商头文件通过 Arm Compiler 6 的 Cortex-M4 目标编译。尚待真板验证接线、UART 波形和异常恢复。

## 文件结构

| 路径 | 内容 |
| --- | --- |
| `include/`、`src/` | 固定 RAM FIFO、平台接口、UART 消费者、可选 Flash 保存与回放 |
| `ports/` | Cortex-M 临界区示例、两个厂商的 UART 适配 |
| `docs/` | 设计说明与其他芯片移植步骤 |
| `tests/` | 独立主机验证 |
