#ifndef BSP_TOOLS_H
#define BSP_TOOLS_H

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "main.h"
#include <stddef.h>

#define CALLBACK_TASK_DEFAULT_STACK_SIZE 1024U

/* 与 FreeRTOS 临界区使用相同的中断屏蔽级别,避免各处硬编码 0x50 */
#define BSP_CRITICAL_BASEPRI ((uint32_t)configMAX_SYSCALL_INTERRUPT_PRIORITY)

static inline uint32_t BSP_CriticalEnter(void)
{
  uint32_t saved_baspri = __get_BASEPRI();
  __set_BASEPRI(BSP_CRITICAL_BASEPRI);
  return saved_baspri;
}

static inline void BSP_CriticalExit(uint32_t saved_baspri)
{
  __set_BASEPRI(saved_baspri);
}

typedef struct
{
  osThreadId_t thread_id;
  uint32_t signal;
} CallbackTaskHandle_t;

/**
 * @brief 创建一个事件回调任务,任务收到信号后才执行回调
 *
 * @param name 任务名称,注意以'\0'结尾
 * @param callback 回调函数指针
 * @param argument 回调函数的参数,通常为BSP实例指针
 * @param priority 任务优先级
 * @param stack_size 任务栈大小,单位字节,0表示使用默认值
 * @return CallbackTaskHandle_t 任务句柄和对应的线程标志
 */
CallbackTaskHandle_t CreateCallbackTask(const char *name, void (*callback)(void *), void *argument,
                                        osPriority_t priority, uint32_t stack_size);

/**
 * @brief 从任务或中断中触发回调任务
 *
 * @param handle CreateCallbackTask()返回的句柄
 */
static inline void CallbackTaskNotify(const CallbackTaskHandle_t *handle)
{
  if ((handle != NULL) && (handle->thread_id != NULL) && (handle->signal != 0U))
    (void)osThreadFlagsSet(handle->thread_id, handle->signal);
}

#endif // BSP_TOOLS_H
