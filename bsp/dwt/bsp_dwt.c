/**
 ******************************************************************************
 * @file	bsp_dwt.c
 * @author  Wang Hongxi
 * @author modified by Neo with annotation
 * @version V1.1.0
 * @date    2022/3/8
 * @brief
 */

#include "bsp_dwt.h"

static DWT_Time_t SysTime;
static uint32_t CPU_FREQ_Hz, CPU_FREQ_Hz_ms, CPU_FREQ_Hz_us;
static uint32_t CYCCNT_RountCount;
static uint32_t CYCCNT_LAST;

/**
 * @brief 读取CYCCNT并更新回绕计数,返回本次读取的计数值
 * @note 通过关闭中断保证CYCCNT_LAST、CYCCNT_RountCount和返回的计数值来自同一次快照
 *
 * @param rounds 输出当前回绕次数,可为NULL
 * @return uint32_t 本次读取的CYCCNT值
 */
static uint32_t DWT_CNT_UpdateWithRounds(uint32_t *rounds)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  uint32_t cnt_now = DWT->CYCCNT;
  if (cnt_now < CYCCNT_LAST)
    CYCCNT_RountCount++;

  CYCCNT_LAST = cnt_now;
  uint32_t rounds_now = CYCCNT_RountCount;

  __set_PRIMASK(primask);

  if (rounds != NULL)
    *rounds = rounds_now;

  return cnt_now;
}

/**
 * @brief 私有函数,用于检查DWT CYCCNT寄存器是否溢出,并更新CYCCNT_RountCount
 *
 * @todo 更好的方案是为dwt的时间更新单独设置一个任务?
 *       不过,使用dwt的初衷是定时不被中断/任务等因素影响,因此该实现仍然有其存在的意义
 */
static void DWT_CNT_Update(void)
{
  (void)DWT_CNT_UpdateWithRounds(NULL);
}

/**
 * @brief 获取当前时间快照,使用64位周期计数避免回绕错误
 */
static DWT_Time_t DWT_GetTimeSnapshot(void)
{
  DWT_Time_t time = {0};

  if (CPU_FREQ_Hz == 0U)
    return time;

  uint32_t rounds = 0;
  uint32_t cnt_now = DWT_CNT_UpdateWithRounds(&rounds);
  uint64_t cycle_count = ((uint64_t)rounds << 32) + (uint64_t)cnt_now;

  uint64_t seconds = cycle_count / CPU_FREQ_Hz;
  uint64_t remainder = cycle_count - seconds * CPU_FREQ_Hz;

  time.s = (uint32_t)seconds;
  time.ms = (uint16_t)(remainder / CPU_FREQ_Hz_ms);
  remainder -= (uint64_t)time.ms * CPU_FREQ_Hz_ms;
  time.us = (uint16_t)(remainder / CPU_FREQ_Hz_us);

  return time;
}

void DWT_Init(uint32_t CPU_Freq_mHz)
{
  /* 使能DWT外设 */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  /* DWT CYCCNT寄存器计数清0 */
  DWT->CYCCNT = (uint32_t)0u;

  /* 使能Cortex-M DWT CYCCNT寄存器 */
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  CPU_FREQ_Hz = CPU_Freq_mHz * 1000000UL;
  CPU_FREQ_Hz_ms = CPU_FREQ_Hz / 1000UL;
  CPU_FREQ_Hz_us = CPU_FREQ_Hz / 1000000UL;
  CYCCNT_RountCount = 0;
  CYCCNT_LAST = 0;

  DWT_CNT_Update();
}

float DWT_GetDeltaT(uint32_t *cnt_last)
{
  if ((cnt_last == NULL) || (CPU_FREQ_Hz == 0U))
    return 0.0f;

  uint32_t cnt_now = DWT->CYCCNT;
  float dt = ((uint32_t)(cnt_now - *cnt_last)) / ((float)(CPU_FREQ_Hz));
  *cnt_last = cnt_now;

  DWT_CNT_Update();

  return dt;
}

double DWT_GetDeltaT64(uint32_t *cnt_last)
{
  if ((cnt_last == NULL) || (CPU_FREQ_Hz == 0U))
    return 0.0;

  uint32_t cnt_now = DWT->CYCCNT;
  double dt = ((uint32_t)(cnt_now - *cnt_last)) / ((double)(CPU_FREQ_Hz));
  *cnt_last = cnt_now;

  DWT_CNT_Update();

  return dt;
}

void DWT_SysTimeUpdate(void)
{
  SysTime = DWT_GetTimeSnapshot();
}

float DWT_GetTimeline_s(void)
{
  DWT_Time_t time = DWT_GetTimeSnapshot();
  return (float)time.s + (float)time.ms * 0.001f + (float)time.us * 0.000001f;
}

float DWT_GetTimeline_ms(void)
{
  DWT_Time_t time = DWT_GetTimeSnapshot();
  return (float)time.s * 1000.0f + (float)time.ms + (float)time.us * 0.001f;
}

uint64_t DWT_GetTimeline_us(void)
{
  DWT_Time_t time = DWT_GetTimeSnapshot();
  return (uint64_t)time.s * 1000000ULL + (uint64_t)time.ms * 1000ULL + (uint64_t)time.us;
}

void DWT_Delay(float Delay)
{
  if ((Delay <= 0.0f) || (CPU_FREQ_Hz == 0U))
    return;

  uint32_t tickstart = DWT->CYCCNT;
  uint32_t wait_cycles = (uint32_t)(Delay * (float)CPU_FREQ_Hz);

  while ((DWT->CYCCNT - tickstart) < wait_cycles)
    ;
}
