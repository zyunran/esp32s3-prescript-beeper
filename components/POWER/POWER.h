#ifndef __POWER_H
#define __POWER_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"   /* TaskHandle_t */

/* POWER 组件: 屏幕亮灭 / 活动时刻 / 浅睡眠待机引擎
 * (自 main.c 拆出. 单写者约定不变: 本组件只操作背光开关 lcd_on/lcd_off, 不绘制内容)
 * 待机原理见 POWER.c 头注(定时器片睡眠, GPIO 唤醒在本板被硬件拒绝).
 * 注: 扣屏静音/抬腕亮屏已删除 —— 省电完全由「息屏超时→待机浅睡眠」承担. */

typedef struct {
    TaskHandle_t input_task;        /* 待机期间挂起/恢复的轮询任务句柄(可 NULL) */
    void (*on_enter)(void);         /* 进待机前宿主收尾(断网/清等待标志), 可 NULL */
    uint8_t (*btn_pressed)(void);   /* 唤醒键查询: 任一按下返回 1 */
    uint8_t (*alarm_due)(void);     /* 睡眠片中查闹钟到点(到点即唤醒并置标记) */
    void (*sensor_suspend)(void);   /* 睡前挂起传感器采样(MPU, 削弱轮询唤醒) */
    void (*sensor_resume)(void);    /* 醒后恢复采样 */
    void (*on_alarm_wake)(void);    /* 闹钟唤醒宿主善后(回主界面/显示/入栈), 亮屏由本组件负责 */
} pwr_host_t;

void     PWR_Init(const pwr_host_t *host);
uint8_t  PWR_ScreenOn(void);         /* 1=屏亮 */
uint32_t PWR_LastAct(void);          /* 上次活动时刻(esp_timer ms 时间轴) */
void     PWR_Wake(uint32_t now_ms);  /* 亮屏+记活动时刻(用户交互/推送唤醒统一入口) */
void     PWR_LcdOff(void);           /* 仅熄屏(超时息屏用) */
void     PWR_Activity(uint32_t now_ms); /* 仅记活动时刻(屏已亮, 如页面自动返回) */
uint8_t  PWR_StandbyAllowed(uint32_t now_ms); /* 息屏前提下是否允许进待机(扣屏窗/唤醒冷却判定) */
void     PWR_StandbyEnter(void);     /* 进入浅睡眠待机(阻塞至按键/闹钟唤醒才返回) */

#endif
