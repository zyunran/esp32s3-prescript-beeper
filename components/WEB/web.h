#ifndef __WEB_H
#define __WEB_H

#include <stdint.h>
#include <stddef.h>

/* WEB 组件: 内嵌配置页 + REST API(改指令库/闹钟/UI主题色/WiFi等, NVS 持久化)
 * 联网后手机/PC 浏览器访问 http://<esp32-ip>/ 打开配置页 */

void WEB_Init(void);   /* 加载颜色配置 + 启动 HTTP 服务器 */
uint8_t WEB_TakeCmd(char *buf, size_t n);   /* 取回网页下发的指令(取走即清), 1=有 */
uint8_t WEB_ConfigDirty(void);      /* 1=网页刚保存了配置(主界面需重绘应用) */
void    WEB_ConfigDirtyClear(void); /* 清除"配置已改"标志 */

#endif
