# daemon

<p align='right'>neozng1@hnu.edu.cn</p>

用于监测模块和应用运行情况的 module（和官方代码中的 detect task 作用相同）。被监测模块周期性调用 `DaemonReload()` 喂狗；超时后 daemon 调用注册的离线回调，可用于停机、重启通信等。

## 使用范例

包含 `daemon.h`，并在使用 daemon 的文件中保存一个 `DaemonInstance *`。

```c
Daemon_Init_Config_s daemon_conf = {
    .reload_count = 10,       // 100ms 未喂狗视为离线
    .init_count = 100,        // 可选：上电后等待 1s 再开始检测
    .callback = MyOfflineCallback,
    .owner_id = my_instance,  // 多实例时用于区分，单实例可为 NULL
};
my_daemon = DaemonRegister(&daemon_conf);
```

初始化参数：

```c
typedef struct
{
    uint16_t reload_count;     // 喂狗时重载的计数值
    uint16_t init_count;       // 上线等待时间，0 表示使用默认值 100
    offline_callback callback; // 离线回调，可为 NULL
    void *owner_id;            // 拥有者实例指针，回调时原样传入
} Daemon_Init_Config_s;
```

- `reload_count`：`DaemonTask()` 每 10ms 递减一次，因此离线时间约为 `reload_count × 10ms`。例如 `reload_count = 30` 约 300ms。
- `init_count`：注册后计数器从该值开始递减，给模块留出上电/首次通信的等待时间；未设置时默认 100（约 1s）。旧版本中该字段会被 `reload_count` 覆盖，当前已修复。
- `callback`：计数器归零后调用；`owner_id` 会作为参数传入，便于多个实例共用同一个回调。
- `owner_id`：通常填模块实例指针，回调中强转回具体类型即可访问该实例。

## 运行机制

- `DaemonTask()` 在 `StartDAEMONTASK` 中每 10ms 调用一次（100Hz）。
- 计数器大于 0 时每次减 1；归零且注册了回调时调用 `callback(owner_id)`。
- 离线期间回调会每 100ms 重复调用，模块可据此持续重试（例如 HT 电机每 10 次重试一次重新使能）；如果只需要一次通知，请在回调内部自行记录状态。
- `DaemonReload()` 会把计数器恢复为 `reload_count`，模块收到数据或完成操作后应调用。
- `DaemonIsOnline()` 在计数器大于 0 时返回 1。
- `DaemonRegister()` 在实例数超过 `DAEMON_MX_CNT`（64）或内存分配失败时返回 `NULL`；`DaemonReload()` / `DaemonIsOnline()` 已做空指针保护。

## 相关任务

`DaemonTask()` 与 `BuzzerTask()` 一起运行在 `StartDAEMONTASK` 中，任务周期 10ms（100Hz）。
