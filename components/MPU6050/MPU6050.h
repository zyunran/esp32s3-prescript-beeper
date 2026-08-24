#ifndef __MPU6050_H
#define __MPU6050_H

#include <stdint.h>

/* MPU6050 六轴组件: 软件I2C(GPIO39=SCL推挽 / GPIO38=SDA方向切换, 读时切输入+内部上拉),
 * 参考 STM32 work_watch 工程 Hardware/MPU6050.c(寄存器) + memu.c(互补滤波) 移植.
 *  - MPU_Init: 寄存器初始化 + WHO_AM_I 校验; 失败后台每 2 秒重试(后接传感器也能识别)
 *  - 独立任务每 ~30ms 采样六轴 -> 互补滤波姿态角 -> 摇动方向检测
 *  - 摇动四向 = 按键事件: 上摇=EVT_UP / 下摇=EVT_DOWN / 左摇=EVT_OK(确认) / 右摇=EVT_LONG_OK(退出), 送入按键队列
 *    PCB 实测校准: 上下摇=俯仰(+10→+40/-30); 左右摇=横滚(0→±30); 航向/平移加速度兜底(见 MPU6050.c)
 *  - 设置「平衡」页: MPU_BalanceTick 实时绘制 横滚/俯仰/航向 + 最近摇动方向(由 UI 主任务每循环驱动) */

void MPU_Init(void);                /* GPIO + 软件I2C + MPU6050 寄存器初始化(不含任务) */
void MPU_Start(void *key_q);        /* 启动采样任务(key_q=按键队列, 摇动事件送入) */
void MPU_SetShake(uint8_t on);
uint8_t MPU_EvtWasShake(void);      /* 1=最近200ms内有摇动产生的 确认/退出 事件(平衡页过滤用) */      /* 摇动检测开关(1=开, 0=关; 默认开; 只禁摇动, 采样/平衡照常) */
void MPU_Suspend(void);             /* 待机暂停采样(标志位自查, 不打断 I2C 事务防总线锁死); 唤醒后 MPU_Resume 恢复 */
void MPU_Resume(void);              /* 恢复采样并强制立即重新探测 */
float MPU_Roll(void);               /* 横滚角 ° */
float MPU_Pitch(void);              /* 俯仰角 ° */
float MPU_Yaw(void);                /* 航向角 °(陀螺仪积分, 会缓慢漂移) */
void MPU_BalanceTick(void);         /* 绘制「平衡」实时页(由 UI 主任务驱动) */

#endif
