# bsp_dwt

DWT 是 Cortex-M 内核中的周期计数器，工程用它提供微秒级的系统时间戳和短延时。`DWT_Init()` 必须在其他模块使用 DWT 之前调用，当前由 `BSPInit()` 以 168MHz 初始化。

## 常用接口

```c
void DWT_Init(uint32_t CPU_Freq_mHz);
float DWT_GetDeltaT(uint32_t *cnt_last);
double DWT_GetDeltaT64(uint32_t *cnt_last);
void DWT_SysTimeUpdate(void);
float DWT_GetTimeline_s(void);
float DWT_GetTimeline_ms(void);
uint64_t DWT_GetTimeline_us(void);
void DWT_Delay(float Delay);
```

### 计算两次调用的时间间隔

```c
static uint32_t cnt;
float deltaT = DWT_GetDeltaT(&cnt);
```

`DWT_GetDeltaT()` 内部使用无符号减法，允许 `CYCCNT` 在两次调用之间回绕；但两次调用间隔必须小于约 25.6 秒（168MHz 下 32 位计数器的回绕周期）。

### 获取系统时间

```c
float now_ms = DWT_GetTimeline_ms();
uint64_t now_us = DWT_GetTimeline_us();
```

- `DWT_GetTimeline_s()` / `DWT_GetTimeline_ms()` 返回 `float`，长时间运行后精度会下降，适合任务周期测量和日志。
- `DWT_GetTimeline_us()` 返回 `uint64_t`，内部使用 64 位周期计数，适合需要微秒级绝对时间的场景。
- 时间快照在临界区内同时读取 `CYCCNT` 和回绕计数，避免任务/中断并发导致时间跳变。
- CPU 频率变化后必须重新调用 `DWT_Init()` 或以新的主频初始化。

### 短延时

```c
DWT_Delay(0.001f); // 约 1ms
```

`DWT_Delay()` 是忙等延时，不依赖 SysTick 中断，因此可在初始化阶段中断关闭时使用；但不应在延时期间执行长任务。

## 代码段计时宏

```c
static float my_func_dt;
TIME_ELAPSE(my_func_dt,
            Function1(var);
            Function2(some, var);
            Function3(your, param);
            );
```

宏会把执行时间写入第一个参数，并通过 RTT 按毫秒整数打印。SEGGER RTT 的 `printf` 不支持 `%f`，因此不要直接把 `float` 传给 `LOGINFO`；需要浮点文本时使用 `Float2Str()`。
