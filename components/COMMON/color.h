#ifndef __COLOR_H
#define __COLOR_H

/* 公共颜色词汇(RGB565): 驱动(LCD)/框架(UI)/业务组件共用.
 * 独立成头的目的: 业务头文件不再被迫 #include "LCD.h"(驱动头),
 * 消除"为拿几个颜色宏而反向依赖驱动层"的分层违规. */

#define WHITE           0xFFFF      /* 白色 */
#define BLACK           0x0000      /* 黑色 */
#define RED             0xF800      /* 红色 */
#define GREEN           0x07E0      /* 绿色 */
#define BLUE            0x001F      /* 蓝色 */
#define MAGENTA         0XF81F      /* 品红色/紫红色 = BLUE + RED */
#define YELLOW          0XFFE0      /* 黄色 = GREEN + RED */
#define CYAN            0X07FF      /* 青色 = GREEN + BLUE */
#define BROWN           0XBC40      /* 棕色 */
#define BRRED           0XFC07      /* 棕红色 */
#define GRAY            0X8430      /* 灰色 */
#define DARKBLUE        0X01CF      /* 深蓝色 */
#define LIGHTBLUE       0X7D7C      /* 浅蓝色 */
#define GRAYBLUE        0X5458      /* 灰蓝色 */
#define LIGHTGREEN      0X841F      /* 浅绿色 */
#define LGRAY           0XC618      /* 浅灰色(PANNEL),窗体背景色 */
#define LGRAYBLUE       0XA651      /* 浅灰蓝色(中间层颜色) */
#define LBBLUE          0X2B12      /* 浅棕蓝色(选择条目的反色) */


/* ================= 主题/界面调色板(默认护眼柔和绿, RGB565) ================= */
#define THEME_BG        0x10C2   /* 背景 #141A14 深暖绿黑 */
#define THEME_MENU      0xCF18   /* 菜单文字 #C8E0C0 淡绿白 */
#define THEME_FRAME     0x8EB1   /* 选中框 #8FD48A 柔和绿 */
#define THEME_ICON      0x7E9A   /* 图标/扫描线 #7FD0D0 柔和青 */
#define THEME_TIME      0xCF18   /* 时钟 #C8E0C0 淡绿白 */
#define THEME_DATE      0xCF18   /* 日期 #C8E0C0 淡绿白 */
#define UI_WIFI_OFF     0x3186   /* WiFi 未连接图标灰 */

/* ================= 业务/特殊色(集中管理) ================= */
#define GACHA_RED       0xE249   /* ★2红人格 #E04848 */
#define GACHA_GOLD      0xEE08   /* ★3金/EGO #E8C040 */
#define GACHA_VOICE     0xCF18   /* 抽卡语音淡绿白 */

/* ================= 指令库颜色(与全局主题解耦) ================= */
#define INS_DEFAULT_RED 0xE249   /* 普通破译真字默认清晰红 */
#define INS_GARBLE_BLUE 0x651D   /* 未破译乱码亮钢蓝 */

#endif
