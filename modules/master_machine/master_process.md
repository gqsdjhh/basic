# master_process

<p align='right'>neozng1@hnu.edu.cn</p>

视觉/上位机通信模块。负责 seasky 协议的打包、解析，以及离线检测。通信物理层支持两种模式，由 `application/robot_def.h` 中的宏选择：

- `VISION_USE_VCP`：USB CDC 虚拟串口（当前默认）
- `VISION_USE_UART`：普通 USART + DMA/IDLE 接收

## 数据结构

`Vision_Recv_s` 保存解析后的目标状态和角度：

```c
typedef struct
{
    Fire_Mode_e fire_mode;
    Target_State_e target_state;
    Target_Type_e target_type;

    float pitch;
    float yaw;
} Vision_Recv_s;
```

`Vision_Send_s` 保存回传给视觉的姿态和状态：

```c
typedef struct
{
    Enemy_Color_e enemy_color;
    Work_Mode_e work_mode;
    Bullet_Speed_e bullet_speed;

    float yaw;
    float pitch;
    float roll;
} Vision_Send_s;
```

枚举定义见 `modules/master_machine/master_process.h`。

## 外部接口

```c
Vision_Recv_s *VisionInit(UART_HandleTypeDef *_handle);

void VisionSend(void);

void VisionSetFlag(Enemy_Color_e enemy_color,
                   Work_Mode_e work_mode,
                   Bullet_Speed_e bullet_speed);

void VisionSetAltitude(float yaw, float pitch, float roll);
```

- `VisionInit()`：`VISION_USE_UART` 下使用传入的串口句柄注册 USART 实例；`VISION_USE_VCP` 下忽略该参数并初始化 USB。返回接收数据结构体指针。
- `VisionSend()`：读取 `VisionSetAltitude()` 设置的姿态，通过 seasky 协议打包后发送。VCP 模式下写入 USB 发送环形队列；UART 模式下使用 USART DMA 发送。
- `VisionSetFlag()` / `VisionSetAltitude()`：更新发送数据中的状态和姿态，由应用在发送前调用。

## UART 模式

`VisionInit(&huart1)` 会注册 USART 实例并注册 daemon。串口回调任务取出完整帧后调用 `DecodeVision()`：

```c
static void DecodeVision(void)
{
    uint16_t flag_register;
    DaemonReload(vision_daemon_instance);

    (void)get_protocol_info(vision_usart_instance->recv_buff,
                            vision_usart_instance->rx_len,
                            &flag_register,
                            (uint8_t *)&recv_data.pitch,
                            sizeof(recv_data.pitch) * 2U);
    // TODO: code to resolve flag_register;
}
```

`get_protocol_info()` 会校验帧头 CRC8、数据长度是否超过接收缓冲区、载荷是否超过目标缓冲区，以及整帧 CRC16；任一项失败都会丢弃该帧。

UART 模式的发送使用 `USARTSend(..., USART_TRANSFER_DMA)`。发送队列满时丢弃当前帧并打印日志，已排队帧不会被截断。

## VCP 模式

VCP 是字节流，`DecodeVision(uint16_t available)` 每次从 USB 环形队列读取数据并追加到内部字节缓冲区，然后由 `VisionParseRxStream()` 自行拆帧：

1. 查找帧头 `0xA5`；
2. 校验帧头 CRC8；
3. 读取 `data_length`，计算完整帧长并校验不超过 `VISION_RX_BUFF_SIZE` 和载荷上限；
4. 等待完整帧；
5. 调用 `get_protocol_info()` 校验 CRC16 并解析；
6. 从缓冲区移除已处理帧。

USB 接收溢出时 `USBDeferredRx()` 会清空接收队列并以 `rx_cbk(0)` 通知本模块复位解析状态，避免半帧继续参与解析。

## 协议接口

```c
uint16_t get_protocol_info(uint8_t *rx_buf,
                           uint16_t rx_len,
                           uint16_t *flags_register,
                           uint8_t *rx_data,
                           uint16_t rx_data_size);

void get_protocol_send_data(uint16_t send_id,
                            uint16_t flags_register,
                            float *tx_data,
                            uint8_t float_length,
                            uint8_t *tx_buf,
                            uint16_t *tx_buf_len);
```

`get_protocol_info()` 增加了 `rx_len` 和 `rx_data_size` 参数，用于限制解析范围；UART 和 VCP 两条路径都必须传入实际有效长度。

## 相关文件

| 文件 | 作用 |
|---|---|
| `modules/master_machine/master_process.c` | 初始化、UART/VCP 收发、解析回调 |
| `modules/master_machine/seasky_protocol.c` | seasky 协议打包/解包和 CRC 校验 |
| `modules/master_machine/master_process.h` | 数据结构、枚举和外部接口 |
