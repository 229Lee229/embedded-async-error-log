# GD32F303 / GD32F30x 标准外设库移植

示例以 GD32F303CGT6 为目标，使用 USART1 的 PA2 发送引脚和 DMA0 通道 6。终端参数为 115200、8 数据位、无校验、1 停止位、无流控。日志生产端在调用现场复制数值到 FIFO。主循环或专用 RTOS 线程格式化记录，DMA 将字节送入 USART。消费者每次运行只推进发送状态，随后把 CPU 交还给其他工作。

## 1. 接线

| GD32 板端 | USB 转 3.3 V TTL 串口 |
| --- | --- |
| PA2（USART1_TX，默认映射） | RXD |
| GND | GND |

示例只发日志，转接器 TXD 和电源脚可以空着。板子按自己的供电方式上电。连接前确认转接器是 3.3 V TTL 电平，按开发板原理图找出 PA2 引出位置。

## 2. 加入工程

1. 确认原工程能编译并完成系统时钟初始化。选用对应芯片的启动文件、CMSIS、GD32F30x 标准外设库和设备宏。本示例所对照的 GD32F303CGT6 工程使用设备宏 `GD32F30X_XD`。其他型号以对应 SDK 的容量分组为准。使用 Keil ARMCC 5 时，启用 C99 模式以支持标准整数类型和指定初始化器。
2. 把 `src/error_log.c`、`src/error_log_uart.c`、`ports/cortex-m/error_log_port.c` 和本目录的 `error_log_gd32f30x_uart.c` 加入 Keil 的编译组、CMake 或 Makefile。为编译器加入仓库的 `include/` 和本目录，保留原有 CMSIS、SDK 头文件路径。
3. 确认工程已编译 SDK 的 `gd32f30x_rcu.c`、`gd32f30x_gpio.c`、`gd32f30x_usart.c`、`gd32f30x_dma.c`。示例自行配置 USART1 和 DMA0 CH6。这对外设要由日志示例独占，检查板级驱动、`printf` 重定向以及其他 DMA 请求是否使用它们。
4. 按下节实现 `error_log_board_tick()`。其读取过程应可在普通中断中调用，并在极短时间内完成。
5. 系统时钟和 tick 就绪后调用 `error_log_gd32f30x_uart_init()`。发送示例通过查询 DMA 状态运行，保持该通道的 DMA 中断关闭即可。

## 3. 选择 tick 来源

**已有单核 RT-Thread 工程**可直接读取内核 tick。此值的单位是系统 tick，换算为毫秒时使用工程的 `RT_TICK_PER_SECOND`。确认工程是单核，并核对当前 RT-Thread 版本的 `rt_tick_get()` 可在普通 ISR 内读取。此示例逻辑不依赖日志 tick 的频率。

```c
#include "error_log_port_board.h"
#include <rtthread.h>

uint32_t error_log_board_tick(void)
{
    return (uint32_t)rt_tick_get();
}
```

**裸机工程**可沿用系统已有的递增 tick，或者为独立示例加入以下 1 ms SysTick。若启动文件或工程中已有 `SysTick_Handler()`，将计数逻辑合并到已有处理函数。独立示例在系统时钟稳定后调用 `diagnostic_tick_init()` 并检查它返回 0，再开始记录。使用 RTOS 时直接使用上一种方式。

```c
#include "error_log_port_board.h"

static volatile uint32_t log_tick_ms;

void SysTick_Handler(void)
{
    ++log_tick_ms;
}

uint32_t diagnostic_tick_init(void)
{
    SystemCoreClockUpdate();
    return SysTick_Config(SystemCoreClock / 1000U);
}

uint32_t error_log_board_tick(void)
{
    return log_tick_ms;
}
```

## 4. 跑通第一条日志

把下面的初始化、测试代码和轮询合并到自己的入口或 RTOS 日志线程。`output` 须保持静态生命周期，且只能交给一个消费者轮询。USART 初始化完成后提交两条记录，用终端验证它们都能收到。实际工程给使用宏的 `.c` 文件分配不同的稳定 `LOG_FILE_ID`，给出错原因分配稳定的错误编号。

```c
#define LOG_FILE_ID 1U
#include "error_log.h"
#include "error_log_gd32f30x_uart.h"

static error_log_uart_t output;

void log_demo_init(void)
{
    error_log_gd32f30x_uart_init();
    (void)LOG_SAVE_ERR2(1U, 123U, 456U);
    (void)LOG_SAVE_ERR2(1U, 789U, 42U);
}

void log_demo_poll(void)
{
    error_log_uart_poll(&output, &error_log_gd32f30x_uart_driver);
}
```

裸机在 `while (1)` 中定期运行 `log_demo_poll()`。RT-Thread 使用一个日志线程循环调用并适当 `rt_thread_mdelay(1)` 让出 CPU，实际唤醒间隔受系统 tick 和调度影响。普通 ISR 可以提交数值记录，格式化及 DMA 状态处理由消费者完成。

## 5. 看懂发送状态

1. `start()` 清掉上次的 DMA 和 USART 完成标志，设置 SRAM 源地址与字节数，再启动 DMA 通道和 USART 发送请求。此时消费者保留这块缓冲区。
2. DMA 的 `FTF` 表示数据已经搬到 USART，最后的字节可能还在移位寄存器里。示例关闭本次 DMA，等 USART 的 `TC` 置位才报告发送完成。
3. 若 DMA 报 `ERR`，先关 USART DMA 请求、关闭通道并确认 `CHEN` 已清零，再返回传输错误，让消费者释放缓冲区。终端可能已收到这行的一部分。
4. 若通道一直没有 `FTF` 或 `ERR`，或者 USART 一直没有 `TC`，每次轮询仍立即返回，待发缓冲区继续保留。需要自动恢复时，应按应用的波特率和调度延迟设置发送超时，并在确认 DMA 停止访问缓冲区后再结束本次发送。

## 6. 移植或排错

| 现象或变更 | 检查项 |
| --- | --- |
| 没有任何输出 | 先确认 PA2 和地线、波特率、日志轮询是否被调用，再读 DMA 剩余计数。计数不动时核对时钟、USART 发送请求、DMA 通道映射以及资源冲突。 |
| 只能收到一条 | 检查 `FTF`、`TC` 是否分别到达，并确认主循环或日志线程持续轮询。示例不依赖 NVIC；若工程里有其他代码清除该通道标志，它会与这个适配竞争。 |
| 文字乱码或偶尔丢行 | 检查 USART 输入时钟和终端参数、是否同时输出了其他 `printf`，再查看 FIFO 满及 UART 消费侧的丢弃统计。 |
| 想换一个 USART 或 GD32 系列 | 用具体芯片数据手册核对 TX 引脚、重映射或 AF 编号，在用户手册核对 TX 对应的 DMA 请求，替换 USART、GPIO、RCU 和 DMA 初始化。带 DMAMUX 的系列还要选正确的请求输入。 |
| 换用带 D-Cache 的 MCU | 把发送状态对象放在 DMA 可访问的内存。按该芯片的 Cache 规则放到不可缓存区，或在 DMA 启动前清理覆盖发送数据的 Cache 行。 |

适配、公共源码和 Cortex-M 平台层已用 Arm Compiler 6.23 对 Cortex-M4 交叉编译，使用 GD32F30x 标准外设库及 `GD32F30X_XD`。接线与串口收发需要在目标板验证。原工程中的日志持久化可作为另外一条消费路径。要复用自己的 Flash，应根据目标芯片重新设计擦除页、写入粒度、预留区域和掉电提交策略。

- [GD32F303xx 数据手册（引脚功能）](https://www.gd32mcu.com/data/documents/datasheet/GD32F303xx%20Datasheet_Rev3.3.pdf)
- [GD32F30x 用户手册（DMA 请求映射与 USART DMA 发送）](https://www.gd32mcu.com/data/documents/userManual/GD32F30x_User_Manual_Rev2.9.pdf)
- [GD32F30x 标准外设库使用指南](https://www.gd32mcu.com/data/documents/userManual/GD32F30x_Firmware_Library_User_Guide_Rev1.0.pdf)

## Flash 保存和上电回放

项目宏设 `ERROR_LOG_FLASH_ENABLED=1`，加入 `src/error_log_flash.c` 和本目录的 `error_log_gd32f30x_flash.c`。保留原 GPIO、系统时钟和 UART 初始化。在链接配置中预留一块由完整页组成的 Bank0 Flash 区，用 `APP_LOG_FLASH_BASE` 和 `APP_LOG_FLASH_SIZE` 提供该区域的实际地址和字节数。工程还需编译标准库的 `gd32f30x_fmc.c`。此适配面向 GD32F303CGT6 的 Bank0，使用与该器件匹配的 SDK 设备宏。

在诊断串口初始化后调用下方的启动函数。示例调用 [公共启动代码 `diagnostic_log_start()`](../../docs/flash.md#3-接入启动流程与后台)，先扫描和输出历史记录，再在后台定期调用 `diagnostic_log_step()` 保存新记录。

```c
#include "error_log_gd32f30x_flash.h"
#include "gd32f30x_usart.h"

error_log_flash_result_t diagnostic_log_start(
    const error_log_flash_driver_t *flash_driver, void *uart_context);

error_log_flash_result_t gd32_log_start(void)
{
    static error_log_gd32f30x_flash_region_t region = {
        .base = APP_LOG_FLASH_BASE,
        .size = APP_LOG_FLASH_SIZE,
    };
    error_log_flash_driver_t driver;

    if (!error_log_gd32f30x_flash_make_driver(&driver, &region)) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }
    return diagnostic_log_start(&driver, NULL);
}
```

`region` 需要在整个日志服务运行期间有效。Flash 控制器还给其他任务写配置或擦页时，在 `region` 提供与它们共用的 `.lock`、`.unlock`、`.lock_context`。日志适配器只在指定区域追加。此适配面向 GD32F303CGT6 的 Bank0，页大小为 2 KiB。需要使用正常执行的毫秒时基 `board_millis()` 来限制串口超时。

回放时，串口写回调逐行发送，保留每行 100 ms 的预算。串口需已经就绪且当前没有 DMA TX 操作。

```c
/* Application's millisecond timebase, advancing during this blocking call. */
uint32_t board_millis(void);

static bool wait_usart_flag(usart_flag_enum flag, uint32_t start_ms)
{
    while (usart_flag_get(USART1, flag) == RESET) {
        if ((uint32_t)(board_millis() - start_ms) >= 100U) {
            return false;
        }
    }
    return true;
}

bool board_log_write(void *context, const char *data, size_t length)
{
    (void)context;
    const uint32_t start_ms = board_millis();

    for (size_t i = 0; i < length; ++i) {
        if (!wait_usart_flag(USART_FLAG_TBE, start_ms)) {
            return false;
        }
        usart_data_transmit(USART1, (uint16_t)(uint8_t)data[i]);
    }
    return wait_usart_flag(USART_FLAG_TC, start_ms);
}
```

完整步骤、上电输出样板和字段说明见 [Flash 保存与上电回放](../../docs/flash.md)。
