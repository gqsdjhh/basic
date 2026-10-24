# buzzer

用于拉响蜂鸣器警报

## 使用范例

```c
Buzzer_config_s buzzer_config ={
        .alarm_level = ALARM_LEVEL_HIGH, //设置警报等级 同一状态下 高等级的响应
        .loudness=  0.4, //设置响度
        .octave=  OCTAVE_1, // 设置音阶
    };
robocmd_alarm = BuzzerRegister(&buzzer_config);

AlarmSetStatus(robocmd_alarm, ALARM_ON);
AlarmSetStatus(robocmd_alarm, ALARM_OFF);

```

`BuzzerInit()` 只初始化 PWM，报警实例需要应用调用 `BuzzerRegister()` 创建。未注册时 `BuzzerTask()` 会跳过空槽位，不会访问空指针；`BuzzerRegister()` 在参数越界或内存分配失败时返回 `NULL`。`AlarmSetStatus()` 也接受 `NULL` 实例并直接忽略。

@todo: 将音阶改为可供选择的搭配 如 Do Re Mi Fa So La Si 自由组合 用户只需输入字符串"DoReMi"即可