#ifndef __TIMER_H
#define __TIMER_H

#include <stdint.h>

/* TIMER 组件: 倒计时(未来到达)
 * 流程: 设定屏(标题+滑动条+角光标, 上下改分钟) -> 确认 -> 倒计时(大字)
 *   -> 归零蜂鸣2下 + 显示"你已到达X分钟后的未来!"(按键退出)
 *   -> 倒计时中按确认: 极速快进到归零并显示到达消息(不蜂鸣)
 * 由 UI 主任务驱动: Enter 进入, Key 按键, Tick 每循环推进. */

#define TIMER_MAX_MIN  60   /* 可选分钟上限(1..60) */

typedef enum {
    TIM_RUN,     /* 继续 */
    TIM_DONE,    /* 倒计时正常归零: 已显示到达消息, 需蜂鸣提示 */
    TIM_EXIT,    /* 退出计时回主界面 */
} tim_ret_t;

void TIM_Enter(void);                              /* 进入倒计时设定屏 */
void TIM_Exit(void);                               /* 强制退出(OK 长按返回), 下次 Tick 返回 TIM_EXIT */
tim_ret_t TIM_Key(uint8_t up, uint8_t ok, uint8_t down); /* 按键(1=按下) */
tim_ret_t TIM_Tick(uint8_t render);                /* 每主循环推进; render=0 只推进不重绘(熄屏省电) */

#endif
