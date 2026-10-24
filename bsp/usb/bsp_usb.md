# bsp usb

目前仅支持虚拟串口(CDC/VCP)通信,暂未开发 HID/MSC 等其他内容。

## 收发模型

- **接收**: `CDC_Receive_FS()` 在 USB 中断中调用 `USBRxEvent()`,只把数据追加到 2KB 字节环形缓冲区并通知 `usb_rx` 任务。
  `usb_rx` 任务调用模块注册的 `rx_cbk(available)`;模块通过 `USBRead()` 读取字节流并自行按协议拆帧。
  USB CDC 是字节流,一次 USB 传输不等于一帧,分包/粘包由模块的解析器处理。
  若 `usb_rx` 任务创建失败,`USBNotifyRx()` 会回退到中断中直接调用 `rx_cbk`,避免数据静默丢失。
- **发送**: `USBTransmit()` 把数据拷贝到 512B 发送环形缓冲区,返回 0 表示入队成功,1 表示队列满或参数错误。
  实际发送由 `CDC_TransmitCplt_FS()` 触发的发送完成中断链式驱动;调用者无需保持发送缓冲区有效。

## 溢出处理

- 接收环形缓冲区空间不足时,整包丢弃并置溢出标志。任务清空环形缓冲区后以 `rx_cbk(0)` 通知模块复位解析状态。
- 发送队列空间不足时整帧拒绝,可通过 `USBGetTxDropCount()` 查看丢帧次数;接收丢包次数通过 `USBGetRxDropCount()` 查看。

## 注意

为了增加发送完成和接收完成回调,对 `USB_DEVICE/App/usbd_cdc_if.c/h` 的 `USER CODE` 区做了修改。
`CDC_Receive_FS()` 现在调用 `USBRxEvent()`;重新用 CubeMX 生成代码时需确认这些修改仍然存在。
