#ifndef __EVT_H
#define __EVT_H

/* 按键/摇动事件码(全工程唯一来源).
 * 旧状况: main.c 与 MPU6050.c 各自复制一份宏, ANSWER.c/SETTING.c 用裸数字字面量,
 * 改一处即静默错位(摇动"确认"变成别的键). 统一从此处取用. */
#define EVT_NONE     0
#define EVT_UP       1    /* 上(内容上移) */
#define EVT_OK       2    /* 确认(短按) */
#define EVT_DOWN     3    /* 下(内容下移) */
#define EVT_LONG_OK  4    /* OK 长按(返回上一级) */

#endif
