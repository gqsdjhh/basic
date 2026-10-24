/**
 * @file master_process.c
 * @author neozng
 * @brief  module for recv&send vision data
 * @version beta
 * @date 2022-11-03
 * @todo 增加对串口调试助手协议的支持,包括vofa和serial debug
 * @copyright Copyright (c) 2022
 *
 */
#include <string.h>

#include "bsp_log.h"
#include "daemon.h"
#include "master_process.h"
#include "robot_def.h"
#include "seasky_protocol.h"

static Vision_Recv_s recv_data;
static Vision_Send_s send_data;
static DaemonInstance *vision_daemon_instance;

void VisionSetFlag(Enemy_Color_e enemy_color, Work_Mode_e work_mode, Bullet_Speed_e bullet_speed)
{
  send_data.enemy_color = enemy_color;
  send_data.work_mode = work_mode;
  send_data.bullet_speed = bullet_speed;
}

void VisionSetAltitude(float yaw, float pitch, float roll)
{
  send_data.yaw = yaw;
  send_data.pitch = pitch;
  send_data.roll = roll;
}

/**
 * @brief 离线回调函数,将在daemon.c中被daemon task调用
 * @attention 由于HAL库的设计问题,串口开启DMA接收之后同时发送有概率出现__HAL_LOCK()导致的死锁,使得无法
 *            进入接收中断.通过daemon判断数据更新,重新调用服务启动函数以解决此问题.
 *
 * @param id vision_usart_instance的地址,此处没用.
 */
static void VisionOfflineCallback(void *id)
{
#ifdef VISION_USE_UART
  USARTServiceInit(vision_usart_instance);
#endif // !VISION_USE_UART
  LOGWARNING("[vision] vision offline, restart communication.");
}

#ifdef VISION_USE_UART

#include "bsp_usart.h"

static USARTInstance *vision_usart_instance;

/**
 * @brief 接收解包回调函数,将在bsp_usart.c中被usart rx callback调用
 * @todo  1.提高可读性,将get_protocol_info的第四个参数增加一个float类型buffer
 *        2.添加标志位解码
 */
static void DecodeVision()
{
  uint16_t flag_register;
  DaemonReload(vision_daemon_instance); // 喂狗
  (void)get_protocol_info(vision_usart_instance->recv_buff,
                          vision_usart_instance->rx_len,
                          &flag_register,
                          (uint8_t *)&recv_data.pitch,
                          sizeof(recv_data.pitch) * 2U);
  // TODO: code to resolve flag_register;
}

Vision_Recv_s *VisionInit(UART_HandleTypeDef *_handle)
{
  USART_Init_Config_s conf;
  conf.module_callback = DecodeVision;
  conf.recv_buff_size = VISION_RECV_SIZE;
  conf.usart_handle = _handle;
  vision_usart_instance = USARTRegister(&conf);

  // 为master process注册daemon,用于判断视觉通信是否离线
  Daemon_Init_Config_s daemon_conf = {
      .callback = VisionOfflineCallback, // 离线时调用的回调函数,会重启串口接收
      .owner_id = vision_usart_instance,
      .reload_count = 10,
  };
  vision_daemon_instance = DaemonRegister(&daemon_conf);

  return &recv_data;
}

/**
 * @brief 发送函数
 *
 * @param send 待发送数据
 *
 */
void VisionSend()
{
  // buff和txlen必须为static,才能保证在函数退出后不被释放,使得DMA正确完成发送
  // 析构后的陷阱需要特别注意!
  static uint16_t flag_register;
  static uint8_t send_buff[VISION_SEND_SIZE];
  static uint16_t tx_len;
  // TODO: code to set flag_register
  flag_register = 30 << 8 | 0b00000001;
  // 将数据转化为seasky协议的数据包
  get_protocol_send_data(0x02, flag_register, &send_data.yaw, 3, send_buff, &tx_len);
  USARTSend(vision_usart_instance, send_buff, tx_len, USART_TRANSFER_DMA); // 和视觉通信使用IT,防止和接收使用的DMA冲突
                                                                           // 此处为HAL设计的缺陷,DMASTOP会停止发送和接收,导致再也无法进入接收中断.
                                                                           // 也可在发送完成中断中重新启动DMA接收,但较为复杂.因此,此处使用IT发送.
                                                                           // 若使用了daemon,则也可以使用DMA发送.
}

#endif // VISION_USE_UART

#ifdef VISION_USE_VCP

#include "bsp_usb.h"

#define VISION_RX_BUFF_SIZE 64U
#define VISION_RX_PAYLOAD_SIZE (sizeof(float) * 2U)

static uint8_t vision_rx_buffer[VISION_RX_BUFF_SIZE];
static uint16_t vision_rx_length;

static void VisionResetRxParser(void)
{
  vision_rx_length = 0U;
}

static void VisionParseRxStream(void)
{
  while (vision_rx_length > 0U)
  {
    if (vision_rx_buffer[0] != PROTOCOL_CMD_ID)
    {
      uint16_t sof_index = 1U;
      while ((sof_index < vision_rx_length) && (vision_rx_buffer[sof_index] != PROTOCOL_CMD_ID))
        sof_index++;

      memmove(vision_rx_buffer, &vision_rx_buffer[sof_index], vision_rx_length - sof_index);
      vision_rx_length -= sof_index;

      if (vision_rx_length < 4U)
        return;
    }

    if (vision_rx_length < 4U)
      return;

    if (Get_CRC8_Check(vision_rx_buffer, 3U) != vision_rx_buffer[3])
    {
      memmove(vision_rx_buffer, &vision_rx_buffer[1], vision_rx_length - 1U);
      vision_rx_length--;
      continue;
    }

    uint16_t data_length = (uint16_t)vision_rx_buffer[1] | ((uint16_t)vision_rx_buffer[2] << 8);
    uint32_t frame_length = (uint32_t)OFFSET_BYTE + data_length;

    if ((data_length < 2U) ||
        ((data_length - 2U) > VISION_RX_PAYLOAD_SIZE) ||
        (frame_length > VISION_RX_BUFF_SIZE))
    {
      memmove(vision_rx_buffer, &vision_rx_buffer[1], vision_rx_length - 1U);
      vision_rx_length--;
      continue;
    }

    if (vision_rx_length < frame_length)
      return;

    uint16_t flag_register;
    uint16_t cmd_id = get_protocol_info(vision_rx_buffer,
                                        (uint16_t)frame_length,
                                        &flag_register,
                                        (uint8_t *)&recv_data.pitch,
                                        VISION_RX_PAYLOAD_SIZE);
    if (cmd_id != 0U)
    {
      if (vision_daemon_instance != NULL)
        DaemonReload(vision_daemon_instance);
      // TODO: code to resolve flag_register;
      (void)flag_register;
    }

    memmove(vision_rx_buffer, &vision_rx_buffer[frame_length], vision_rx_length - frame_length);
    vision_rx_length -= (uint16_t)frame_length;
  }
}

static void DecodeVision(uint16_t available)
{
  if (available == 0U)
  {
    VisionResetRxParser();
    return;
  }

  while (USBAvailable() > 0U)
  {
    uint16_t space = (uint16_t)VISION_RX_BUFF_SIZE - vision_rx_length;

    if (space == 0U)
    {
      VisionParseRxStream();
      space = (uint16_t)VISION_RX_BUFF_SIZE - vision_rx_length;
      if (space == 0U)
      {
        VisionResetRxParser();
        continue;
      }
    }

    uint16_t read_length = USBRead(&vision_rx_buffer[vision_rx_length], space);
    if (read_length == 0U)
      break;

    vision_rx_length += read_length;
    VisionParseRxStream();
  }
}

/* 视觉通信初始化 */
Vision_Recv_s *VisionInit(UART_HandleTypeDef *_handle)
{
  UNUSED(_handle); // 仅为了消除警告

  VisionResetRxParser();

  USB_Init_Config_s conf = {.rx_cbk = DecodeVision};
  USBInit(conf);

  // 为master process注册daemon,用于判断视觉通信是否离线
  Daemon_Init_Config_s daemon_conf = {
      .callback = VisionOfflineCallback, // 离线时调用的回调函数,会重启串口接收
      .owner_id = NULL,
      .reload_count = 5, // 50ms
  };
  vision_daemon_instance = DaemonRegister(&daemon_conf);

  return &recv_data;
}

void VisionSend()
{
  uint8_t send_buff[VISION_SEND_SIZE];
  uint16_t flag_register;
  uint16_t tx_len;

  // TODO: code to set flag_register
  flag_register = 30 << 8 | 0b00000001;
  // 将数据转化为seasky协议的数据包
  get_protocol_send_data(0x02, flag_register, &send_data.yaw, 3, send_buff, &tx_len);
  (void)USBTransmit(send_buff, tx_len);
}

#endif // VISION_USE_VCP
