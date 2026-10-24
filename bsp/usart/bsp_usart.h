#ifndef BSP_RC_H
#define BSP_RC_H

#include "bsp_tools.h"
#include "main.h"
#include <stdint.h>

#define DEVICE_USART_CNT 3     // C板至多分配3个串口
#define USART_RXBUFF_LIMIT 256 // 如果协议需要更大的buff,请修改这里
#define USART_TXBUFF_SIZE 512  // 每个串口实例发送环形缓冲区的大小,若经常出现队列满的日志,请修改这里
#define USART_RX_FIFO_DEPTH 4U // 接收帧环形队列槽位数(可用容量为深度-1),必须是2的幂
#define USART_RX_FIFO_MASK (USART_RX_FIFO_DEPTH - 1U)

#if (USART_RX_FIFO_DEPTH < 2U) || ((USART_RX_FIFO_DEPTH & (USART_RX_FIFO_DEPTH - 1U)) != 0U)
#error "USART_RX_FIFO_DEPTH must be a power of two and at least 2"
#endif

// 模块回调函数,用于解析协议
typedef void (*usart_module_callback)();

/* 接收帧,由DMA/IDLE事件产生 */
typedef struct
{
  uint16_t length;
  uint8_t data[USART_RXBUFF_LIMIT];
} USART_RxFrame_t;

/* 发送模式枚举 */
typedef enum
{
  USART_TRANSFER_NONE = 0,
  USART_TRANSFER_BLOCKING,
  USART_TRANSFER_IT,
  USART_TRANSFER_DMA,
} USART_TRANSFER_MODE;

// 串口实例结构体,每个module都要包含一个实例.
// 由于串口是独占的点对点通信,所以不需要考虑多个module同时使用一个串口的情况,因此不用加入id;当然也可以选择加入,这样在bsp层可以访问到module的其他信息
typedef struct
{
  uint8_t recv_buff[USART_RXBUFF_LIMIT];     // 任务解析用buffer,模块回调从此处读取完整帧
  uint8_t dma_recv_buff[USART_RXBUFF_LIMIT]; // DMA直接写入的buffer
  uint16_t recv_buff_size;                   // 模块接收一包数据的最大长度
  UART_HandleTypeDef *usart_handle;          // 实例对应的usart_handle
  usart_module_callback module_callback;     // 解析收到的数据的回调函数

  CallbackTaskHandle_t rx_task; // 接收数据解析任务
  volatile uint16_t rx_len;     // 当前任务的帧长度

  USART_RxFrame_t rx_fifo[USART_RX_FIFO_DEPTH]; // 接收帧环形缓冲区
  volatile uint16_t rx_fifo_head;               // 生产者(ISR)写入位置
  volatile uint16_t rx_fifo_tail;               // 消费者(任务)读取位置
  volatile uint32_t rx_fifo_overflow;           // 接收队列溢出丢帧计数
  volatile uint8_t rx_restart_pending;          // ISR重启接收失败,等待任务补启动

  /* 发送环形缓冲区相关,IT/DMA模式下USARTSend会先将数据入队,由发送完成中断链式驱动发送 */
  uint8_t tx_buff[USART_TXBUFF_SIZE];    // 发送环形缓冲区(队列)
  uint8_t tx_staging[USART_TXBUFF_SIZE]; // 连续化暂存区,供HAL IT/DMA发送直接读取
  volatile uint16_t tx_front;            // 环形缓冲区队头,指向最旧的数据
  volatile uint16_t tx_back;             // 环形缓冲区队尾,指向写入位置
  volatile uint16_t tx_count;            // 环形缓冲区中排队的字节数,用于区分满/空
  volatile uint16_t tx_in_flight;        // 已交给HAL但尚未发送完成的字节数,0表示空闲
  volatile uint32_t tx_drop_count;       // 发送队列满导致的丢帧计数
  USART_TRANSFER_MODE tx_mode;           // 最近一次调用的发送模式,链式续发时使用
} USARTInstance;

/* usart 初始化配置结构体 */
typedef struct
{
  uint16_t recv_buff_size;               // 模块接收一包数据的大小,不得超过USART_RXBUFF_LIMIT
  UART_HandleTypeDef *usart_handle;      // 实例对应的usart_handle
  usart_module_callback module_callback; // 解析收到的数据的回调函数
} USART_Init_Config_s;

/**
 * @brief 注册一个串口实例,返回一个串口实例指针
 *
 * @param init_config 传入串口初始化结构体
 */
USARTInstance *USARTRegister(USART_Init_Config_s *init_config);

/**
 * @brief 启动串口服务,需要传入一个usart实例.一般用于lost callback的情况(使用串口的模块daemon)
 *
 * @param _instance
 */
void USARTServiceInit(USARTInstance *_instance);

/**
 * @brief 通过调用该函数可以发送一帧数据,需要传入一个usart实例,发送buff以及这一帧的长度
 * @note IT/DMA模式下不再直接调用HAL,而是先将数据拷贝至发送环形缓冲区中,由发送完成中断链式驱动发送,
 *       因此短时间内连续调用此接口不会丢帧.
 * @note 队列满时会丢弃当前帧并打印错误日志,已排队帧不会被截断;请控制发送频率或增大USART_TXBUFF_SIZE.
 * @note BLOCKING模式不经过队列直接阻塞发送,请勿与同一实例的IT/DMA模式混用.
 *
 * @param _instance 串口实例
 * @param send_buf 待发送数据的buffer
 * @param send_size how many bytes to send
 */
void USARTSend(USARTInstance *_instance, uint8_t *send_buf, uint16_t send_size, USART_TRANSFER_MODE mode);

/**
 * @brief 判断串口发送是否已完成(队列为空且没有在途数据)
 * @note 发送接口本身是非阻塞的,该函数仅在需要确保上一帧完全发出总线时使用
 *
 * @param _instance 要判断的串口实例
 * @return uint8_t ready 1, busy 0
 */
uint8_t USARTIsReady(USARTInstance *_instance);

#endif
