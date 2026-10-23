/**
 * @file bsp_usb.h
 * @author your name (you@domain.com)
 * @brief 提供对usb vcp(virtal com port)的操作接口,hid和msf考虑后续添加
 * @attention 这一版usb修改了usbd_cdc_if.c中的CDC_Receive_FS函数,若使用cube生成后会被覆盖.后续需要由usbcdciftemplate创建一套自己的模板
 * @version 0.2
 * @date 2023-02-09
 *
 * @copyright Copyright (c) 2023
 *
 */
#pragma once
#include <stdint.h>

#include "usb_device.h"
#include "usbd_cdc.h"
#include "usbd_conf.h"
#include "usbd_desc.h"
#include "usbd_cdc_if.h"

/* 接收环形缓冲区与发送环形缓冲区的大小,必须为2的幂 */
#define USB_RX_RING_SIZE 2048U
#define USB_TX_RING_SIZE 512U

typedef struct
{
    USBCallback tx_cbk; // 发送完成回调,在USB中断中调用,参数为本次完成的字节数
    USBCallback rx_cbk; // 有新的接收数据时调用(任务上下文),参数为当前可读字节数;参数为0表示接收溢出,模块应复位解析状态
} USB_Init_Config_s;

/**
 * @brief 初始化USB虚拟串口,注册收发回调并创建接收处理任务
 *
 * @param usb_conf 回调配置
 */
void USBInit(USB_Init_Config_s usb_conf);

/**
 * @brief USB接收中断入口,由CDC_Receive_FS调用
 *
 * @param buffer 本次USB传输收到的数据
 * @param len 数据长度
 */
void USBRxEvent(uint8_t *buffer, uint32_t len);

/**
 * @brief 将一帧数据放入USB发送环形缓冲区,实际发送由发送完成中断链式驱动
 *
 * @param buffer 待发送数据,函数返回后调用者可释放该缓冲区
 * @param len 数据长度
 * @return uint8_t 0表示入队成功,1表示参数错误或发送队列已满
 */
uint8_t USBTransmit(uint8_t *buffer, uint16_t len);

/**
 * @brief 获取接收环形缓冲区中可读的字节数
 *
 * @return uint16_t 可读字节数
 */
uint16_t USBAvailable(void);

/**
 * @brief 从接收环形缓冲区读取数据,只应由rx_cbk所在任务调用
 *
 * @param buffer 目标缓冲区
 * @param len 最多读取的字节数
 * @return uint16_t 实际读取的字节数
 */
uint16_t USBRead(uint8_t *buffer, uint16_t len);

/**
 * @brief 获取接收溢出(丢包)次数
 */
uint32_t USBGetRxDropCount(void);

/**
 * @brief 获取发送队列满导致的丢帧次数
 */
uint32_t USBGetTxDropCount(void);
