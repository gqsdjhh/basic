# USART 接收环形缓冲区与中断任务化改造详解

> 本文以 USART 封装、遥控器和裁判系统应用为例，详细说明 USART 接收帧环形缓冲区的实现。
>
> 总体改造说明见：[中断数据解析任务化改造说明](./中断数据解析任务化改造说明.md)。

## 1. 改造目标

改造前，USART 的协议解析直接放在 `HAL_UARTEx_RxEventCallback()` 中执行：

```text
USART 中断
  → 直接调用模块回调
  → 在中断中解析协议、更新全局数据
  → 清 buffer
  → 重新启动 DMA
  → 返回
```

后续虽然把解析移到了回调任务，但采用的是“暂停 DMA → 任务解析 → 重新启动 DMA”的方式：

```text
USART 中断
  → 通知任务
  → 返回

任务
  → 解析 recv_buff
  → 清 buffer
  → 重新启动 DMA
```

这种方式在任务解析期间无法接收新数据，如果连续来帧就可能丢帧。

本次改造增加 **接收帧环形缓冲区**：

```text
USART 中断
  → 把整帧拷贝到环形缓冲区
  → 立即重新启动 DMA
  → 通知任务
  → 返回

回调任务
  → 从环形缓冲区取出整帧
  → 拷贝到 recv_buff
  → 调用模块回调解析
```

这样 DMA 不再等待任务解析，连续帧可以被缓存起来。

## 2. 为什么选择帧环形缓冲区

### 2.1 帧环形缓冲区

每个缓冲区元素是一整帧：

```c
typedef struct
{
    uint16_t length;
    uint8_t data[USART_RXBUFF_LIMIT];
} USART_RxFrame_t;
```

优点：

- 保持“一次 DMA/IDLE 事件对应一帧”的现有语义；
- 模块回调继续读取 `instance->recv_buff`；
- 应用层、模块层不需要修改；
- 溢出时丢弃整帧，协议不会错位。

### 2.2 不采用原始字节环形缓冲区

原始字节环形缓冲区只保存字节流，任务侧必须自己实现：

- 帧头查找；
- 长度字段解析；
- 半帧缓存；
- 一次收到多帧时的拆分；
- 溢出后的重新同步。

这意味着遥控器、裁判系统、HC05、总线舵机等所有 USART 模块都要改造。当前工程是包协议结构，因此选择帧环形缓冲区。

## 3. bsp_tools 的任务通知机制

`bsp/bsp_tools.h`：

```c
typedef struct
{
    osThreadId_t thread_id;
    uint32_t signal;
} CallbackTaskHandle_t;
```

`bsp/bsp_tools.c` 中的任务入口：

```c
static void CallbackTaskEntry(void *argument)
{
    CallbackTaskInfo_t *info = (CallbackTaskInfo_t *)argument;

    for (;;)
    {
        osThreadFlagsWait(info->signal, osFlagsWaitAll, osWaitForever);
        info->callback(info->argument);
    }
}
```

任务采用“先等待，再执行”的顺序，避免创建任务时先空跑一次回调。

中断通过：

```c
static inline void CallbackTaskNotify(const CallbackTaskHandle_t *handle)
{
    if ((handle != NULL) && (handle->thread_id != NULL) && (handle->signal != 0U))
        (void)osThreadFlagsSet(handle->thread_id, handle->signal);
}
```

唤醒 USART 回调任务。

## 4. USART 数据结构

### 4.1 环形缓冲区参数

`bsp/usart/bsp_usart.h`：

```c
#define USART_RXBUFF_LIMIT 256
#define USART_RX_FIFO_DEPTH 4U
#define USART_RX_FIFO_MASK (USART_RX_FIFO_DEPTH - 1U)

#if ((USART_RX_FIFO_DEPTH & (USART_RX_FIFO_DEPTH - 1U)) != 0U)
#error "USART_RX_FIFO_DEPTH must be a power of two"
#endif
```

说明：

| 参数 | 含义 |
|---|---|
| `USART_RXBUFF_LIMIT` | 单帧最大长度，也是环形缓冲区槽位的最大长度 |
| `USART_RX_FIFO_DEPTH` | 每个 USART 实例的环形槽位数，必须是 2 的幂 |
| `USART_RX_FIFO_MASK` | 用于快速计算环形索引 |

当前槽位数为 4，环形队列保留一个空槽区分满/空，因此可用容量为 **3 帧**。

### 4.2 帧结构

```c
typedef struct
{
    uint16_t length;
    uint8_t data[USART_RXBUFF_LIMIT];
} USART_RxFrame_t;
```

每个槽位保存：

- 本帧实际长度；
- 本帧数据。

### 4.3 USARTInstance 新增字段

```c
typedef struct
{
    uint8_t recv_buff[USART_RXBUFF_LIMIT];     // 任务解析用buffer
    uint8_t dma_recv_buff[USART_RXBUFF_LIMIT]; // DMA直接写入的buffer
    uint8_t recv_buff_size;                    // 一帧最大长度
    UART_HandleTypeDef *usart_handle;
    usart_module_callback module_callback;

    CallbackTaskHandle_t rx_task; // 接收解析任务
    volatile uint16_t rx_len;     // 当前帧长度

    USART_RxFrame_t rx_fifo[USART_RX_FIFO_DEPTH];
    volatile uint16_t rx_fifo_head;
    volatile uint16_t rx_fifo_tail;
    volatile uint32_t rx_fifo_overflow;

    /* 发送环形缓冲区相关成员保持不变 */
    ...
} USARTInstance;
```

字段职责：

| 字段 | 作用 |
|---|---|
| `dma_recv_buff` | DMA 直接写入，避免和任务解析 buffer 冲突 |
| `recv_buff` | 模块回调读取的完整帧 |
| `rx_fifo` | 帧环形缓冲区 |
| `rx_fifo_head` | ISR 写入位置 |
| `rx_fifo_tail` | 任务读取位置 |
| `rx_fifo_overflow` | 队列满或帧过大时的丢帧计数 |
| `rx_len` | 当前正在处理的帧长度 |

原来的 `rx_pending` 已移除。DMA 不再等待任务解析，因此不需要 pending 标志来阻止重复启动。

## 5. 初始化流程

`USARTRegister()` 主要流程：

```c
instance->usart_handle = init_config->usart_handle;
instance->recv_buff_size = init_config->recv_buff_size;
instance->module_callback = init_config->module_callback;

if (instance->module_callback != NULL)
{
    instance->rx_task = CreateCallbackTask("usart_rx",
                                           USARTDeferredRx,
                                           instance,
                                           osPriorityAboveNormal,
                                           CALLBACK_TASK_DEFAULT_STACK_SIZE);
    if (instance->rx_task.thread_id == NULL)
        LOGERROR("[bsp_usart] failed to create rx callback task");
}

usart_instance[idx++] = instance;
USARTServiceInit(instance);
```

注册时：

1. 分配 `USARTInstance` 并清零；
2. 保存串口句柄、最大帧长度和模块回调；
3. 如果模块注册了回调，创建 `usart_rx` 任务；
4. 将实例加入全局实例数组；
5. 启动第一次 DMA 接收。

## 6. DMA 接收启动与重启

内部函数：

```c
static HAL_StatusTypeDef USARTStartReceive(USARTInstance *instance)
{
    HAL_StatusTypeDef status =
        HAL_UARTEx_ReceiveToIdle_DMA(instance->usart_handle,
                                     instance->dma_recv_buff,
                                     instance->recv_buff_size);

    if (status == HAL_OK)
        __HAL_DMA_DISABLE_IT(instance->usart_handle->hdmarx, DMA_IT_HT);

    return status;
}
```

`USARTServiceInit()`：

```c
void USARTServiceInit(USARTInstance *_instance)
{
    HAL_StatusTypeDef status = USARTStartReceive(_instance);
    if ((status != HAL_OK) && (status != HAL_BUSY))
        LOGERROR("[bsp_usart] failed to start receive, status [%d]", status);
}
```

说明：

- `HAL_UARTEx_ReceiveToIdle_DMA()` 每次只负责一次接收事务；
- 收到 IDLE 或接收长度达到 `recv_buff_size` 后，HAL 会结束本次接收并调用回调；
- 因此每处理完一帧都必须再次启动，否则后续数据不会再进入 DMA；
- 返回 `HAL_BUSY` 表示接收已经处于活动状态，不属于错误；
- DMA 半传输中断被关闭，避免同一帧触发两次 `HAL_UARTEx_RxEventCallback()`。

## 7. 环形缓冲区入队

```c
static uint8_t USART_RxFifoPush(USARTInstance *instance,
                                const uint8_t *data,
                                uint16_t length)
{
    if (length > USART_RXBUFF_LIMIT)
    {
        instance->rx_fifo_overflow++;
        return 0;
    }

    uint16_t next = (instance->rx_fifo_head + 1U) & USART_RX_FIFO_MASK;
    if (next == instance->rx_fifo_tail)
    {
        instance->rx_fifo_overflow++;
        return 0;
    }

    USART_RxFrame_t *frame = &instance->rx_fifo[instance->rx_fifo_head];
    frame->length = length;
    memcpy(frame->data, data, length);

    __DMB();
    instance->rx_fifo_head = next;
    return 1;
}
```

特点：

- 单生产者：USART ISR；
- 单消费者：`usart_rx` 任务；
- 队列满时丢弃 **最新帧**，保持已入队帧的顺序；
- `rx_fifo_overflow++` 记录丢帧；
- 写入数据后使用 `__DMB()`，再发布 `head`；
- 槽位数为 4，保留一个空槽，因此最多缓存 3 帧。

## 8. 环形缓冲区出队

```c
static uint8_t USART_RxFifoPop(USARTInstance *instance, uint16_t *length)
{
    if (instance->rx_fifo_tail == instance->rx_fifo_head)
        return 0;

    __DMB();

    USART_RxFrame_t *frame = &instance->rx_fifo[instance->rx_fifo_tail];
    uint16_t frame_length = frame->length;
    if (frame_length > instance->recv_buff_size)
        frame_length = instance->recv_buff_size;

    memset(instance->recv_buff, 0, instance->recv_buff_size);
    memcpy(instance->recv_buff, frame->data, frame_length);
    *length = frame_length;

    __DMB();
    instance->rx_fifo_tail = (instance->rx_fifo_tail + 1U) & USART_RX_FIFO_MASK;
    return 1;
}
```

出队时：

1. 检查队列非空；
2. 读取帧长度；
3. 清空整个 `recv_buff`，避免短帧尾部残留旧数据；
4. 把帧拷贝到 `recv_buff`；
5. 更新 `tail`，释放槽位。

模块回调仍然只读取 `instance->recv_buff`，因此模块层不需要修改。

## 9. 回调任务处理

```c
static void USARTDeferredRx(void *arg)
{
    USARTInstance *instance = (USARTInstance *)arg;
    uint16_t frame_length;

    while (USART_RxFifoPop(instance, &frame_length))
    {
        instance->rx_len = frame_length;
        if (instance->module_callback != NULL)
            instance->module_callback();
    }

    // ISR中重启失败时,在任务中补一次恢复
    if (instance->usart_handle->RxState == HAL_UART_STATE_READY)
    {
        HAL_StatusTypeDef status = USARTStartReceive(instance);
        if ((status != HAL_OK) && (status != HAL_BUSY))
            LOGERROR("[bsp_usart] failed to restart receive, status [%d]", status);
    }

    uint32_t saved_baspri = __get_BASEPRI();
    __set_BASEPRI(0x50U);
    uint32_t overflow = instance->rx_fifo_overflow;
    instance->rx_fifo_overflow = 0U;
    __set_BASEPRI(saved_baspri);

    if (overflow != 0U)
        LOGWARNING("[bsp_usart] rx fifo overflow, dropped [%u] frames", (unsigned)overflow);
}
```

任务行为：

- 被唤醒后一次处理队列中所有帧；
- 每帧设置 `rx_len` 后调用模块回调；
- 正常情况下不在任务中重新启动 DMA，DMA 已由 ISR 立即重启；
- 如果检测到 `RxState == HAL_UART_STATE_READY`，会补一次启动，作为 ISR 重启失败的后备；
- 队列溢出时在任务中打印警告。

## 10. 中断回调

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    for (uint8_t i = 0; i < idx; ++i)
    {
        if (huart == usart_instance[i]->usart_handle)
        {
            USARTInstance *instance = usart_instance[i];

            if (instance->module_callback != NULL)
            {
                if (instance->rx_task.thread_id != NULL)
                {
                    if (Size > 0U)
                        (void)USART_RxFifoPush(instance,
                                               instance->dma_recv_buff,
                                               Size);

                    (void)USARTStartReceive(instance);
                    if (Size > 0U)
                        CallbackTaskNotify(&instance->rx_task);
                }
                else
                {
                    // 任务创建失败时回退到中断内处理
                    uint16_t copy_len = Size;
                    if (copy_len > instance->recv_buff_size)
                        copy_len = instance->recv_buff_size;

                    memset(instance->recv_buff, 0, instance->recv_buff_size);
                    memcpy(instance->recv_buff,
                           instance->dma_recv_buff,
                           copy_len);
                    instance->rx_len = copy_len;
                    instance->module_callback();
                    (void)USARTStartReceive(instance);
                }
            }
            else
            {
                (void)USARTStartReceive(instance);
            }
            return;
        }
    }
}
```

中断顺序非常重要：

```text
1. 从 dma_recv_buff 拷贝整帧到 ring
2. 重新启动 DMA
3. 通知任务
```

如果先重启 DMA 再拷贝，DMA 可能覆盖 `dma_recv_buff`，造成数据错误。

三种情况：

| 情况 | 行为 |
|---|---|
| 有模块回调且有任务 | 入队 + 重启 DMA + 通知任务 |
| 有模块回调但任务创建失败 | 回退到中断内直接解析 |
| 没有模块回调 | 只重启 DMA |

## 11. 错误和离线恢复

`HAL_UART_ErrorCallback()` 中：

```c
if (usart_instance[i]->usart_handle->RxState == HAL_UART_STATE_READY)
    (void)USARTStartReceive(usart_instance[i]);
```

含义：

- 如果 UART 接收状态已经回到 READY，重新启动 DMA；
- 如果 RX 仍在进行中，不重复启动；
- `USARTStartReceive()` 返回 `HAL_BUSY` 时忽略，表示已经处于接收状态。

模块 daemon 离线恢复仍然调用：

```c
USARTServiceInit(instance);
```

由 `USARTStartReceive()` 重新启动接收。

## 12. 遥控器应用示例

### 12.1 初始化

```c
rc_data = RemoteControlInit(&huart3);
```

模块内部：

```c
conf.module_callback = RemoteControlRxCallback;
conf.recv_buff_size = REMOTE_CONTROL_FRAME_SIZE;
rc_usart_instance = USARTRegister(&conf);
```

`USARTRegister()` 创建 `usart_rx` 任务并启动第一次 DMA。

### 12.2 接收流程

```text
USART3/DMA 中断
    → HAL_UARTEx_RxEventCallback()
        → USART_RxFifoPush()
        → USARTStartReceive()
        → CallbackTaskNotify()
    → 返回

usart_rx 任务
    → osThreadFlagsWait()
    → USARTDeferredRx()
        → USART_RxFifoPop()
        → module_callback()
            → RemoteControlRxCallback()
                → DaemonReload()
                → sbus_to_rc()
                    → 更新 rc_ctrl[]
```

### 12.3 应用读取

`RobotCMDTask()` 仍然直接读取：

```c
rc_data[TEMP].rc.switch_right
rc_data[TEMP].rc.rocker_l_
rc_data[TEMP].rc.dial
...
```

数据结构和访问方式没有变化。由于 `usart_rx` 任务优先级为 `AboveNormal`，高于 ROBOT 任务的 `Normal`，所以应用任务读取前数据已经更新。

## 13. 裁判系统应用示例

```text
USART6/DMA 中断
    → 整帧入队
    → 立即重启 DMA
    → 通知任务

usart_rx 任务
    → 取出整帧
    → RefereeRxCallback()
        → DaemonReload()
        → JudgeReadData()
            → 解析裁判系统多帧数据
```

`JudgeReadData()` 可能包含多帧递归解析，现在运行在任务中，不再占用 USART6 中断。

应用层 `ChassisTask()` 仍然读取：

```c
referee_data->GameRobotState.chassis_power_limit
referee_data->PowerHeatData...
```

不需要修改。

## 14. 应用层改动

业务应用层不需要修改。

唯一相关的启动时序调整在 `application/robot.c`：

```c
__disable_irq();
BSPInit();
RobotCMDInit();
ChassisInit();
OSTaskInit();
// 不在这里 __enable_irq()
```

中断保持关闭，直到 FreeRTOS 启动第一个任务时由移植层统一开启，避免调度器启动前 USART 中断调用 `osThreadFlagsSet()`。

## 15. 优先级与数据一致性

当前优先级：

| 任务 | 优先级 |
|---|---|
| INS | `osPriorityAboveNormal` |
| USART 回调任务 | `osPriorityAboveNormal` |
| ROBOT | `osPriorityNormal` |
| MOTOR | `osPriorityNormal` |
| DAEMON | `osPriorityNormal` |
| UI | `osPriorityNormal` |

USART 回调任务高于应用任务，因此：

- 中断把帧放入 ring；
- 回调任务尽快取出并解析；
- 应用任务读取时数据已经更新；
- 多字段结构不会在中断和任务之间同时读写。

## 16. 内存和性能

### 16.1 静态构建数据

最近一次全量构建：

```text
RAM:   91144 B / 128 KB (69.54%)
FLASH: 107080 B / 512 KB (20.42%)
```

### 16.2 USART 实例运行时内存

`USARTInstance` 使用 `malloc()` 分配，因此接收环形缓冲区占用的是 newlib 堆，不会体现在静态 `.bss` 中。

每个实例新增约：

```text
dma_recv_buff: 256 B
rx_fifo:       4 × (256 + 2) ≈ 1032 B
索引/计数:     ≈ 8 B
合计:          ≈ 1.3 KB
```

3 个 USART 实例约增加 **3.9 KB** 运行时内存。

### 16.3 ISR 开销

ISR 新增工作：

- 最多 255B 的 `memcpy`；
- 一次环形队列索引更新；
- 一次 DMA 重启；
- 一次线程通知。

典型帧 18B，开销很小；最大 255B 时也只有一次短拷贝，远小于原来的协议解析时间。

## 17. 已知限制

1. **帧边界仍然来自 HAL IDLE/TC**  
   环形缓冲区解决的是“任务解析期间接收暂停”，不解决半帧、粘包问题。

2. **一次 DMA 事件包含多帧时**  
   整块会作为一个槽传入模块回调。当前遥控器等固定帧协议通常不会出现；裁判系统解析器本身支持一块多帧。

3. **一帧被拆成多次 IDLE 事件时**  
   会作为多个槽进入队列，模块可能收到半帧。需要协议层自己处理或增大 IDLE 判定。

4. **队列满时丢弃最新帧**  
   不会破坏已缓存帧，但会丢帧并打印警告。槽位数为 4，可用容量为 3 帧，可通过增大 `USART_RX_FIFO_DEPTH` 缓解。

5. **每帧两次拷贝**  
   `dma_recv_buff → rx_fifo → recv_buff`。当前帧长度很小，开销可接受。

6. **队列槽位固定为 4**  
   如需更高突发承载能力，可改为 8 或按实例配置，但会增加 RAM。

## 18. 验证建议

1. 全量构建通过；
2. 遥控器、裁判系统、视觉串口、HC05、总线舵机数据正常；
3. 连续发送多帧，确认队列不丢帧；
4. 人为增加解析耗时，观察 `rx_fifo_overflow`；
5. 测试 18B、25B、255B 等不同帧长；
6. 测试环形回绕和队列满；
7. 测试串口错误和 daemon 离线恢复；
8. 测量改造前后的 USART ISR 时间；
9. 检查运行时 newlib 堆占用。

## 19. 完整调用链

```text
RobotInit()
  → RobotCMDInit()
    → RemoteControlInit(&huart3)
      → USARTRegister()
        → CreateCallbackTask("usart_rx", USARTDeferredRx, instance, AboveNormal, 1KB)
        → USARTServiceInit()
          → HAL_UARTEx_ReceiveToIdle_DMA(dma_recv_buff)

USART3 中断
  → HAL_UARTEx_RxEventCallback()
    → USART_RxFifoPush(dma_recv_buff, Size)
    → USARTStartReceive()
    → CallbackTaskNotify()

usart_rx 任务
  → USARTDeferredRx()
    → USART_RxFifoPop()
    → module_callback()
      → RemoteControlRxCallback()
        → DaemonReload()
        → sbus_to_rc()
          → 更新 rc_ctrl[]

RobotCMDTask()
  → 读取 rc_data
```

结论：

- DMA 不再等待任务解析，连续帧可以缓存在环形队列中；
- 模块回调接口保持不变，应用层业务代码不需要修改；
- 代价是每个 USART 实例增加约 1.3KB 运行时内存；
- 队列满时丢弃最新帧，需根据实际负载调整 `USART_RX_FIFO_DEPTH`。

## 20. 相关文件

| 文件 | 作用 |
|---|---|
| `bsp/usart/bsp_usart.c` | 环形缓冲区、ISR 入队、DMA 重启、任务解析 |
| `bsp/usart/bsp_usart.h` | 帧结构、队列深度、USARTInstance 字段 |
| `bsp/bsp_tools.c` | 回调任务创建和线程标志等待 |
| `bsp/bsp_tools.h` | 回调任务句柄和通知接口 |
| `modules/remote/remote_control.c` | 遥控器回调注册 |
| `modules/referee/rm_referee.c` | 裁判系统回调注册 |
| `application/cmd/robot_cmd.c` | 遥控器数据应用 |
| `application/chassis/chassis.c` | 裁判系统数据应用 |
| `application/robot.c` | 启动阶段中断开关时序 |
