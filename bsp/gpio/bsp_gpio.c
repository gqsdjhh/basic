#include "bsp_gpio.h"
#include "bsp_log.h"
#include "memory.h"
#include "stdlib.h"

static uint8_t idx;
static GPIOInstance *gpio_instance[GPIO_MX_DEVICE_NUM] = {NULL};

static void GPIODeferredCallback(void *arg)
{
  GPIOInstance *instance = (GPIOInstance *)arg;

  if (instance->gpio_model_callback != NULL)
    instance->gpio_model_callback(instance);
}

/**
 * @brief EXTI中断回调函数,根据GPIO_Pin找到对应的GPIOInstance,并调用模块回调函数(如果有)
 * @note 如何判断具体是哪一个GPIO的引脚连接到这个EXTI中断线上?
 *       一个EXTI中断线只能连接一个GPIO引脚,因此可以通过GPIO_Pin来判断,PinX对应EXTIX
 *       一个Pin号只会对应一个EXTI,详情见gpio.md
 * @param GPIO_Pin 发生中断的GPIO_Pin
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  // 如有必要,可以根据pinstate和HAL_GPIO_ReadPin来判断是上升沿还是下降沿/rise&fall等
  GPIOInstance *gpio;
  for (size_t i = 0; i < idx; i++)
  {
    gpio = gpio_instance[i];
    if (gpio->GPIO_Pin == GPIO_Pin && gpio->gpio_model_callback != NULL)
    {
      if (gpio->callback_task.thread_id != NULL)
        CallbackTaskNotify(&gpio->callback_task);
      else
        gpio->gpio_model_callback(gpio);
      return;
    }
  }
}

GPIOInstance *GPIORegister(GPIO_Init_Config_s *GPIO_config)
{
  if (GPIO_config == NULL || idx >= GPIO_MX_DEVICE_NUM)
  {
    LOGERROR("[bsp_gpio] invalid config or instance count exceeds limit");
    return NULL;
  }

  GPIOInstance *ins = (GPIOInstance *)malloc(sizeof(GPIOInstance));
  if (ins == NULL)
  {
    LOGERROR("[bsp_gpio] failed to allocate GPIO instance");
    return NULL;
  }
  memset(ins, 0, sizeof(GPIOInstance));

  ins->GPIOx = GPIO_config->GPIOx;
  ins->GPIO_Pin = GPIO_config->GPIO_Pin;
  ins->pin_state = GPIO_config->pin_state;
  ins->exti_mode = GPIO_config->exti_mode;
  ins->id = GPIO_config->id;
  ins->gpio_model_callback = GPIO_config->gpio_model_callback;

  if (ins->gpio_model_callback != NULL)
  {
    ins->callback_task = CreateCallbackTask("gpio_exti", GPIODeferredCallback, ins,
                                            osPriorityAboveNormal, CALLBACK_TASK_DEFAULT_STACK_SIZE);
    if (ins->callback_task.thread_id == NULL)
      LOGERROR("[bsp_gpio] failed to create exti callback task");
  }

  gpio_instance[idx++] = ins;
  return ins;
}

// ----------------- GPIO API -----------------
// 都是对HAL的形式上的封装,后续考虑增加GPIO state变量,可以直接读取state

void GPIOToggel(GPIOInstance *_instance)
{
  HAL_GPIO_TogglePin(_instance->GPIOx, _instance->GPIO_Pin);
}

void GPIOSet(GPIOInstance *_instance)
{
  HAL_GPIO_WritePin(_instance->GPIOx, _instance->GPIO_Pin, GPIO_PIN_SET);
}

void GPIOReset(GPIOInstance *_instance)
{
  HAL_GPIO_WritePin(_instance->GPIOx, _instance->GPIO_Pin, GPIO_PIN_RESET);
}

GPIO_PinState GPIORead(GPIOInstance *_instance)
{
  return HAL_GPIO_ReadPin(_instance->GPIOx, _instance->GPIO_Pin);
}
