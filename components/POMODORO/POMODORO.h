#ifndef __POMODORO_H
#define __POMODORO_H

#include <stdint.h>

/* POMODORO 组件: 番茄钟(TTL协议 新增子项)
 * 流程: 设定屏(工作分钟 5..60 步进5, 上下调) -> OK 开始 -> 工作25:00 倒计时
 *   -> 到点蜂鸣(main 处理) + 自动进入休息 5:00 -> 到点蜂鸣 + 自动开始下一轮工作(轮数+1)
 *   工作中按 OK = 暂停/继续; 长按OK = 退出(TTL 子菜单)
 * 计时基于 esp_timer, 熄屏照常走(render=0 不重绘), 与倒计时组件同模式.
 * 注意: 长按OK退出即放弃本轮(轮数不持久化). */

#define POM_WORK_DEFAULT   25    /* 默认工作分钟 */
#define POM_WORK_MIN        5    /* 可设下限 */
#define POM_WORK_MAX       60    /* 可设上限 */
#define POM_WORK_STEP       5    /* 设定步进 */
#define POM_BREAK_MIN       5    /* 休息分钟(固定) */

typedef enum {
    POM_RUN,     /* 继续 */
    POM_DONE,    /* 一个阶段(工作或休息)到点: main 蜂鸣+亮屏 */
    POM_EXIT,    /* 退出回 TTL 子菜单 */
} pom_ret_t;

void POM_Init(void);                                   /* 复位内部状态 */
void POM_Enter(void);                                   /* 进入设定屏 */
void POM_Exit(void);                                    /* 长按OK退出(下次 Tick 返回 POM_EXIT) */
pom_ret_t POM_Key(uint8_t up, uint8_t ok, uint8_t down);/* 按键(1=按下) */
pom_ret_t POM_Tick(uint8_t render);                     /* 每主循环推进; render=0 只推进不重绘 */

#endif
