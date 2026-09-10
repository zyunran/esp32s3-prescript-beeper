#ifndef __TIMER_H
#define __TIMER_H

#include <stdint.h>

/* TIMER: 倒计时 API。 */

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
