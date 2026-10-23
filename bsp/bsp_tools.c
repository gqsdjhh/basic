#include <stddef.h>

#include "bsp_tools.h"
#include "bsp_log.h"

/* CMSIS-RTOS2线程标志仅bit0~bit30可用 */
#define CALLBACK_TASK_MAX_COUNT 31U

typedef struct
{
    void (*callback)(void *);
    void *argument;
    uint32_t signal;
} CallbackTaskInfo_t;

static uint8_t callback_task_count = 0U;
static CallbackTaskInfo_t callback_task_info_list[CALLBACK_TASK_MAX_COUNT];

static void CallbackTaskEntry(void *argument)
{
    CallbackTaskInfo_t *info = (CallbackTaskInfo_t *)argument;

    for (;;)
    {
        osThreadFlagsWait(info->signal, osFlagsWaitAll, osWaitForever);
        info->callback(info->argument);
    }
}

CallbackTaskHandle_t CreateCallbackTask(const char *name, void (*callback)(void *), void *argument,
                                        osPriority_t priority, uint32_t stack_size)
{
    CallbackTaskHandle_t handle = {NULL, 0U};

    if (callback == NULL)
    {
        LOGERROR("[rtos:callback_task] CreateCallbackTask: callback is NULL");
        return handle;
    }

    if (callback_task_count >= CALLBACK_TASK_MAX_COUNT)
    {
        LOGERROR("[rtos:callback_task] CreateCallbackTask: callback task count exceeds limit");
        return handle;
    }

    if (stack_size == 0U)
        stack_size = CALLBACK_TASK_DEFAULT_STACK_SIZE;

    CallbackTaskInfo_t *info = &callback_task_info_list[callback_task_count];
    info->callback = callback;
    info->argument = argument;
    info->signal = 1UL << callback_task_count;

    const osThreadAttr_t thread_attr = {
        .name = name,
        .stack_size = stack_size,
        .priority = priority,
    };

    osThreadId_t thread_id = osThreadNew(CallbackTaskEntry, info, &thread_attr);
    if (thread_id == NULL)
    {
        LOGERROR("[rtos:callback_task] CreateCallbackTask: failed to create task [%s]", name);
        return handle;
    }

    handle.thread_id = thread_id;
    handle.signal = info->signal;
    callback_task_count++;

    return handle;
}
