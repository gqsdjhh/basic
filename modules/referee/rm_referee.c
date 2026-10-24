/**
 * @file rm_referee.C
 * @author kidneygood (you@domain.com)
 * @brief
 * @version 0.1
 * @date 2022-11-18
 *
 * @copyright Copyright (c) 2022
 *
 */

#include "rm_referee.h"
#include "bsp_log.h"
#include "bsp_usart.h"
#include "cmsis_os2.h"
#include "crc_ref.h"
#include "daemon.h"
#include "string.h"
#include "task.h"

#define RE_RX_BUFFER_SIZE 255u // 裁判系统接收缓冲区大小

static USARTInstance *referee_usart_instance; // 裁判系统串口实例
static DaemonInstance *referee_daemon;        // 裁判系统守护进程
static referee_info_t referee_info;           // 裁判系统数据

/**
 * @brief  读取裁判数据,在回调任务中解析
 * @param  buff: 读取到的裁判系统原始数据
 * @param  available: buff中有效字节数
 * @attention  在此判断帧头和CRC校验,无误再写入数据，不重复判断帧头
 */
static void JudgeReadData(uint8_t *buff, uint16_t available)
{
  if ((buff == NULL) || (available < (LEN_HEADER + LEN_CMDID + LEN_TAIL)))
    return;

  // 写入帧头数据(5-byte),用于判断是否开始存储裁判数据
  memcpy(&referee_info.FrameHeader, buff, LEN_HEADER);

  // 判断帧头数据(0)是否为0xA5
  if (buff[SOF] != REFEREE_SOF)
    return;

  // 帧头CRC8校验
  if (Verify_CRC8_Check_Sum(buff, LEN_HEADER) != TRUE)
    return;

  uint16_t data_length = referee_info.FrameHeader.DataLength;
  uint32_t judge_length = (uint32_t)data_length + LEN_HEADER + LEN_CMDID + LEN_TAIL;

  // 帧长度必须落在当前有效数据范围内
  if (judge_length > available)
    return;

  // 帧尾CRC16校验
  if (Verify_CRC16_Check_Sum(buff, judge_length) != TRUE)
    return;

  // 2个8位拼成16位int
  referee_info.CmdID = (buff[6] << 8 | buff[5]);

  // 解析数据命令码,将数据拷贝到相应结构体中(注意拷贝数据的长度)
  // 第8个字节开始才是数据 data=7
#define JUDGE_COPY(dst, len)                                       \
  do                                                               \
  {                                                                \
    if ((uint16_t)(judge_length - DATA_Offset) >= (uint16_t)(len)) \
      memcpy(&(dst), buff + DATA_Offset, (len));                   \
  } while (0)

  switch (referee_info.CmdID)
  {
    case ID_game_state: // 0x0001
      JUDGE_COPY(referee_info.GameState, LEN_game_state);
      break;
    case ID_game_result: // 0x0002
      JUDGE_COPY(referee_info.GameResult, LEN_game_result);
      break;
    case ID_game_robot_survivors: // 0x0003
      JUDGE_COPY(referee_info.GameRobotHP, LEN_game_robot_HP);
      break;
    case ID_event_data: // 0x0101
      JUDGE_COPY(referee_info.EventData, LEN_event_data);
      break;
    case ID_referee_warning_t: // 0x0104
      JUDGE_COPY(referee_info.WarnData, LEN_referee_warning_t);
      break;
    case ID_dart_info_t: // 0x0105
      JUDGE_COPY(referee_info.DartData, LEN_dart_info_t);
      break;
    case ID_game_robot_state: // 0x0201
      JUDGE_COPY(referee_info.GameRobotState, LEN_game_robot_state);
      break;
    case ID_power_heat_data: // 0x0202
      JUDGE_COPY(referee_info.PowerHeatData, LEN_power_heat_data);
      break;
    case ID_game_robot_pos: // 0x0203
      JUDGE_COPY(referee_info.GameRobotPos, LEN_game_robot_pos);
      break;
    case ID_buff_musk: // 0x0204
      JUDGE_COPY(referee_info.BuffMusk, LEN_buff_musk);
      break;
    case ID_robot_hurt: // 0x0206
      JUDGE_COPY(referee_info.RobotHurt, LEN_robot_hurt);
      break;
    case ID_shoot_data: // 0x0207
      JUDGE_COPY(referee_info.ShootData, LEN_shoot_data);
      break;
    case ID_projectile_allowance_t: // 0x0208
      JUDGE_COPY(referee_info.AllowanceData, LEN_projectile_allowance_t);
      break;
    case ID_rfid_status_t: // 0x0209
      JUDGE_COPY(referee_info.RFIDdata, LEN_rfid_status_t);
      break;
    case ID_student_interactive: // 0x0301
      JUDGE_COPY(referee_info.ReceiveData, LEN_receive_data);
      break;
    case ID_map_interact_data: // 0x0303
      JUDGE_COPY(referee_info.MapData, LEN_map_interact_data);
      break;
    default:
      break;
  }
#undef JUDGE_COPY

  // 指向下一帧,若仍为0xA5则继续解析
  uint8_t *next_frame = buff + judge_length;
  uint16_t remaining = available - (uint16_t)judge_length;
  if ((remaining > 0U) && (*next_frame == REFEREE_SOF))
    JudgeReadData(next_frame, remaining);
}

/*裁判系统串口接收回调函数,解析数据 */
static void RefereeRxCallback()
{
  DaemonReload(referee_daemon);
  JudgeReadData(referee_usart_instance->recv_buff, referee_usart_instance->rx_len);
}
// 裁判系统丢失回调函数,重新初始化裁判系统串口
static void RefereeLostCallback(void *arg)
{
  USARTServiceInit(referee_usart_instance);
  LOGWARNING("[rm_ref] lost referee data");
}

/* 裁判系统通信初始化 */
referee_info_t *RefereeInit(UART_HandleTypeDef *referee_usart_handle)
{
  USART_Init_Config_s conf;
  conf.module_callback = RefereeRxCallback;
  conf.usart_handle = referee_usart_handle;
  conf.recv_buff_size = RE_RX_BUFFER_SIZE; // mx 255(u8)
  referee_usart_instance = USARTRegister(&conf);

  Daemon_Init_Config_s daemon_conf = {
      .callback = RefereeLostCallback,
      .owner_id = referee_usart_instance,
      .reload_count = 30, // 0.3s没有收到数据,则认为丢失,重启串口接收
  };
  referee_daemon = DaemonRegister(&daemon_conf);

  return &referee_info;
}

/**
 * @brief 裁判系统数据发送函数
 * @param
 */
void RefereeSend(uint8_t *send, uint16_t tx_len)
{
  USARTSend(referee_usart_instance, send, tx_len, USART_TRANSFER_DMA);
  osDelay(34);
}
