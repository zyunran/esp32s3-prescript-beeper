#ifndef __WEB_H
#define __WEB_H

#include <stdint.h>
#include <stddef.h>

/* WEB: 配置页启动与脏标记/下发指令 API。联网后访问 http://<ip>/ */
void WEB_Init(void);   /* 加载颜色配置 + 启动 HTTP 服务器 */
uint8_t WEB_TakeCmd(char *buf, size_t n);   /* 取回网页下发的指令(取走即清), 1=有 */
uint8_t WEB_ConfigDirty(void);      /* 1=网页刚保存了配置(主界面需重绘应用) */
void    WEB_ConfigDirtyClear(void); /* 清除"配置已改"标志 */

#endif
