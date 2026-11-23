# Non-blocking UART DMA Debug Print / UART DMA 非阻塞调试打印

`board_uart1_dma_printf()` 把调试文本复制到固定 RAM 队列后返回，UART DMA 在后台发送。业务线程不用等上一条发完，也不用等队列腾出空间。调用方仍负责格式化和内存复制。

Format and enqueue a complete debug message in the caller, then let UART TX DMA send it in chunks. A positive return value confirms queue acceptance. Check UART TC when you need to know that the final stop bit has left the pin.

## 1. 在原工程里怎么用

等板级 UART 初始化完成后，在普通线程调用 `s_debug_printf()`。这个宏指向 `board_uart1_dma_printf()`。已经有文本或二进制缓冲区时，可用 `board_uart1_dma_write()` 直接复制到同一个队列，省去格式化。

```c
#include "board_uart1.h"

void print_sample_status(void)
{
    int queued = s_debug_printf("[DBG] sample=%lu status=%d\r\n", 42UL, 0);

    if (queued == -2) {
        /* Drop this message or update an application-owned loss counter. */
    }
}
```

串口最终会收到类似下面的一行。示例用于说明格式，发送时机以实际队列和串口状态为准。

```text
[DBG] sample=42 status=0
```

| 返回值 | 怎么理解 |
| --- | --- |
| `> 0` | 本次接纳的字节数，发送可能还在进行。 |
| `0` | UART 尚未初始化，或内容为空。 |
| `-1` | 无效参数、中断调用、格式化失败、超长，或 DMA/队列错误。若第一次启动 DMA 失败，文本可能已复制进队列。 |
| `-2` | 空间不足，本条消息一个字节也未入队。 |

## 2. 一条消息如何从 CPU 交给 DMA

1. **格式化**。`vsnprintf()` 先把参数转成文本，写入共享的 513 B 暂存区。512 B 是单条消息的字节上限，另一个字节留给字符串结束符。DMA 读取的是发送队列。
2. **整条入队**。保存当前中断屏蔽状态，再屏蔽普通中断并检查 1024 B 环形队列的剩余空间。够用才整条复制。若写入跨过末尾，就分成尾部和头部两段。退出时恢复调用前的中断状态。
3. **启动一块发送**。DMA 用普通模式。每次取读位置起的连续区间，长度取已用字节数、到队尾的连续长度与 512 B 上限三者的最小值。先记录在途长度，再启动 DMA。队列已用字节数包含在途段，新的写入不会覆盖它。
4. **中断续发**。DMA 成功搬完后，中断推进读指针并释放刚刚完成的空间。还有数据就启动下一块，包括回绕后的队头。发生 DMA 错误时锁存错误、停止续发，出错的段不会按成功发送回收。当前接口没有自动重试。

例如，队列里已有 300 B，其中 80 B 正在被 DMA 读取。新日志可用的空间仍是 `1024 - 300 = 724 B`。收到这 80 B 的 DMA 完成事件后，才释放对应的 80 B。

## 3. 怎么判断真的发完

| 时刻 | 代表什么 |
| --- | --- |
| `board_uart1_dma_printf()` 返回正数 | 当前消息已被接纳。业务线程可以继续。 |
| DMA `FTF` | 当前块已从 RAM 搬到 UART。可以回收该块队列空间。 |
| 发送队列为空、没有在途块、DMA 不忙且 UART `TC` 置位 | 整个共享发送队列已排空，最后一个停止位已从 TX 发出。 |

需要等日志真的发完时，用 `board_uart1_dma_printf_tx_done()` 检查整个共享队列。`0` 表示仍在发送，`-1` 表示错误，`1` 表示空闲。未初始化也会返回 `1`，所以先确认 UART 已初始化且有消息成功入队。若必须等待，给等待设置超时，并在普通线程适当让出 CPU。原工程的 `board_uart1_flush()` 对应另一套队列，此处使用 `tx_done()`。

## 4. 非阻塞的边界

- 多个线程调用时，串行化整个 `board_uart1_dma_printf()`。环形队列保护了入队和中断之间的共享状态，格式化暂存区在队列临界区之外。当前两个入队接口都要求普通线程上下文。
- 普通 `printf()` 走 `fputc()` 的逐字节轮询路径。使用同一个 UART 时统一发送入口，避免轮询字符与 DMA 日志交错。
- 调用耗时包括 `vsnprintf()`、关中断期间的复制以及 DMA 启动。原实现启动 DMA 前轮询通道关闭，未设置独立超时。若产品需要可证明的最坏执行时间，应先给这段硬件等待设限，并实测临界区和格式化开销。
- DMA 节省 CPU 等待，发送时间仍受波特率限制。以 115200 baud、8N1 为例，100 B 的线上时间约为 `100 × 10 ÷ 115200 ≈ 8.7 ms`，这是计算值。

## 5. 移植与验证

移植时保留两条约定。业务调用交付后由驱动拥有缓冲区，DMA 搬完前保留在途空间。最后一字节是否离开 TX 按目标 UART 的发送完成事件单独判断。选择 DMA 可访问的 RAM。启用 D-cache 的平台还要按芯片要求处理缓存一致性。 [ST UART DMA 完成回调说明](https://community.st.com/stm32-mcus-60/faq-stm32-hal-uart-driver-api-and-callbacks-29)

原工程文档记录 UART 原始入队与完成判断的 3 个宿主测试场景通过。可上板分别测量调用耗时、DMA 完成时刻和 UART TC，连续发送验证回绕与顺序，压满队列检查 `-2` 时终端是否只收到完整消息。本笔记依据现有源码和测试记录编写，示例未上板复测。

同仓库的 [Embedded Async Error Log](../README.md) 使用数字事件作为业务入口，把格式化推迟到后台，并支持可选的 [Flash 保存和上电回放](flash.md)。`board_uart1_dma_printf()` 适合在线程中直接输出调试文本。
