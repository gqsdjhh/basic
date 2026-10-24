# FreeRTOS 使用分析

> 本文档基于当前工作区源码整理，说明本工程如何集成 FreeRTOS、任务如何划分、任务间如何通信，以及调度、中断、内存方面的关键细节。
> 相关文档：[工程架构分析](./工程架构分析.md)、[中断数据解析任务化改造说明](./中断数据解析任务化改造说明.md)。

## 1. 总体结论

当前工程使用 **FreeRTOS Kernel V10.3.1 + CMSIS-RTOS2 封装**，内核源码位于 `Middlewares/Third_Party/FreeRTOS/Source`，API 封装为 `CMSIS_RTOS_V2/cmsis_os2.c`。

FreeRTOS 在本工程中主要承担：

1. 抢占式任务调度：姿态解算、电机控制、整车逻辑、UI、守护进程分任务运行；
2. `osDelay()` 节拍：任务按 1ms/5ms/10ms 周期循环；
3. 线程标志/任务通知：`bsp_tools` 的回调任务通过 `osThreadFlagsSet()` 唤醒；
4. 动态内存和栈溢出检查：任务、TCB 从 FreeRTOS heap 分配。

工程没有使用 FreeRTOS 队列、信号量、互斥量或软件定时器作为主要通信手段；应用层通信仍是自定义的 `message_center`、全局数据和 `daemon`。BSP 回调任务使用 CMSIS-RTOS2 的线程标志。

## 2. 内核集成与关键配置

### 2.1 源码与移植层

```text
Middlewares/Third_Party/FreeRTOS/Source/
├── tasks.c / queue.c / list.c / timers.c / event_groups.c / stream_buffer.c
├── CMSIS_RTOS_V2/cmsis_os2.c
└── portable/
    ├── GCC/ARM_CM4F/port.c
    └── MemMang/heap_4.c
```

CMake 由 `cmake/stm32cubemx/CMakeLists.txt` 加入这些源文件。

- 内核版本：V10.3.1
- 移植层：Cortex-M4F，`portable/GCC/ARM_CM4F`
- 内存管理：`heap_4.c`
- API：CMSIS-RTOS2，工程代码使用 `osThreadNew()`、`osDelay()`、`osThreadFlagsSet()` 等
- FPU：`configENABLE_FPU=1`，浮点任务会保存 FPU 上下文

### 2.2 主要配置

配置集中在 `Core/Inc/FreeRTOSConfig.h`：

| 配置项 | 值 |
|---|---:|
| 抢占式调度 | 1 |
| 静态分配 | 0 |
| 动态分配 | 1 |
| 系统节拍 | 1000Hz（1ms） |
| 最大优先级数 | 56 |
| FreeRTOS heap | 65536 字节 |
| 栈溢出检测 | 2 |
| malloc 失败钩子 | 1 |
| 任务通知 | 1 |
| 软件定时器 | 1 |
| 互斥量 / 计数信号量 | 1 / 1 |
| 线程标志（CMSIS-RTOS2） | 1 |
| 运行时间统计 | 0 |

`configASSERT()` 关闭中断后死循环；`vApplicationStackOverflowHook()` 和 `vApplicationMallocFailedHook()` 已实现，前者会打印任务名并停机。

### 2.3 系统节拍与 HAL 时基分离

- FreeRTOS 使用 SysTick，1ms 内核节拍；
- HAL 时基使用 TIM14（`Core/Src/stm32f4xx_hal_timebase_tim.c`）；
- DWT 用于微秒级计时和任务耗时统计，不依赖 RTOS 节拍；
- `DWT_Init(168)` 必须与实际 HCLK 一致（当前 168MHz）。

### 2.4 NVIC 优先级

工程使用 4 位抢占优先级：

- `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`
- `configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x50`

绝大多数外设中断（USART、CAN、DMA、SPI、I2C、USB、EXTI）都配置为优先级 5。因此：

- 这些中断可以调用 FreeRTOS 的 `FromISR` 接口；
- ISR 中不能调用会阻塞的 API；
- BSP 的临界区使用 `BSP_CriticalEnter()/BSP_CriticalExit()`，内部以 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 屏蔽优先级 5 及以上的中断，不再硬编码 `0x50`。

## 3. 启动流程

`main()` 的关键顺序（`Core/Src/main.c`）：

```text
HAL_Init()
SystemClock_Config()
MX_xxx_Init()
osKernelInitialize()
RobotInit()              // 应用、模块、BSP 初始化
MX_FREERTOS_Init()       // 创建 defaultTask
osKernelStart()          // 启动调度器，不再返回
```

`RobotInit()` 会先 `__disable_irq()`，然后执行 `BSPInit()`、应用初始化、`OSTaskInit()`；**不在结尾重新开中断**。中断由 FreeRTOS 启动第一个任务时的移植层开启，避免调度器启动前外设中断调用 `osThreadFlagsSet()`。

启动阶段：

- 所有模块实例、CAN/USART 实例、消息中心话题在调度器启动前注册；
- BSP 回调任务在 `RobotInit()` 中通过 `osThreadNew()` 创建，调度器启动后才运行；
- 初始化中不能使用 `osDelay()`，工程使用 `DWT_Delay()`；
- INS 任务启动后先执行 `INS_Init()`，再进行 1kHz 循环。如果 IMU 初始化失败，当前代码会在任务中持续重试，可能饿死低优先级任务，上板时需关注。

## 4. 任务模型

任务创建集中在 `application/robot_task.h`：

| 任务 | 优先级 | 栈大小 | 周期 | 主要工作 |
|---|---|---:|---|---|
| INS | AboveNormal | 4KB | 1kHz | `INS_Task()` 姿态解算 + `VisionSend()` |
| MOTOR | Normal | 1KB | 1kHz | `MotorControlTask()`：DJI 电机闭环、功率控制、DM 任务 |
| DAEMON | Normal | 512B | 100Hz | `DaemonTask()` + `BuzzerTask()` |
| ROBOT | Normal | 4KB | ~200Hz | `RobotTask()` = `RobotCMDTask()` + `ChassisTask()` |
| UI | Normal | 2KB | 事件式 | 裁判系统 UI 刷新；每次发送会 `osDelay()` |
| default | Normal | 512B | 1kHz 循环 | 初始化 USB 设备后持续 `osDelay(1)` |
| HT 电机任务 | Normal | 512B/台 | 1kHz | 每台 HT04 独立控制/发送，避免 CAN 拥塞 |
| BSP 回调任务 | AboveNormal | 1KB/个 | 事件式 | USART/USB/CAN/SPI/IIC/GPIO 数据解析 |

说明：

- CMSIS-RTOS2 的 `stack_size` 单位是字节，`cmsis_os2.c` 会转换为 `StackType_t` 字数；
- BSP 回调任务由 `CreateCallbackTask()` 动态创建，使用线程标志唤醒；线程标志占用 `bit0~bit30`，最多 31 个；
- 创建失败时 `CreateCallbackTask()` 返回空句柄，USART/CAN/SPI/IIC/GPIO 会回退到中断内直接处理；USB 会回退到 `USBNotifyRx()` 中直接调用 `rx_cbk`。

## 5. 调度与实时性

- `osPriorityAboveNormal` 高于 `osPriorityNormal`；INS 和 BSP 回调任务高于 MOTOR/ROBOT/DAEMON/UI；
- 同优先级任务按时间片轮转；
- 所有周期任务使用相对延时 `osDelay()`，不是 `vTaskDelayUntil()`，实际周期受上一轮执行时间影响；
- 每个任务用 DWT 统计自身执行时间，超时只打印 `LOGERROR`，不做周期补偿；
- USART 回调任务单次最多处理 `USART_RX_FIFO_DEPTH` 帧，剩余帧通过再次通知 + `osDelay(1)` 处理；CAN 回调任务目前会一次排空队列，极端负载下仍需关注 CPU 占用；
- `UI` 任务受 `RefereeSend()` 内部 `osDelay(34)` 限制，实际刷新率远低于 1kHz。

## 6. 任务间通信与同步

### 6.1 应用层 `message_center`

- `PubRegister()` / `SubRegister()` 注册发布者和订阅者；
- `PubPushMessage()` 向所有订阅者推送最新值；
- `SubGetMessage()` 从订阅者队列取数据，队列长度为 1，满时丢弃最旧值；
- 适合控制指令，不适合完整历史消息流；
- 当前没有临界区或内存屏障，假设每个话题单生产者/单消费者；如果以后跨任务或从 ISR 发布，需要增加保护。

### 6.2 `daemon` 软看门狗

- 模块注册 `DaemonInstance`，收到数据后调用 `DaemonReload()`；
- `DaemonTask()` 每 10ms 递减计数器；
- 计数到 0 后调用离线回调；`init_count` 用于上电后的首次等待，默认 100（约 1s）；
- 离线回调在离线期间每 100ms 重复调用，模块可按需重试；
- `DaemonRegister()` 在实例数超过 64 或分配失败时返回 `NULL`。

### 6.3 `bsp_tools` 回调任务

```c
CallbackTaskHandle_t CreateCallbackTask(const char *name,
                                        void (*callback)(void *),
                                        void *argument,
                                        osPriority_t priority,
                                        uint32_t stack_size);

static inline void CallbackTaskNotify(const CallbackTaskHandle_t *handle);
```

任务循环为“先等待标志，再执行回调”：

```c
for (;;)
{
    osThreadFlagsWait(info->signal, osFlagsWaitAll, osWaitForever);
    info->callback(info->argument);
}
```

中断通过 `CallbackTaskNotify()`（内部 `osThreadFlagsSet()`）唤醒任务。USART、USB、CAN、SPI、IIC、GPIO 的协议解析/数据读取均通过该机制移出中断。

### 6.4 全局数据

应用层仍通过全局变量传递部分数据，例如遥控器 `rc_ctrl`、裁判系统 `referee_info`、视觉 `recv_data`。这些数据由 AboveNormal 回调任务更新、Normal 应用任务读取；多字段结构体仍未使用双缓冲或版本号，极端情况下可能出现字段不一致，后续可迁移到更严格的消息机制。

## 7. 中断与任务边界

当前设计原则是“中断只做搬运和通知，解析放到任务”：

- USART：IDLE/DMA 中断把整帧写入 `rx_fifo`，立即重启 DMA，再通知 `usart_rx` 任务；
- CAN：RX FIFO 中断把原始帧写入每总线队列，通知 `can1_rx`/`can2_rx` 任务；
- USB：CDC 接收中断写入 2KB 字节环形缓冲区，通知 `usb_rx` 任务；
- SPI/IIC/GPIO：完成/EXTI 中断只通知对应回调任务，传感器读取和协议解析在任务中执行。

因此：

- 中断执行时间与协议复杂度解耦；
- 回调任务优先级高于应用任务，数据更新后应用任务能尽快看到；
- 回调任务中仍然不能阻塞；阻塞式传感器读取需要评估超时时间。

## 8. 内存管理

### 8.1 FreeRTOS heap

- `configTOTAL_HEAP_SIZE = 65536` 字节；
- 所有 `osThreadNew()` 创建的任务栈、TCB 和内核对象从该堆分配；
- Idle 任务由 CMSIS-RTOS2 使用静态内存提供；
- 已开启 `configCHECK_FOR_STACK_OVERFLOW=2` 和 `vApplicationStackOverflowHook()`；
- 已开启 `configUSE_MALLOC_FAILED_HOOK` 和 `vApplicationMallocFailedHook()`。

粗算任务栈：INS 4KB + MOTOR 1KB + DAEMON 512B + ROBOT 4KB + UI 2KB + default 512B ≈ 12KB，再叠加 BSP 回调任务（默认 1KB/个）、HT 电机任务和 TCB，64KB 堆仍有余量，但新增模块时应使用 `xPortGetFreeHeapSize()` 检查。

### 8.2 newlib/C 库堆

模块实例通过 `malloc()` 从 newlib 堆分配，与 FreeRTOS heap 分离：

- `USARTInstance`（含收发环形缓冲区，约 2.6KB/实例）；
- CAN、SPI、IIC、GPIO、电机、daemon、消息中心节点等；
- 链接脚本 `STM32F407xx_FLASH.ld` 只保留 `_Min_Heap_Size = 0x200`、`_Min_Stack_Size = 0x400` 作为检查值，实际 newlib 堆可增长到栈限制。

RAM 统计中的 `RAM used` 不包含运行时 newlib 堆分配，评估内存时需单独考虑。

### 8.3 已知风险

- `INS_Init()` 在 IMU 初始化失败时会持续重试，可能阻塞低优先级任务；
- `message_center` 没有线程安全保护；
- 部分模块直接使用全局数据，缺少双缓冲/版本号；
- 新增实例未检查 `malloc()` 返回值的代码仍存在，建议统一补充；
- CAN 接收任务一次排空队列，极端负载下可能长时间占用 AboveNormal 优先级。

## 9. 相关文件

| 文件 | 作用 |
|---|---|
| `Core/Inc/FreeRTOSConfig.h` | 内核配置 |
| `Core/Src/main.c` | 启动顺序、`osKernelInitialize()` |
| `Core/Src/freertos.c` | defaultTask、栈溢出/malloc 失败钩子 |
| `application/robot_task.h` | 应用任务创建和循环 |
| `application/robot.c` | `RobotInit()` 中断时序 |
| `bsp/bsp_tools.c` / `bsp/bsp_tools.h` | 回调任务、线程标志、BSP 临界区 |
| `modules/daemon/daemon.c` | 软看门狗 |
| `modules/message_center/message_center.c` | 应用层发布/订阅 |
