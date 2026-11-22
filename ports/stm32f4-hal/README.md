# STM32F4 HAL 接入

示例对象为 STM32F411、STM32CubeF4 HAL、单核 Cortex-M4。后台每次调用 `error_log_uart_poll()` 推进发送，UART TX DMA 搬运暂存区里的字符，DMA 与 UART 中断通过 HAL 报告完成。多个应用任务或普通中断可以提交记录，只有一个后台任务或主循环调用发送器。

## 1. 接线与 CubeMX 配置

1. 选一个独占的 USART 用于诊断输出，例如 USART1。通过 CubeMX 为实际封装和板卡选 TX 复用脚。USB 转串口模块用 **3.3 V TTL**，板子 TX 接模块 RX，两端 GND 共地。只打印时接这两根线即可。已有板载 ST-LINK VCP 时，先对照板卡原理图确认 VCP 接到哪路 UART。
2. 通信模式选 `Asynchronous`，方向选 **Transmit Only**，设为 **115200、8 数据位、无校验、1 停止位**，关闭硬件流控。STM32F411 的 UART 与引脚、DMA 流和通道的映射由型号和工程资源决定，按 CubeMX 实际分配核对。
3. 在 UART 的 DMA 设置中添加 `USARTx_TX` 请求。选择 `Memory to Peripheral`、`Normal`、内存地址递增、外设地址固定、两端数据宽度 `Byte`，F4 DMA FIFO 关闭即可。注意避开已有外设占用的 DMA stream。
4. 在 NVIC 中同时启用 **DMA stream IRQ** 和 **USARTx global IRQ**。确认生成的 DMA ISR 调用 `HAL_DMA_IRQHandler()`，USART ISR 调用 `HAL_UART_IRQHandler()`。正常 DMA 模式下，DMA IRQ 处理最后一个字节搬入 UART，UART TC IRQ 在最后一个停止位发送完后触发 `HAL_UART_TxCpltCallback()`。两级 IRQ 都要接通。
5. 生成代码后，核对 `MX_DMA_Init()` 里 DMA 时钟已打开，且在 `MX_USARTx_UART_Init()` 前执行。核对 UART MSP 初始化通过 `__HAL_LINKDMA()` 把 TX DMA 链到 UART 句柄的 `hdmatx`。

## 2. 加入文件

| 类型 | 添加到工程的文件 |
| --- | --- |
| 队列和 UART 输出 | `src/error_log.c`、`src/error_log_uart.c` |
| 临界区和时间戳 | `ports/cortex-m/error_log_port.c` |
| 当前 UART 的适配 | `ports/stm32f4-hal/error_log_stm32f4_uart.c` |
| 头文件搜索路径 | `include`、`ports/stm32f4-hal` |

在 Keil 的 **Options for Target → C/C++ → Include Paths** 加搜索路径，并将三个目录中的四个 C 文件加入工程分组。在 CubeIDE 中把 C 文件加入参与构建的源目录，并在 **Project Properties → C/C++ General → Paths and Symbols** 或编译器包含目录中配置头文件路径。当前目录的 `error_log_port_board.h` 为公共 Cortex-M 移植层提供 CMSIS 定义和 `HAL_GetTick()`。默认 HAL 时基返回毫秒。工程如果改了时基，需确认它在主循环、任务、普通 ISR 读起来都安全，并保持以毫秒递增。UART 超时通过无符号差值处理 tick 回绕。

## 3. 初始化与第一条日志

把自定义代码放在 CubeMX 保留的 `USER CODE` 区域，以下以 USART1 句柄为例。生成独立外设文件时，包含生成的 `usart.h` 即可取得声明。若句柄定义在 `main.c`，在同一文件使用即可。

```c
/* USER CODE BEGIN Includes */
#include "error_log.h"
#include "error_log_stm32f4_uart.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
static error_log_uart_t log_output;
#define LOG_FILE_ID 1U
#define LOG_ERR_BOOT_CHECK 1U
/* USER CODE END PV */

/* USER CODE BEGIN 2 */
/* Place after MX_DMA_Init() and MX_USART1_UART_Init(). */
if (error_log_stm32f4_uart_init(&huart1) != HAL_OK) {
    Error_Handler();
}
(void)LOG_SAVE_ERR2(LOG_ERR_BOOT_CHECK, 123U, 456U);
/* USER CODE END 2 */
```

在 `while (1)` 的用户代码内，每轮推进输出：

```c
error_log_uart_poll(&log_output, &error_log_stm32f4_uart_driver);
```

在 RTOS 中，可由单个低优先级任务持有静态 `log_output` 并定期调用 `error_log_uart_poll()`。任务能短暂睡眠，负责上报错误的调用方继续执行。裸机主循环同样需要保持回访频率。长时间阻塞在其他业务处理会推迟下一条日志。

## 4. 转发 HAL 回调

把以下实现放在 `main.c` 的 `USER CODE BEGIN 4`，也可以放进单独的应用源文件。已有全局同名回调时，把两条转发合入原实现，适配器会自行过滤句柄。若工程启用 `USE_HAL_UART_REGISTER_CALLBACKS` 并另行注册了回调，也要在那里转发。这两个适配函数只报告事件，格式化与启动下次发送由后台处理。

```c
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    error_log_stm32f4_uart_tx_complete(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    error_log_stm32f4_uart_error(huart);
}
```

此例给诊断独占一路 TX。这一路的 `printf` 重定向、业务发送和 HAL 接收需要避免并用。已有日志口时，先让现有 UART 使用方统一到一个输出端。否则发送互相穿插，UART 的通用 Error 回调也会混入 RX 错误。

编译验证使用 Keil MDK 5.42 的 Arm Compiler 6.23、STM32CubeF4 V1.28.3 和 `STM32F411xE`。编译覆盖了核心代码、CMSIS 临界区、UART 适配和上述接入片段。板级验证按下一节的步骤执行。

## 5. 上板验证和排错

1. PC 打开串口终端，设为 `115200 8N1`。按复位，确认能看到错误 ID、文件 ID、行号与两个参数。例如 `seq=1 tick=12 file=1 line=93 err=1 p1=0x0000007B p2=0x000001C8`。tick 和行号随运行时机及源码而变。再从两个应用调用点提交日志，验证顺序和位置。
2. 在背景还来不及发送时集中提交，观察队列统计中的峰值和满队列丢弃计数。8N1 时 115200 波特的理想负载上限约为 11,520 字节每秒。串口排空的速度还受日志长度和后台调度影响。
3. 检查 UART 时基、IRQ 与缓存。此适配器把预估线上帧时间加约 100 ms 用作完成预算。收到 HAL 错误或超过预算后，由后台请求 `HAL_UART_AbortTransmit_IT()`。随后等待 UART 和 DMA 状态均就绪且 DMA stream 的 `EN` 位已清零，才让上层复用暂存区。DMA 彻底停不下来时，适配器继续保留缓冲区。排查 IRQ、外设时钟和驱动状态后再重新启动。

| 现象 | 先查什么 |
| --- | --- |
| 终端完全无数据 | TX 到 RX、共地、电平、端口号、波特率，然后检查 `hdmatx` 及 DMA 请求映射。 |
| 只能打印第一条 | UART global IRQ 是否已启用，生成的 USART ISR 是否调用 HAL UART IRQ handler，完成回调有没有转发。 |
| 时常丢日志 | 检查 `error_log_get_stats()` 的 `dropped_full_count` 及后台所持 `log_output.dropped_count`，估算日志密度与串口吞吐，确认后台能及时推进。 |
| 回调没有路由或 abort 一直等待 | 检查 DMA 和 UART 两级 IRQ、NVIC 优先级、实际中断入口以及 `HAL_GetTick()`。等状态机确认 DMA 已停，再复用发送缓冲区。 |

## 6. 迁移到 STM32H5 或 H7

- **STM32H523**。用对应 STM32CubeH5 HAL 头文件替换 `error_log_port_board.h` 的 F4 头文件。在 CubeMX 为所选 USART TX 分配 GPDMA 请求，使用普通单次传输、源地址递增、目的地址固定，并启用 GPDMA 与 USART IRQ。启用 TrustZone 时，检查 UART、DMA 通道、NVIC 和发送 RAM 的安全属性允许同一个执行域访问。重写 F4 适配器的 DMA 状态和停止确认。H5 的 GPDMA 寄存器及 HAL abort 流程以该系列驱动为准。
- **STM32H723**。先从参考手册的总线矩阵核对所选 DMA 主机可访问的 RAM。常用 DMA1/2 发送区应放在它可达的 AXI SRAM 或系统 SRAM，DTCM 不适合作为这些 DMA 的发送区。若启用了 D-Cache，可将 DMA 区配置成不可缓存，或在启动 TX DMA 前 clean 覆盖发送区的 32 字节缓存行，并给暂存区做合适的对齐和分区。结合实际链接脚本检查 `log_output` 的落点。TX 的维护动作是 clean，使 CPU 已写入的字符对 DMA 可见。
- 两个系列都需要验证正常完成与错误分支。启动成功后，DMA 对暂存区的所有权持续到有效的完成或停止确认。重新对照本系列 HAL API 和完成回调位置。

参考资料

- [ST HAL UART 模式和回调](https://community.st.com/stm32-mcus-60/faq-stm32-hal-uart-driver-api-and-callbacks-29)
- [ST STM32F4 HAL UART 源码](https://github.com/STMicroelectronics/stm32f4xx-hal-driver/blob/master/Src/stm32f4xx_hal_uart.c)
- [ST STM32 GPDMA 应用笔记 AN5593](https://www.st.com/content/st_com/en/technical-documents/an5593-how-to-use-the-gpdma-for-stm32-mcus-stmicroelectronics.html)
- [ST STM32F7/H7 D-Cache 应用笔记 AN4839](https://www.st.com/resource/en/application_note/an4839-level-1-cache-on-stm32f7-series-and-stm32h7-series-stmicroelectronics.pdf)

## Flash 保存和上电回放

项目宏设 `ERROR_LOG_FLASH_ENABLED=1`，加入 `src/error_log_flash.c` 和本目录的 `error_log_stm32f4_flash.c`。保留原 GPIO、系统时钟和 UART 初始化。在链接配置中预留一块由完整扇区组成的 Flash 区，用 `APP_LOG_FLASH_BASE` 和 `APP_LOG_FLASH_SIZE` 提供该区域的实际地址和字节数。CubeF4 工程启用 `HAL_FLASH_MODULE_ENABLED` 并编译 `stm32f4xx_hal_flash.c`。F411xC/xE 的容量和扇区边界由板级适配校验。

在诊断串口初始化后调用下方的启动函数。示例调用 [公共启动代码 `diagnostic_log_start()`](../../docs/flash.md#3-接入启动流程与后台)，先扫描和输出历史记录，再在后台定期调用 `diagnostic_log_step()` 保存新记录。

```c
#include "error_log_stm32f4_flash.h"
#include "stm32f4xx_hal.h"

error_log_flash_result_t diagnostic_log_start(
    const error_log_flash_driver_t *flash_driver, void *uart_context);

error_log_flash_result_t stm32_log_start(UART_HandleTypeDef *uart)
{
    static error_log_stm32f4_flash_region_t region = {
        .base = APP_LOG_FLASH_BASE,
        .size = APP_LOG_FLASH_SIZE,
    };
    error_log_flash_driver_t driver;

    if (!error_log_stm32f4_flash_make_driver(&driver, &region)) {
        return ERROR_LOG_FLASH_INVALID_ARGUMENT;
    }
    return diagnostic_log_start(&driver, uart);
}
```

`region` 需要在整个日志服务运行期间有效。Flash 控制器还给其他任务写配置或擦页时，在 `region` 提供与它们共用的 `.lock`、`.unlock`、`.lock_context`。日志适配器只在指定区域追加。F411 适配使用 HAL 的字节编程，按数据手册在 1.7–3.6 V 编程电压范围内使用。片内 Flash 编程可能阻塞同 Bank 读取，应在目标板检查实时性。

回放时，串口写回调逐行发送，保留每行 100 ms 的预算。串口需已经就绪且当前没有 DMA TX 操作。

```c
bool board_log_write(void *context, const char *data, size_t length)
{
    UART_HandleTypeDef *uart = context;
    return uart != NULL && length <= UINT16_MAX &&
        HAL_UART_Transmit(uart, (uint8_t *)data, (uint16_t)length, 100U) == HAL_OK;
}
```

完整步骤、上电输出样板和字段说明见 [Flash 保存与上电回放](../../docs/flash.md)。

- [ST STM32F411 参考手册 RM0383](https://www.st.com/resource/en/reference_manual/rm0383-stm32f411xce-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)
- [ST STM32F411xC/E 数据手册](https://www.st.com/resource/en/datasheet/stm32f411ce.pdf)
