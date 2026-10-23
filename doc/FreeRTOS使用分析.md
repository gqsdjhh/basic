# FreeRTOS 使用分析

> 本文档基于当前工作区源码整理，重点说明本工程如何集成 FreeRTOS、任务如何划分、任务间如何通信，以及调度、中断、内存方面的关键细节。
> 相关文档：[工程架构分析](./工程架构分析.md)、[架构介绍与开发指南](./架构介绍与开发指南.md)、[README](../README.md)。

## 1. 总体结论

当前工程使用的是 **FreeRTOS Kernel V10.3.1 + STM32 的 CMSIS-OS v1 封装**（`Inc/FreeRTOSConfig.h:3`）。FreeRTOS 在这里主要承担两件事：

1. 提供**抢占式任务调度**，把姿态解算、电机控制、整车逻辑、UI、守护进程拆成不同任务；
2. 提供 **`osDelay()` 节拍**，让各任务按 1ms/5ms/10ms 的周期循环。

工程**没有使用** FreeRTOS 的队列、信号量、互斥量、事件组、任务通知和软件定时器。任务间通信主要依靠自定义的 `message_center`、全局变量和软看门狗 `daemon`。

## 2. 内核集成与关键配置

### 2.1 源码与移植层

FreeRTOS 源码位于：

```text
Middlewares/Third_Party/FreeRTOS/Source/
├── tasks.c / queue.c / list.c / timers.c / event_groups.c / stream_buffer.c
├── CMSIS_RTOS/cmsis_os.c
└── portable/
    ├── GCC/ARM_CM4F/port.c
    └── MemMang/heap_4.c
```

Makefile 中明确加入了这些文件（`Makefile:94-103`），CMake 则通过 `Middlewares/*.c` 递归收集（`CMakeLists.txt:71-78`）。

- 内核版本：V10.3.1
- 内核移植层：Cortex-M4F，`portable/GCC/ARM_CM4F`
- 内存管理：`heap_4.c`
- API 封装：CMSIS-OS v1，工程代码主要调用 `osThreadCreate()`、`osDelay()`、`osThreadTerminate()`、`osSignalWait()` 等
- FPU：开启（`configENABLE_FPU=1`），因此浮点运算任务会保存 FPU 上下文

### 2.2 主要配置

配置集中在 `Inc/FreeRTOSConfig.h`：

| 配置项 | 值 | 位置 |
|---|---:|---|
| 抢占式调度 | 1 | `FreeRTOSConfig.h:58` |
| 静态内存分配 | 1 | `FreeRTOSConfig.h:59` |
| 动态内存分配 | 1 | `FreeRTOSConfig.h:60` |
| Idle/Tick Hook | 关闭 | `FreeRTOSConfig.h:61-62` |
| CPU 频率 | `SystemCoreClock` | `FreeRTOSConfig.h:63` |
| 系统节拍 | 1000Hz（1ms） | `FreeRTOSConfig.h:64` |
| 最大优先级数 | 7（0~6） | `FreeRTOSConfig.h:65` |
| 最小任务栈 | 128 字 | `FreeRTOSConfig.h:66` |
| FreeRTOS 堆大小 | 20000 字节 | `FreeRTOSConfig.h:67` |
| 最大任务名长度 | 32 | `FreeRTOSConfig.h:68` |
| 互斥量 | 开启 | `FreeRTOSConfig.h:70` |
| 计数信号量 | 开启 | `FreeRTOSConfig.h:72` |
| 软件定时器 | 未定义，默认关闭 | FreeRTOS 默认值 |
| 栈溢出检测 | 未开启 | FreeRTOS 默认值 |
| 运行时间统计 | 未开启 | FreeRTOS 默认值 |

`configASSERT()` 的实现是关中断后死循环（`FreeRTOSConfig.h:123`），因此内核断言失败时系统会直接停住。

### 2.3 系统节拍与 HAL 时基分离

工程做了一个关键处理：**FreeRTOS 使用 SysTick，HAL 使用 TIM14**。

- `configTICK_RATE_HZ=1000`，SysTick 产生 1ms 内核节拍；
- `xPortSysTickHandler` 映射到 `SysTick_Handler`（`FreeRTOSConfig.h:134`）；
- HAL 时基改为 TIM14，`HAL_InitTick()` 在 `Src/stm32f4xx_hal_timebase_tim.c:41-112` 中实现；
- TIM14 中断优先级为 15（`Src/tim.c:396`）；
- PendSV 优先级设为 15（`Src/stm32f4xx_hal_msp.c:75`）。

因此：

- `osDelay()` 使用 FreeRTOS 的 1ms 节拍；
- `HAL_Delay()` 使用 TIM14 的 1ms 计数；
- 高精度计时和任务耗时统计使用 DWT（`bsp/dwt/bsp_dwt.c`），不依赖 RTOS 节拍。

### 2.4 NVIC 优先级

工程使用 4 位抢占优先级（`HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4)`，`Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c:173`）。

- `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`
- `configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x50`

绝大多数外设中断都配置为优先级 5：

- USART1/3/6：`Src/usart.c:194/240/310`
- CAN1/CAN2：`Src/can.c:125-166`
- DMA：`Src/dma.c:48-72`
- SPI/I2C/USB/EXTI：优先级同样为 5

这意味着所有外设中断都处在“可以调用 `FromISR` API”的边界上。不过当前工程的中断回调里**没有调用任何 FreeRTOS `FromISR` API**。

## 3. 启动流程

`main()` 的执行顺序如下（`Src/main.c:78-143`）：

```text
HAL_Init()
SystemClock_Config()
MX_xxx_Init()                 // CubeMX 外设初始化
RobotInit()                   // 应用、模块、BSP 初始化
MX_FREERTOS_Init()            // 创建 defaultTask
osKernelStart()               // 启动调度器，不再返回
```

### 3.1 `RobotInit()` 在调度器启动前完成大量初始化

`RobotInit()`（`application/robot.c:23-46`）执行：

```c
__disable_irq();

BSPInit();           // DWT + RTT 日志
RobotCMDInit();      // 遥控器、视觉、消息中心
ChassisInit();       // 底盘电机、裁判系统、超级电容
OSTaskInit();        // 创建 INS/MOTOR/DAEMON/ROBOT/UI 任务

__enable_irq();
```

这个阶段有几个重要特点：

- 初始化期间**关闭全局中断**；
- 所有模块实例、CAN/USART 实例、消息中心话题都在此阶段注册；
- 任务通过 `osThreadCreate()` 在调度器启动前创建，创建后处于就绪状态，但不会运行；
- 初始化中不能使用 `osDelay()` 等 RTOS 延时，工程使用 `DWT_Delay()` 忙等；
- 因此 `RobotInit()` 中如果进行较长的传感器初始化，会直接延长启动时间。

### 3.2 `MX_FREERTOS_Init()` 创建 USB 初始化任务

`Src/freertos.c:83-114` 创建了 `defaultTask`：

```c
osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);
```

`StartDefaultTask()` 只做一件事：

```c
MX_USB_DEVICE_Init();
osThreadTerminate(NULL);
```

即把 USB 设备初始化推迟到调度器启动之后，初始化完成后删除自身（`Src/freertos.c:123-129`）。

### 3.3 启动阶段的任务执行顺序

INS 任务优先级为 `AboveNormal`，且 `StartINSTASK()` 在进入循环前调用 `INS_Init()`。`INS_Init()` 中会：

- 循环等待 BMI088 初始化完成；
- 用 100 次采样、每次 `DWT_Delay(0.001)` 计算初始姿态，约 100ms（`modules/imu/ins_task.c:60-67`）。

由于 INS 优先级高于其他任务，且初始化期间没有调用 `osDelay()` 让出 CPU，**其他任务（包括 USB 初始化任务）会被推迟到 INS 初始化完成后才开始运行**。这是启动阶段的一个实际行为。

## 4. 任务模型

任务创建集中在 `application/robot_task.h:36-55`：

```c
void OSTaskInit()
{
    osThreadDef(instask, StartINSTASK, osPriorityAboveNormal, 0, 1024);
    insTaskHandle = osThreadCreate(osThread(instask), NULL);

    osThreadDef(motortask, StartMOTORTASK, osPriorityNormal, 0, 256);
    motorTaskHandle = osThreadCreate(osThread(motortask), NULL);

    osThreadDef(daemontask, StartDAEMONTASK, osPriorityNormal, 0, 128);
    daemonTaskHandle = osThreadCreate(osThread(daemontask), NULL);

    osThreadDef(robottask, StartROBOTTASK, osPriorityNormal, 0, 1024);
    robotTaskHandle = osThreadCreate(osThread(robottask), NULL);

    osThreadDef(uitask, StartUITASK, osPriorityNormal, 0, 512);
    uiTaskHandle = osThreadCreate(osThread(uitask), NULL);

    HTMotorControlInit();
}
```

### 4.1 任务清单

| 任务 | 创建位置 | CMSIS 优先级 | FreeRTOS 优先级 | 栈参数 | 标称周期 | 主要工作 |
|---|---|---|---:|---:|---:|---|
| INS | `robot_task.h:38-39` | `osPriorityAboveNormal` | 4 | 1024 字（约 4KB） | 1kHz | `INS_Task()` 姿态解算 + `VisionSend()` |
| MOTOR | `robot_task.h:42-43` | `osPriorityNormal` | 3 | 256 字（约 1KB） | 1kHz | `MotorControlTask()` |
| DAEMON | `robot_task.h:45-46` | `osPriorityNormal` | 3 | 128 字（约 512B） | 100Hz | `DaemonTask()` + `BuzzerTask()` |
| ROBOT | `robot_task.h:48-49` | `osPriorityNormal` | 3 | 1024 字（约 4KB） | 约 200Hz | `RobotCMDTask()` + `ChassisTask()` |
| UI | `robot_task.h:51-52` | `osPriorityNormal` | 3 | 512 字（约 2KB） | 名义 1kHz，实际受发送延时限制 | 裁判系统 UI 刷新 |
| default | `freertos.c:106-107` | `osPriorityNormal` | 3 | 128 字（约 512B） | 一次性 | USB 设备初始化后自删除 |
| HT 电机任务 | `HT04.c:218-219` | `osPriorityNormal` | 3 | 128 字（约 512B） | 1kHz | 每台 HT04 电机独立控制/发送，避免 CAN 拥塞 |

> 关于栈大小：CMSIS 头文件把 `stacksize` 注释为“bytes”（`cmsis_os.h:285`），但 ST 的 `cmsis_os.c` 在调用 `xTaskCreate()` 时直接透传 `thread_def->stacksize`（`cmsis_os.c:213`），而 `xTaskCreate()` 的单位是 `StackType_t`（字）。因此本工程中的 `1024`、`256`、`128` 实际按字解释，分别约为 4KB、1KB、512B。调整栈时要以这个实现为准，不要按“字节”再除以 4。

### 4.2 各任务的具体循环

#### INS 任务

`robot_task.h:57-74`：

```c
INS_Init();

for (;;)
{
    ins_start = DWT_GetTimeline_ms();
    INS_Task();
    ins_dt = DWT_GetTimeline_ms() - ins_start;
    if (ins_dt > 1)
        LOGERROR("[freeRTOS] INS Task is being DELAY! dt = [%f]", &ins_dt);
    VisionSend();
    osDelay(1);
}
```

- 优先级最高，保证姿态解算优先执行；
- 每次循环用 DWT 统计自身执行时间；
- 超过 1ms 只打印错误，不做频率补偿；
- 使用相对延时 `osDelay(1)`，不是 `vTaskDelayUntil()`。

#### MOTOR 任务

`robot_task.h:76-90`：

```c
for (;;)
{
    motor_start = DWT_GetTimeline_ms();
    MotorControlTask();
    motor_dt = DWT_GetTimeline_ms() - motor_start;
    if (motor_dt > 1)
        LOGERROR("[freeRTOS] MOTOR Task is being DELAY! dt = [%f]", &motor_dt);
    osDelay(1);
}
```

`MotorControlTask()` 中包括：

- `DJIMotorControl()`：DJI 电机三环 PID；
- `PowerControl()`：底盘功率控制；
- `DMMotorTask()`：达妙电机控制；
- 其他电机任务当前被注释。

#### DAEMON 任务

`robot_task.h:92-109`：

```c
for (;;)
{
    daemon_start = DWT_GetTimeline_ms();
    DaemonTask();
    BuzzerTask();
    daemon_dt = DWT_GetTimeline_ms() - daemon_start;
    if (daemon_dt > 10)
        LOGERROR("[freeRTOS] Daemon Task is being DELAY! dt = [%f]", &daemon_dt);
    osDelay(10);
}
```

这是软件看门狗任务，不是硬件看门狗。`DaemonTask()` 每 10ms 将所有注册实例的计数器减 1；模块在收到数据时调用 `DaemonReload()` 重置计数。计数到 0 后调用离线回调（`modules/daemon/daemon.c:37-51`）。

典型超时时间：

- 遥控器：`reload_count=10`，约 100ms（`modules/remote/remote_control.c:203-205`）
- 裁判系统：`reload_count=30`，约 300ms（`modules/referee/rm_referee.c:140-144`）
- HT 电机：`reload_count=5`，约 50ms（`modules/motor/HTmotor/HT04.c:121-125`）

#### ROBOT 任务

`robot_task.h:111-126`：

```c
for (;;)
{
    robot_start = DWT_GetTimeline_ms();
    RobotTask();
    robot_dt = DWT_GetTimeline_ms() - robot_start;
    if (robot_dt > 5)
        LOGERROR("[freeRTOS] ROBOT core Task is being DELAY! dt = [%f]", &robot_dt);
    osDelay(5);
}
```

`RobotTask()` 中调用：

```c
RobotCMDTask();   // 解析遥控器/视觉，发布底盘、云台、发射指令
ChassisTask();    // 底盘运动学解算、功率限制、电机设定
```

当前 `GimbalTask()` 和 `ShootTask()` 在 `application/robot.c:50-57` 中被注释，因此实际运行的是 **RobotCMD + Chassis**。

#### UI 任务

`robot_task.h:128-139`：

```c
MyUIInit();

for (;;)
{
    UITask();
    osDelay(1);
}
```

`MyUIInit()` 会等待裁判系统数据：

```c
while (referee_recv_info->GameRobotState.robot_id == 0)
    osDelay(100);
```

如果裁判系统未初始化，任务会自行删除：

```c
if (!referee_recv_info->init_flag)
    vTaskDelete(NULL);
```

另外，UI 数据发送函数中存在阻塞式延时：

```c
void RefereeSend(uint8_t *send, uint16_t tx_len)
{
    USARTSend(..., USART_TRANSFER_DMA);
    osDelay(34);
}
```

位置在 `modules/referee/rm_referee.c:154-157`。因此 UI 任务虽然循环里写的是 `osDelay(1)`，但每发送一包裁判系统数据都会挂起 34ms，**实际刷新率远低于 1kHz，取决于本轮需要发送多少包 UI 数据**。这也是 `robot_task.h:135` 注释所说的“每给裁判系统发送一包数据会挂起一次”。

#### HT 电机任务

HT04 电机每注册一台，就创建一个独立任务（`HT04.c:206-221`）：

```c
for (size_t i = 0; i < idx; i++)
{
    osThreadDef(ht_task_name, HTMotorTask, osPriorityNormal, 0, 128);
    ht_task_handle[i] = osThreadCreate(osThread(ht_task_name), ht_motor_instance[i]);
}
```

`HTMotorTask()` 内部是 1kHz 循环，每次控制后 `osDelay(1)`（`HT04.c:146-203`）。这样每台电机独立发送 CAN 报文，避免多台 HT 电机集中发送造成总线拥塞。

> 小细节：`osThreadDef(ht_task_name, ...)` 宏会把参数名 `ht_task_name` 字符串化，而不是读取数组内容，因此所有 HT 任务名实际上都是 `"ht_task_name"`，不影响功能，但调试时无法通过任务名区分不同电机。

## 5. 调度行为与时间特性

### 5.1 优先级映射

CMSIS-OS v1 的优先级枚举为：

```c
osPriorityIdle        = -3
osPriorityLow         = -2
osPriorityBelowNormal = -1
osPriorityNormal      =  0
osPriorityAboveNormal = +1
osPriorityHigh        = +2
osPriorityRealtime    = +3
```

`cmsis_os.c:103-111` 的映射为：

```c
fpriority = tskIDLE_PRIORITY + (priority - osPriorityIdle);
```

因此：

- `osPriorityNormal` → FreeRTOS 优先级 3
- `osPriorityAboveNormal` → FreeRTOS 优先级 4
- 空闲任务优先级 0
- 最高可用优先级 6

### 5.2 同优先级任务的时间片轮转

工程没有关闭 `configUSE_TIME_SLICING`，FreeRTOS 默认开启时间片（`FreeRTOS.h:793-794`）。因此：

- INS 优先级 4，会抢占所有 Normal 任务；
- MOTOR、ROBOT、DAEMON、UI 全部是 Normal 优先级 3；
- 这些任务之间按 1ms tick 轮转，谁也抢不了谁；
- 每个任务循环末尾使用 `osDelay()`，一旦调用就进入阻塞，直到下一个节拍或更长时间。

这意味着任务表里的 1kHz、200Hz、100Hz 都是**标称上限**。实际频率取决于：

- 本任务执行时间；
- 同优先级任务是否同时就绪；
- 中断负载；
- `osDelay()` 的 tick 对齐。

例如 MOTOR 和 UI 如果都在每个 tick 就绪，它们会轮流获得 CPU，MOTOR 不可能稳定每 1ms 执行一次。当前 INS 被单独提高优先级，正是为了保证姿态解算尽量达到 1kHz；但电机控制与整车控制仍处于同一优先级，存在调度抖动风险。

### 5.3 相对延时而非绝对周期

所有任务都使用 `osDelay(n)`，没有使用 `vTaskDelayUntil()` 或 CMSIS 的 `osDelayUntil()`。`osDelay()` 是“从调用时刻起延迟 n 个 tick”，因此：

```text
实际周期 = 任务执行时间 + 延时时间 + tick 对齐误差
```

而不是严格固定的 1ms/5ms/10ms。这也解释了为什么每个任务都用 DWT 统计自己的执行时间，并在超时后打印 `LOGERROR`。但这些日志只是监测，不会补偿周期。

## 6. 任务间通信与同步

### 6.1 没有使用 FreeRTOS 的 IPC 机制

全工程没有使用：

- `xQueueCreate()` / `osMessageCreate()`
- `xSemaphoreCreateMutex()` / `osMutexCreate()`
- `xSemaphoreCreateBinary()` / `osSemaphoreCreate()`
- 事件组
- 任务通知
- 软件定时器

`FreeRTOSConfig.h` 中虽然开启了互斥量和计数信号量，但代码中没有实际创建它们。

### 6.2 自定义 `message_center`

应用层之间的通信使用 `modules/message_center`，它是一个自定义的伪发布-订阅机制：

- `PubRegister()`：注册/查找话题；
- `SubRegister()`：注册订阅者；
- `PubPushMessage()`：向所有订阅者推送数据；
- `SubGetMessage()`：从订阅者队列读取数据。

每个订阅者的队列长度为 `QUEUE_SIZE=1`（`message_center.h:19`），队列满时丢弃最旧数据，写入最新数据。因此它本质上是“最新值邮箱”，适合控制指令，不适合需要完整历史的消息流。

该实现**不是线程安全的**，没有互斥量或临界区。不过在单板配置下，发布者 `RobotCMDTask()` 和订阅者 `ChassisTask()` 都在同一个 ROBOT 任务中顺序执行，所以当前场景下竞争较少。如果以后把发布和订阅拆到不同任务，或者从 ISR 中发布，就需要额外保护。

### 6.3 软看门狗 `daemon`

`daemon` 是工程自己实现的软件看门狗：

- 模块注册 `DaemonInstance`；
- 收到数据时调用 `DaemonReload()`；
- DAEMON 任务每 10ms 调用 `DaemonTask()` 递减计数；
- 计数到 0 后调用模块注册的离线回调。

离线回调通常做：

- 重新启动串口 DMA 接收；
- 重新使能电机；
- 清空遥控器数据；
- 打印警告。

例如：

- `RefereeLostCallback()`：重新初始化裁判系统串口（`rm_referee.c:125-129`）
- `RCLostCallback()`：清空遥控器数据并重启串口（`remote_control.c:180-185`）
- `HTMotorLostCallback()`：尝试让电机重新进入控制模式（`HT04.c:70-76`）

### 6.4 `bsp_tools` 中的事件驱动任务框架

`bsp/bsp_tools.c` 提供了一个通用的事件驱动任务创建接口：

```c
uint32_t CreateCallbackTask(char *name, void *cbk, void *ins, osPriority priority);
```

它内部创建一个任务，任务循环为：

```c
for (;;)
{
    cbk_func(ins);
    osSignalWait(sig, osWaitForever);
}
```

设计意图是：

- 中断中只调用 `osSignalSet()`；
- 复杂回调放到任务中执行；
- 每个回调任务拥有独立的信号位，最多 32 个。

但当前工程中：

- `CreateCallbackTask()` **没有任何调用者**；
- `osSignalSet()` 也没有被使用。

也就是说，这套“把中断回调升级为任务”的机制目前是**预留但未启用**的。实际中断回调仍然在 ISR 中直接执行协议解析、数据更新等操作。

## 7. 中断与 FreeRTOS 的边界

### 7.1 中断中直接调用模块回调

当前 BSP 的中断回调会直接调用上层模块函数：

- 串口 DMA/IDLE 接收完成：
  - `HAL_UARTEx_RxEventCallback()` 遍历 USART 实例，调用 `module_callback()`；
  - 例如遥控器解析、裁判系统解析、视觉数据解析；
  - 位置：`bsp/usart/bsp_usart.c:219-235`。
- CAN FIFO 接收：
  - `HAL_CAN_RxFifo0MsgPendingCallback()` / `Fifo1` 调用 `CANFIFOxCallback()`；
  - 遍历 CAN 实例，匹配 ID 后调用 `can_module_callback()`；
  - 例如电机反馈解析、超级电容反馈、板间通信；
  - 位置：`bsp/can/bsp_can.c:186-207`、`bsp/can/bsp_can.c:221-234`。
- USB CDC 接收：
  - `CDC_Receive_FS()` 直接调用 `rx_cbk`；
  - 视觉数据解析可能发生在 USB 中断上下文中。
- EXTI：
  - BMI088、IST8310 的 DRDY 中断中直接读取传感器数据。

这些回调中都没有调用 FreeRTOS API，也没有使用 `FromISR` 版本。因此不存在“在错误优先级的中断中调用 FreeRTOS API”的问题。

但这也意味着：

- 协议解析、CRC 校验、数据拷贝都在中断上下文中完成；
- 如果数据量大或解析复杂，会增加中断延迟；
- 更高优先级的 INS 任务虽然能抢占普通任务，但不能抢占优先级 5 的外设中断。

### 7.2 临界区实现

BSP 的 CAN 和 USART 发送队列使用 `BASEPRI` 实现临界区，而不是 FreeRTOS 的 `taskENTER_CRITICAL()`：

```c
uint32_t saved_baspri = __get_BASEPRI();
__set_BASEPRI(0x50U);   // 屏蔽优先级 >= 5 的中断
...
__set_BASEPRI(saved_baspri);
```

位置：

- USART 发送入队/启动：`bsp/usart/bsp_usart.c:83-107`、`bsp/usart/bsp_usart.c:115-153`
- CAN 发送 FIFO 入队/发送：`bsp/can/bsp_can.c:72-95`、`bsp/can/bsp_can.c:98-125`

`0x50` 对应优先级 5，正好屏蔽所有外设中断，防止任务和 ISR 同时操作发送环形队列。这个做法与 FreeRTOS 的 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 保持一致，但它不是 RTOS 临界区，不会影响任务调度器。

### 7.3 共享数据的并发访问

工程中大量数据是“ISR 写、任务读”：

- 遥控器 ISR 更新 `rc_ctrl`，ROBOT 任务读取；
- CAN 接收 ISR 更新电机 `measure`，MOTOR 任务读取；
- 裁判系统 ISR 更新 `referee_info`，UI/底盘任务读取；
- 视觉 ISR 更新 `recv_data`，ROBOT 任务读取。

这些共享结构体大多没有互斥量、双缓冲或版本号。当前依赖：

- Cortex-M4 对 32 位对齐变量读写是原子的；
- 多数数据是独立标量，且控制周期远快于数据更新周期；
- 部分模块使用“最新值”语义，容忍偶尔的旧数据。

但对于多字段结构体，理论上仍可能读到“一半新、一半旧”的数据。这是当前工程比较明显的实时性/一致性风险点。

## 8. 内存管理

### 8.1 FreeRTOS 堆

- `configTOTAL_HEAP_SIZE = 20000`（约 19.5KB）
- 使用 `heap_4.c`，支持分配、释放和相邻空闲块合并
- 所有应用任务通过 `osThreadCreate()` 动态创建，任务栈和 TCB 都从该堆分配
- Idle 任务使用静态分配：
  - `static StaticTask_t xIdleTaskTCBBuffer;`
  - `static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];`
  - 位置：`Src/freertos.c:66-75`

粗略估算主要任务栈占用：

```text
INS      1024 字 ≈ 4KB
MOTOR     256 字 ≈ 1KB
DAEMON    128 字 ≈ 512B
ROBOT    1024 字 ≈ 4KB
UI        512 字 ≈ 2KB
default   128 字 ≈ 512B
```

合计约 12KB，再加 TCB、就绪/延时链表节点等内核对象。若注册 HT 电机，每台再增加约 512B 栈和 TCB。20KB 堆目前还有余量，但没有失败保护。

### 8.2 模块实例使用另一套堆

模块中的 `malloc()` 使用的是 C 库堆，不是 FreeRTOS 的 `pvPortMalloc()`。链接脚本中：

- `_Min_Heap_Size = 0x5000`（20KB）
- `_Min_Stack_Size = 0x5000`（20KB）

位置：`STM32F407IGHx_FLASH.ld:58-59`。

因此系统里实际上有两套动态内存：

1. FreeRTOS heap：任务栈、TCB、内核对象；
2. newlib/C 库 heap：`USARTInstance`、`CANInstance`、电机实例、消息中心节点等。

两者互不影响，但都会占用 RAM。

### 8.3 缺少的保护

当前没有启用：

- `configCHECK_FOR_STACK_OVERFLOW`
- `vApplicationStackOverflowHook()`
- `vApplicationMallocFailedHook()`
- `configGENERATE_RUN_TIME_STATS`
- `configUSE_TRACE_FACILITY`

并且 `osThreadCreate()` 的返回值在 `OSTaskInit()` 和 `HTMotorControlInit()` 中都没有检查。如果 FreeRTOS 堆不足，任务创建会失败，但代码不会立即报错，后续任务句柄可能为 `NULL`。

## 9. 典型运行链路

以单板底盘控制为例：

```text
USART3 DMA/IDLE 中断
    └─ RemoteControlRxCallback()
        └─ sbus_to_rc() 更新 rc_ctrl
            └─ DaemonReload()

ROBOT 任务（约 200Hz）
    ├─ RobotCMDTask()
    │   ├─ 读取 rc_ctrl / 视觉数据
    │   ├─ 计算 chassis_cmd
    │   └─ PubPushMessage("chassis_cmd")
    └─ ChassisTask()
        ├─ SubGetMessage("chassis_cmd")
        ├─ 麦轮运动学解算
        ├─ 功率限制
        └─ DJIMotorSetRef() 设置四个电机目标值

MOTOR 任务（标称 1kHz）
    ├─ DJIMotorControl()
    │   ├─ 读取 CAN 反馈
    │   ├─ PID 计算
    │   └─ CANTransmit()
    └─ PowerControl()

CAN 发送完成中断
    └─ CANSendFIFOTransmit() 继续发送队列中的下一帧

CAN 接收中断
    ├─ 匹配电机 ID
    ├─ 更新电机反馈
    └─ DaemonReload()

DAEMON 任务（100Hz）
    ├─ 递减各模块看门狗计数
    └─ 超时调用离线回调（重启串口、重新使能电机等）

UI 任务
    ├─ 等待裁判系统 robot_id
    ├─ 周期性刷新 UI
    └─ 每次 RefereeSend() 后 osDelay(34)
```

## 10. 设计特点与风险点

### 10.1 优点

1. **任务划分清晰**：姿态、电机、整车逻辑、UI、守护进程各自独立，便于理解和维护。
2. **INS 单独提高优先级**：保证姿态解算优先于普通控制任务。
3. **使用 DWT 监测任务耗时**：每个周期任务都能发现自身超时并打印错误。
4. **软看门狗机制完整**：通信、电机、裁判系统都可注册离线恢复回调。
5. **FreeRTOS 堆和 C 库堆分离**：任务栈与模块实例内存互不干扰。
6. **中断优先级统一为 5**：与 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 对齐，为以后使用 `FromISR` API 留出了空间。

### 10.2 风险与可改进点

1. **MOTOR、ROBOT、DAEMON、UI 同为 Normal 优先级**
   - 电机控制与整车控制没有优先级区分；
   - 同优先级时间片轮转会使电机任务的实际频率低于 1kHz；
   - 如果对控制实时性要求高，建议提高 MOTOR 优先级，或把 ROBOT 降到更低优先级。

2. **全部使用相对延时 `osDelay()`**
   - 没有使用 `vTaskDelayUntil()`，周期会随执行时间漂移；
   - 控制频率不是严格固定的；
   - 对 1kHz 电机控制，建议改为绝对周期延时。

3. **没有使用 FreeRTOS 的队列/通知机制**
   - ISR 中直接解析协议，增加了中断执行时间；
   - `bsp_tools` 的事件任务框架已经写好但未启用；
   - 可以把裁判系统、视觉、遥控器等复杂解析移到任务中，用 `osSignalSet()` 或任务通知唤醒。

4. **共享数据缺少同步**
   - ISR 写、任务读的多字段结构体没有临界区或双缓冲；
   - `message_center` 也不是线程安全的；
   - 当前依靠“最新值”语义和 32 位原子访问，但并非严格安全。

5. **缺少栈溢出和内存分配失败检测**
   - 建议开启 `configCHECK_FOR_STACK_OVERFLOW`；
   - 实现 `vApplicationStackOverflowHook()` 和 `vApplicationMallocFailedHook()`；
   - 检查 `osThreadCreate()` 返回值。

6. **UI 任务不是真正的事件驱动**
   - 虽然注释称事件式，但实际是周期循环 + 每次发送后 `osDelay(34)`；
   - 如果 UI 包较多，刷新率会明显下降；
   - 可以改为“脏标志 + 任务通知”，只在 UI 数据变化时发送。

7. **任务栈单位容易误解**
   - CMSIS 头文件注释为 bytes，ST 实现实际按字传递；
   - 后续调整栈大小时需要以 `cmsis_os.c` 的实际行为为准。

## 11. 关键文件索引

| 内容 | 文件 |
|---|---|
| FreeRTOS 配置 | `Inc/FreeRTOSConfig.h` |
| 任务创建与任务循环 | `application/robot_task.h` |
| 启动与调度器启动 | `Src/main.c`、`application/robot.c` |
| CubeMX 默认任务/USB 初始化 | `Src/freertos.c` |
| CMSIS-OS 封装 | `Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS/cmsis_os.c` |
| Cortex-M4F 移植 | `Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c` |
| 内存管理 | `Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c` |
| 事件任务工具（未启用） | `bsp/bsp_tools.c` |
| 软看门狗 | `modules/daemon/daemon.c` |
| 自定义发布订阅 | `modules/message_center/message_center.c` |
| 串口中断回调 | `bsp/usart/bsp_usart.c` |
| CAN 中断回调 | `bsp/can/bsp_can.c` |
| HAL 时基 | `Src/stm32f4xx_hal_timebase_tim.c` |
| HT 电机独立任务 | `modules/motor/HTmotor/HT04.c` |
