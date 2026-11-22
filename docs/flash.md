# Flash 保存与上电回放

默认日志从 RAM 队列经 UART DMA 实时输出。开启 Flash 模式后，后台每次取一条错误记录写入独立 Flash 区。下次上电时，先初始化串口，再扫描 Flash 并输出上次已提交的有效记录。

## 1. 选择编译模式

| 编译参数 | 默认值 | 作用 |
| --- | --- | --- |
| `ERROR_LOG_ENABLED` | `1` | 控制整个日志模块。 |
| `ERROR_LOG_FLASH_ENABLED` | `0` | 设为 `1` 以编译 Flash 模块和相应的板级适配。 |

在整个工程使用相同的编译宏。Keil 可在 **Options for Target → C/C++ → Define** 中加 `ERROR_LOG_FLASH_ENABLED=1`。GCC / ARMClang 也可使用 `-DERROR_LOG_FLASH_ENABLED=1`。启用后加入 `src/error_log_flash.c`，并选一个 Flash 驱动。

| 目标 | 对应驱动 |
| --- | --- |
| STM32F411，STM32CubeF4 HAL | `ports/stm32f4-hal/error_log_stm32f4_flash.c`，见 [STM32F4 接入](../ports/stm32f4-hal/README.md#flash-保存和上电回放)。 |
| GD32F303CGT6，GD32F30x 标准库 | `ports/gd32f30x-spl/error_log_gd32f30x_flash.c`，见 [GD32F30x 接入](../ports/gd32f30x-spl/README.md#flash-保存和上电回放)。 |
| 其他 MCU | 实现 `error_log_flash_driver_t` 的 `read`、`program`，见 [移植约定](#5-移植到其他芯片)。 |

Flash 模式的后台改为调用 `error_log_flash_poll()`。一个 FIFO 只能由一个后台流程消费。需要实时输出和持久化同时开启时，在一个消费者里出队后将该条记录分别交给两个输出端。

## 2. 保留一块独立 Flash

在链接配置里预留完整的擦除页或扇区，把其起始地址和字节数交给板级适配器。检查链接 map、Bootloader、配置区和固件升级分区，确认日志区没有重叠。项目需要与其他模块共用 Flash 控制器时，为适配器传入与其他模块共用的一对锁和解锁回调。

每条记录占 64 B。以保留一个 2 KiB 擦除页为例，可容纳 32 个槽。实际页或扇区大小按具体器件确定。首次使用需要日志区已擦成 `0xFF`。模块按槽追加，区满后保留旧记录，后台消费的后续事件会返回 `ERROR_LOG_FLASH_FULL`。需要回收时，由应用在日志服务停止期间专门擦除预留区，然后重新初始化。若固件下载使用整片擦除，日志也会被擦除。

## 3. 接入启动流程与后台

在 Flash 驱动和诊断串口就绪后调用 `diagnostic_log_start()`，再启动日志线程或开始主循环。示例中的 `flash_driver` 由相应板级适配器创建。`0x00010002U` 是示例固件编号，发布每个固件时应使用自己定义的编号，并保留对应源码。

```c
#include "error_log_flash.h"

static error_log_flash_t log_flash;
static uint32_t flash_full_drops;
static uint32_t flash_write_failures;

/* Select a UART writer from the board guide. */
bool board_log_write(void *context, const char *data, size_t length);

error_log_flash_result_t diagnostic_log_start(
    const error_log_flash_driver_t *flash_driver, void *uart_context)
{
    error_log_flash_result_t status =
        error_log_flash_init(&log_flash, flash_driver, 0x00010002U);

    if (status != ERROR_LOG_FLASH_OK) {
        return status;
    }
    return error_log_flash_replay(&log_flash, board_log_write, uart_context);
}

void diagnostic_log_step(void)
{
    switch (error_log_flash_poll(&log_flash)) {
    case ERROR_LOG_FLASH_FULL:
        ++flash_full_drops;
        break;
    case ERROR_LOG_FLASH_IO_ERROR:
        ++flash_write_failures;
        break;
    default:
        break;
    }
}
```

应用应检查启动返回值。扫描失败时，模块不消费 RAM 队列，可切回实时 UART 输出。扫描成功而回放的 UART 写入失败时，Flash 仍可继续保存，应用可按需要报告回放失败。回放使用逐行同步写入，其返回值表示该行是否发送成功。先完成回放再启用同一 UART 的 DMA 发送，避免输出交错。每行超时和整体启动耗时应纳入产品的启动预算。

业务代码继续调用 `LOG_SAVE_ERR2(error_id, param1, param2)`。返回 `ERROR_LOG_OK` 只表示进了 RAM，后台 `error_log_flash_poll()` 返回 `ERROR_LOG_FLASH_OK` 才表示这一条已经提交并通过读回验证。Flash 操作可能影响同 Bank 的执行时序，要在板上确认日志线程对通信和中断延迟的影响。

## 4. 上电输出样板

下面示例中，日志区有 32 个槽，已用到第 3 个槽。第 2 槽校验失败，因此只回放第 1 和第 3 槽。

```text
[ErrLog] BEGIN valid=2 occupied=3/32 bad=1
[ErrLog] slot=1 fw=0x00010002 seq=1 tick=12345 file=2 line=1100 err=2 p1=0x00002710 p2=0xFFFFFFFF
[ErrLog] slot=3 fw=0x00010002 seq=3 tick=13210 file=3 line=218 err=1 p1=0x00000008 p2=0x00000000
[ErrLog] END shown=2 full=0
```

| 字段 | 含义 |
| --- | --- |
| `valid` / `bad` | 上电扫描认出的有效槽、非空但无效的槽。空槽不计入 `bad`。 |
| `occupied=3/32` | 追加位置已到第 3 槽，共 32 槽。包含其中跳过的空槽和损坏槽。 |
| `slot` / `fw` | Flash 槽号从 1 开始。`fw` 是生成该条记录的固件编号。 |
| `seq` / `tick` | 记录在当次启动的编号与当时的 Tick。Tick 单位由工程时基决定。 |
| `file` / `line` / `err` | 文件编号、调用行号和错误编号。结合相应固件的源码与编号表查找。 |
| `p1` / `p2` | 两个原始 32 位参数，以十六进制输出。传入 `-1` 时显示为 `0xFFFFFFFF`。 |
| `shown` / `full` | 成功回放的条数、扫描时 Flash 是否已无可追加槽。`full=1` 表示已满。 |

空区会输出 `BEGIN valid=0 occupied=0/32 bad=0` 和 `END shown=0 full=0`。回放只读，连续两次上电会再次看到同一批记录。掉电前仍在 RAM 中的记录不会回放。

## 5. 移植到其他芯片

驱动的 `region_size` 是整块日志区的字节数，必须是 64 的整数倍。`read` 和 `program` 的地址参数是日志区内的偏移。驱动负责将偏移换算为实际地址、保证范围检查和读回一致性，并根据目标 MCU 的编程单位、对齐和缓存规则实际写入。每次 `program` 写入 32 B，缓冲区在回调返回前有效。若硬件单次最小编程单位超过 32 B，需要另行设计记录布局。

| 起始偏移 | 内容 |
| ---: | --- |
| 0 | 格式标记 `EFL1`。 |
| 4 | 七个小端 32 位字段，依次为 `seq`、`tick`、`file`、`line`、`err`、`p1`、`p2`。 |
| 32 | 小端 32 位固件编号。 |
| 36 | CRC-32/ISO-HDLC，覆盖偏移 0 到 35。 |
| 40 | 20 字节保留区，内容为 `0xFF`。 |
| 60 | 提交标记 `COM1`。 |

先写入并读回前 32 B，再写包含校验和提交标记的后 32 B。只有两个部分、CRC 和提交标记均有效的记录才会在上电后输出。后 32 B 需要拆分成多次硬件写入时，从低地址顺序往高地址写，将包含提交标记的最小编程单元留到最后。失败的槽本次启动内跳过，下次上电以扫描结果为准。公开版使用 `EFL1` 记录格式，原工程的历史 Flash 数据要用其对应的固件或解析器读取。

首次上板时可先记录一条事件，等待 `error_log_flash_poll()` 成功，再重启检查回放。随后检查两次重启的日志是否一致、写满是否保留旧记录、升级时是否保留日志区，以及写入期间协议任务和中断的响应时间。
