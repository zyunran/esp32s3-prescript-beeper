#ifndef __EVT_H
#define __EVT_H

/* EVT: 全工程唯一事件码(main/MPU/业务共用,勿写裸数字)。 */
#define EVT_NONE     0
#define EVT_UP       1    /* 上(内容上移) */
#define EVT_OK       2    /* 确认(短按) */
#define EVT_DOWN     3    /* 下(内容下移) */
#define EVT_LONG_OK  4    /* OK 长按(返回上一级) */

#endif
