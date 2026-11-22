# 移植到其他 MCU 和 RTOS

最先验证队列，再接串口。只要目标平台提供短临界区和读 Tick 的能力，FIFO 就能工作。UART、DMA 和 Flash 都在 FIFO 之外。

## 1. 把 FIFO 加入工程

加入 `src/error_log.c`，将 `include/` 加到头文件搜索路径。在 Keil ARM Compiler 5 工程中启用 C99（`--c99`）；ARMClang 或 GCC 可用 `-std=c99` 或更新的 C 标准。编译时应用全工程共享的开关和容量。

| 要实现的函数 | 约定 |
| --- | --- |
| `error_log_port_enter()` | 进入短临界区，返回进入前的状态。所有能访问 FIFO 的执行上下文必须互斥。 |
| `error_log_port_exit(key)` | 恢复原状态。由已关中断的代码调用时，退出后仍保持原屏蔽状态。 |
| `error_log_port_tick()` | 快速返回 32 位计数。在临界区或 ISR 中均能调用。尚无可用时钟时可以返回 0。 |

单核 Cortex-M 可用 [通用 CMSIS 实现](../ports/cortex-m/error_log_port.c)。在工程 include 路径提供一个 `error_log_port_board.h`，包含目标芯片的 CMSIS 设备头，并定义 `ERROR_LOG_PORT_READ_TICK()`。例如，应用已有的毫秒计数提供函数 `my_millisecond_counter()` 时：

```c
#include "your_mcu_device.h"
#include <stdint.h>

uint32_t my_millisecond_counter(void);
#define ERROR_LOG_PORT_READ_TICK() my_millisecond_counter()
```

使用已有 Tick，不在中断中等待 Tick 变化。需要用 RTOS 时间时，选可在普通 ISR 以及当前屏蔽状态下使用的读取路径，记录该 Tick 的频率。如果 Tick 尚未初始化，可先返回 0。实验里先做裸机单核，减少 RTOS 细节带来的变量。

| 平台 | 临界区做法 |
| --- | --- |
| Cortex-M 单核裸机 | 用 CMSIS 保存 `PRIMASK`、关可屏蔽中断、恢复原值。 |
| Cortex-M 单核 RTOS | 可使用同样的短临界区，或使用 RTOS 支持并满足全部调用者互斥的保存/恢复接口。线程 mutex 不适用于 ISR 生产者。 |
| 其他单核架构 | 用厂商或 SDK 的中断状态保存/恢复原语。若工程只在一个执行上下文访问 FIFO，可明确限制调用范围并用空对实现。 |
| 多核、跨 TrustZone 状态、NMI / HardFault | 当前 Cortex-M 端口假定同一核、同一安全域、普通可屏蔽上下文。超出范围时需重新设计同步，并验证抢占、内存顺序及异常路径。 |

在调试器中触发一次 `LOG_SAVE_ERR2()`，观察 `accepted_count` 增加。还未接 UART 时，可以调用一次 `error_log_try_pop()` 检查结构体。接好 UART 后，由一个后台消费者负责出队。

## 2. 接上 UART

把 `src/error_log_uart.c` 加入构建，选择一个专用 TX。初次验证可用 115200、8N1、无流控。USB 转串口适配器的 RX 接 MCU TX，两边 GND 共地，电平按板子 I/O 规格选择。一般的 3.3 V UART 使用 3.3 V TTL 串口适配器。

填一个 `error_log_uart_driver_t`。如果你的 SDK 有像 `uart_write_async()` 这样的接口，适配主要是映射返回值和保存发送状态。

| 回调 | 返回值 | 适配器负责的事 |
| --- | --- | --- |
| `start(context, data, length)` | `ERROR_LOG_UART_STARTED` | 硬件或软件发送器已借用内存。调用本身要尽快返回。 |
| 同上 | `ERROR_LOG_UART_START_BUSY` | 本次未接收这块内存。消费者保留数据，后续轮询再试。 |
| 同上 | `ERROR_LOG_UART_START_ERROR` | 本次未接收内存且启动失败。消费者统计丢弃。 |
| `status(context)` | `ERROR_LOG_UART_TX_ACTIVE` | 仍在发送或仍不能证明缓冲已释放。 |
| 同上 | `ERROR_LOG_UART_TX_COMPLETE` | 停止位已发送完，适配器不再访问缓冲。 |
| 同上 | `ERROR_LOG_UART_TX_ERROR` | 已停止 DMA / 中断发送，并且不再访问缓冲。 |

将 `error_log_uart_t` 设为静态零初始化对象。UART/DMA 初始化后，主循环持续调用 `error_log_uart_poll()`，或放到周期调度的一个日志线程。线程每次调用处理至多一条新记录，可以休眠或等待事件后继续轮询。定时太稀疏时帧间空闲会增大。DMA 完成中断只通知状态，格式化继续留在后台。

### 有 DMA

1. 核对 UART TX 请求号或固定通道映射，外设地址选 TX 数据寄存器，方向选内存到外设。8N1 通常用字节宽度、内存递增、外设地址固定、Normal 模式。
2. 启动前清掉旧状态，提交静态的行缓冲。等待 DMA 状态或完成事件，再等待 UART 的物理 TC。有虚拟串口/HAL 回调时，确认该回调定义是否包含 TC。
3. 传输异常时先关 TX 请求、停止通道，确认不再读内存，再报告失败。给停止流程加明确的超时、状态计数或恢复策略。
4. 将行缓冲放在 DMA 能访问的 RAM。启用 D-cache 的型号还要处理 cache clean 或配置不可缓存区。按实际存储总线和缓存行大小设置地址与对齐，避免与不相关数据共用缓存行。初始化时测一次 DMA 传输完成、末字节和错误恢复。

### 没有 DMA

可做 TX 空中断适配。启动时保存缓冲地址与长度、清旧标志并打开 TX-empty 中断。IRQ 每次只按外设允许的数量写字节。最后一个字节写入后，关 TX-empty 中断，等 TX-complete 再报告完成。发送中断持有缓冲，后台在完成前保持它有效。

最简单的裸机移植还可以在 `status()` 里检查 TX 是否可写，准备好就写少量字节然后立即返回，让主循环快速调用。若业务主循环周期较长，实测吞吐会低于波特率上限。

## 3. 上板验证

| 测试 | 预期 |
| --- | --- |
| 触发一条错误并观察串口 | 有编号、行号和参数。TTL 电平、波特率和接线正确。 |
| 触发几条连续记录 | 顺序号递增。检查第二条是否被发送，避免遗漏 UART TC 完成事件。 |
| 暂停调用消费者，提交超过既定容量 | 前面接纳的记录保留，`dropped_full_count` 增加；恢复后按 FIFO 顺序输出。 |
| 注入可控的发送错误或模拟通道繁忙 | 驱动繁忙时保留当前行；确认硬件停止并报告错误后，`dropped_count` 增加并继续处理后续记录。 |
| 有实时约束时 | 用 GPIO 或周期计数测记录函数的临界区，用逻辑分析仪核对发送及与其他串口的并发时序。 |
