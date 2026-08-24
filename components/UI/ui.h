#ifndef __UI_H
#define __UI_H

#include <stdint.h>
#include "LCD.h"        /* 颜色宏(WHITE/YELLOW/LBBLUE...) */

/* 按键 GPIO 布局(改这里即可调整按键映射):
 *   UI_KEY_UP   = 内容上移
 *   UI_KEY_DOWN = 内容下移
 *   UI_KEY_OK   = 确认当前功能
 * PCB 版实测接线: 上=GPIO5 / 下=GPIO6 / 确认=GPIO4(旧板为 上4/下6/确认5);
 * 若换板只改这三个值, 轮询/待机/长按连发逻辑全部跟随本定义 */
#define UI_KEY_UP      5
#define UI_KEY_DOWN    6
#define UI_KEY_OK      4

/* ================= 界面可调参数(改这里即可调整界面) ================= */
/* 界面主题色(UI_COLOR_BG/MENU/...)已定义于 lcd.h, 全工程共用 */
/* 左侧指令图标 */
#define UI_ICON_X       110                 /* 图标 x: 全屏水平居中 (284-64)/2 */
#define UI_ICON_Y       6                   /* 图标 y: 垂直居中 (76-64)/2 */
/* 布局 */
/* 右侧滚动菜单 */
#define UI_MENU_X       195                 /* 菜单项 x(居中靠右) */
#define UI_MENU_ROW_H   26                  /* 行距(字高16 + 间距10) */
#define UI_MENU_Y0      4                   /* 第0行 top */
#define UI_MENU_Y1      30                  /* 第1行 top = 选中行 */
#define UI_MENU_Y2      56                  /* 第2行 top */
/* 选中项效果: 左移 + 白色矩形线框(带间距, 不贴文字) */
#define UI_MENU_SHIFT   16                  /* 选中项左移 px(一个汉字宽) */
#define UI_MENU_FRAME_GAP 3                 /* 白线框与文字间距 px */
#define UI_EASE_STEP    2                   /* 左移/回退缓动每帧 px */
#define UI_EASE_DELAY   15                  /* 缓动帧间隔 ms */
/* 清屏左边界(覆盖项与白线框可达范围) */
#define UI_MENU_CLEAR_X (UI_MENU_X - UI_MENU_SHIFT - UI_MENU_FRAME_GAP - 2)
/* 动画 */
#define UI_ANIM_STEP    2                   /* 滚动动画每帧移动像素 */

/* ================= 光标样式(可切换, 颜色统一走 UI_CURSOR_COLOR) =================
 *  - 白线: 当前 1px 矩形线框
 *  - 白块: 选中项反白填充 + 文字用背景色
 *  - 角框: 只画四角 L 形括号 */
typedef enum {
    UI_CURSOR_LINE = 0,
    UI_CURSOR_BLOCK,
    UI_CURSOR_CORNER,
    UI_CURSOR_N,
} ui_cursor_style_t;

#define UI_CURSOR_DEFAULT   UI_CURSOR_LINE
#define UI_CURSOR_COLOR     UI_COLOR_FRAME

/* ================= 主菜单配置(改这里即可调整菜单项标题与对应功能) =================
 * 顺序即右侧菜单显示顺序. 标题改显示文字, fn 改选中后功能, 一处配置全工程生效
 * 菜单项索引与子菜单项索引均枚举化, 配置与处理共用同一枚举, 增删项不会静默错位 */
typedef enum {
    UI_MENU_INS = 0,      /* 神谕 */
    UI_MENU_ASK,          /* 询问(答案之书) */
    UI_MENU_GACHA,        /* 观测 */
    UI_MENU_TODO,         /* 待办 */
    UI_MENU_USER,         /* 使用者(运行期可变) */
    UI_MENU_SETTING,      /* 设置 */
    UI_MENU_NET,          /* 联网 */
    UI_MENU_TTL,          /* TTL协议 */
    UI_MENU_COUNT,
} ui_menu_idx_t;

typedef enum {
    UI_NET_CONNECT = 0,   /* 联网开关(开启/关闭联网会话) */
    UI_NET_AP,            /* 开启配网 */
    UI_NET_WEATHER,       /* 查看天气 */
    UI_NET_IP,            /* 显示IP */
    UI_NET_EXIT,          /* 退出 */
} ui_net_idx_t;

typedef enum {
    UI_TTL_FUTURE = 0,    /* 跨越时间(倒计时) */
    UI_TTL_PAST,          /* 锚定时间(闹钟) */
    UI_TTL_POMO,          /* 番茄钟(v1.03 新增) */
    UI_TTL_EXIT,          /* 退出 */
} ui_ttl_idx_t;

typedef enum {
    UI_FN_INS,        /* 随机指令破译(神谕) */
    UI_FN_ASK,        /* 询问(答案之书: 回答/吃什么/喝什么/玩什么) */
    UI_FN_GACHA,      /* 抽卡十连(直接进入"十连/退出"菜单) */
    UI_FN_SUBMENU,    /* 通用子菜单: {title}01..NN + 退出 */
    UI_FN_NET,        /* 联网(联网开关/开启配网/查看天气/显示IP; 子菜单项在 ui.c cfg_net_items 配置) */
    UI_FN_SETTING,    /* 设置(子菜单项由 SETTING.c settings_items_refresh 生成) */
    UI_FN_TTL,        /* TTL协议: 跨越时间(倒计时)/锚定时间(闹钟)/退出 */
    UI_FN_TODO,       /* 待办/指令日志: 待执行指令列表, 完成即 PASS */
    UI_FN_USER,       /* 使用者: 当前使用者名称选择 */
} ui_fn_t;

typedef struct {
    const char *title;             /* 主菜单标题 */
    ui_fn_t fn;                    /* 选中后功能 */
    uint8_t sub_count;             /* fn=UI_FN_SUBMENU 时子项数 */
    const char *const *items;      /* 自定义子菜单项(含"退出"); NET/TTL/LOOM/USER 用(TIMER 直入倒计时不用) */
    uint8_t item_count;            /* items 数量 */
} ui_menu_cfg_t;

extern ui_menu_cfg_t ui_menu_cfg[UI_MENU_COUNT];

#define UI_USER_NAME_MAX 24   /* 使用者名称最大字节数(NVS/子菜单共用) */

/* 主菜单「使用者」项标题=当前使用者名(不重绘, 下次渲染生效) */
void UI_SetUserTitle(const char *name);
const char *const *UI_UserList(uint8_t *count); /* 使用者列表(不含"退出", 网页端下拉选择用) */
void UI_SetCursorStyle(uint8_t style);          /* 光标样式: UI_CURSOR_LINE/BLOCK/CORNER */
uint8_t UI_GetCursorStyle(void);                /* 当前光标样式 */
void UI_DrawCursor(const char *s, int16_t x, int16_t y); /* 按当前光标样式画选中框(写帧缓冲, 与 UI_Scr* 同缓冲; 供自定义界面复用) */

/* ================= API ================= */
void UI_Init(void);                          /* 初始化 LCD + 画初始界面(左图标+右菜单) */
void UI_UserInit(void);                      /* 加载使用者列表(NVS, 无则内置默认; UI_Init 前调用) */
uint8_t UI_UserAdd(const char *name);        /* 添加使用者(去重, NVS 持久化), 1=成功/已存在 */
void UI_Scroll(int8_t dir);                  /* 菜单滑动: dir=+1 内容上移, dir=-1 内容下移 */
uint8_t UI_GetSelect(void);                  /* 返回当前选中项索引 */

/* 通用子菜单(复用主界面样式): 非阻塞状态机, 供 RTOS 主任务驱动.
 *   Init: 生成 {title}01 ~ {title}NN + "退出" 并渲染(count 可增减)
 *   InitItems: 用调用方给定的完整项列表(须含"退出")渲染(nitems 可增减)
 *   Scroll: 上下滚动
 *   Cur: 当前选中索引
 * 确认"退出"后由调用方 UI_RenderScreen() 回主界面. */
void    UI_SubMenuInit(const char *title, uint8_t count);
void    UI_SubMenuInitItems(const char *const items[], uint8_t nitems);
void    UI_SubMenuInitItemsC(const char *const items[], uint8_t nitems, uint8_t center); /* center=1 水平居中(长文字不裁边); 2=块左对齐居中 */
void    UI_SubMenuSetCenterDx(int16_t dx);   /* 块左对齐(center=2)整体右移量 px */
void    UI_SubMenuScroll(int8_t dir);
uint8_t UI_SubMenuCur(void);
void    UI_SubMenuSetItem(uint8_t idx, const char *text); /* 更新子菜单单项文字(保持选中) */
void    UI_SubMenuSetCur(uint8_t idx);   /* 设置子菜单光标并重绘(子流程返回后保持选中) */

/* 帧缓冲绘制接口(供 INSTRUCTION/GACHA 等组件做全屏显示用) */
void UI_ScrClear(uint16_t color);            /* 清屏(写进帧缓冲) */
void UI_ScrRect(uint16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t color); /* 实心矩形 */
void UI_ScrImage(uint16_t x, int16_t y, uint16_t w, uint16_t h, const uint8_t *img, uint8_t gray); /* 画RGB565图(大端字节序; 纯黑=透明; gray=1转灰度) */
uint16_t UI_ScrGlyph(uint16_t x, int16_t y, const char *ch, uint16_t fc, uint16_t bc); /* 画单个UTF-8字符, 返回宽度 */
uint16_t UI_ScrGlyphF(uint16_t x, int16_t y, const char *ch, uint8_t font_h, uint16_t fc, uint16_t bc); /* 按字号16/24/32缩放画字符, 返回宽度 */
uint16_t UI_ScrText(uint16_t x, int16_t y, const char *s, uint16_t fc, uint16_t bc);   /* 画UTF-8字符串, 返回宽度 */
uint16_t UI_ScrTextCenter(int16_t y, const char *s, uint16_t fc, uint16_t bc);        /* 水平居中画字符串, 返回宽度 */
void UI_ScrBlit(void);                       /* 整屏刷到 LCD */

/* ================= 白框滤镜(纺织记忆彩蛋) =================
 * 开启后全系统所有字符/图标不画内容, 改画空心白框;
 * 每个字符串按内容哈希随机稳定保留 1 个真字作提示(同串位置不变), 图标/小字/缩放字整体白框.
 * 任意界面持续生效; 再进织机→纺织记忆 关闭恢复. */
void UI_BoxModeSet(uint8_t on);    /* 1=开白框滤镜, 0=关闭恢复正常 */
uint8_t UI_BoxModeGet(void);       /* 当前白框滤镜状态 */
void UI_RenderScreen(void);                  /* 重绘完整界面(左图标+右菜单), 破译退出后调用 */
void UI_TimeSet(const char *date, const char *time, const char *week); /* 主页面左侧显示日期+星期+时间(内容变化才重绘; week="周X", 无时间传"--") */
void UI_WeatherSet(const char *weather);             /* 主页面时钟下方显示天气(变化才重绘, NULL=清除) */
void UI_WifiSet(uint8_t on);                         /* 主页面左上角 WiFi 状态图标(绿=已连/灰=未连) */
void UI_BatterySet(uint8_t pct);                     /* 主页面左上角电量图标(0-100 填充; 255=空框) */
void UI_FullScreen(const char *line1, const char *line2); /* 全屏两行居中显示(计时/倒计时) */

#endif
