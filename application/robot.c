#include "bsp_init.h"
#include "robot.h"
#include "robot_def.h"
#include "robot_task.h"

// 编译warning,提醒开发者修改机器人参数
#ifndef ROBOT_DEF_PARAM_WARNING
#define ROBOT_DEF_PARAM_WARNING
#pragma message "check if you have configured the parameters in robot_def.h, IF NOT, please refer to the comments AND DO IT, otherwise the robot will have FATAL ERRORS!!!"
#endif // !ROBOT_DEF_PARAM_WARNING

#if defined(ONE_BOARD) || defined(CHASSIS_BOARD)
#include "chassis.h"
#endif

#if defined(ONE_BOARD) || defined(GIMBAL_BOARD)
#include "gimbal.h"
#include "shoot.h"
#include "robot_cmd.h"
#endif


void RobotInit()
{  
    // 关闭中断,防止在初始化过程中发生中断
    // 请不要在初始化过程中使用中断和延时函数！
    // 若必须,则只允许使用DWT_Delay()
    __disable_irq();
    
    BSPInit();

#if defined(ONE_BOARD) || defined(GIMBAL_BOARD)
    RobotCMDInit();
    // GimbalInit();
    // ShootInit();
#endif

#if defined(ONE_BOARD) || defined(CHASSIS_BOARD)
    ChassisInit();
#endif

    OSTaskInit(); // 创建基础任务

    // 中断持续关闭,由FreeRTOS启动第一个任务时统一开启,避免调度器启动前触发中断通知
}

void RobotTask()
{
#if defined(ONE_BOARD) || defined(GIMBAL_BOARD)
    RobotCMDTask();
    // GimbalTask();
    // ShootTask();
#endif

#if defined(ONE_BOARD) || defined(CHASSIS_BOARD)
    ChassisTask();
#endif

}