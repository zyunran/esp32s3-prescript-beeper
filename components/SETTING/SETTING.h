#ifndef __SETTING_H
#define __SETTING_H

#include <stdint.h>

/* 设置子菜单项索引(顺序=显示顺序; v1.16 按组重排: 声音->显示->推送->交互->工具;
 * 平衡自 v1.16 移入 织机 菜单) */
typedef enum {
    SET_IDX_VOL = 0,       /* 音量 */
    SET_IDX_BEEP,          /* 蜂鸣器开/关 */
    SET_IDX_KEY,           /* 按键音: 关/蜂鸣/音频/双 */
    SET_IDX_SCREEN,        /* 息屏时长 */
    SET_IDX_AOD,           /* 息屏时钟 开/关 */
    SET_IDX_THEME,         /* 主题预设: 柔和绿/赛博青/深夜黑/标准黑白 */
    SET_IDX_CURSOR,        /* 光标样式 */
    SET_IDX_INS_FONT,      /* 破译字号 */
    SET_IDX_SHAKE,         /* 摇动翻页开/关 */
    SET_IDX_INFO,          /* 系统信息 */
    SET_IDX_RESET,         /* 初始化 */
    SET_IDX_EXIT,          /* 退出 */
    SET_IDX_COUNT,         /* 含"退出"的项数 */
} setting_idx_t;

/* SETTING 组件: 设置(NVS 持久化) + 设置子菜单交互
 *  - 值: 熄屏时长/息屏时钟/音量/蜂鸣开关/摇动开关/光标样式
 *  - 副作用在修改时立即应用(蜂鸣开关/音量/摇动开关/陀螺仪互换)
 *  - 设置子菜单由 UI 主任务驱动: Enter 生成项 -> Select 处理选中 */

void SET_Init(void);                       /* 加载 NVS 设置并应用副作用 */
uint16_t SET_TimeoutSec(void);             /* 熄屏秒数(0=永不) */
uint8_t  SET_Vol(void);                    /* 音量 0~100 */
uint8_t  SET_Beep(void);                   /* 蜂鸣器开关(有源, 0=关) */
uint8_t  SET_KeySound(void);               /* 按键音: 0=关 1=音频(扬声器) */
void     SET_SetKeySound(uint8_t v);       /* 设按键音并保存 */
uint8_t  SET_Theme(void);                  /* 主题预设: 0=柔和绿 1=赛博青 2=深夜黑 3=标准黑白 */
void     SET_SetTheme(uint8_t idx);        /* 设主题预设并保存 */
uint8_t  SET_AodClock(void);               /* 息屏时钟: 0=关 1=开 */
void     SET_SetAodClock(uint8_t on);      /* 设息屏时钟并保存 */
uint8_t  SET_OracleN(void);                /* 神谕每日条数(0=关) */
uint8_t  SET_OracleWin(void);              /* 神谕时段预设索引 */
const uint16_t *SET_OracleWinRange(void);  /* 神谕时段 [start,end] 当日分钟 */
uint32_t SET_BootCount(void);              /* 开机次数(NVS) */
void SET_SetVol(uint8_t v);                /* 设音量并保存(WEB配置用) */
void SET_SetBeep(uint8_t on);              /* 设蜂鸣开关并保存(WEB配置用) */
void SET_SetShake(uint8_t on);             /* 设摇动开关并保存 */
uint8_t  SET_ShakeSwap(void);                  /* 平衡互换: 0=默认 1=上下/左右调换 */
void     SET_SetShakeSwap(uint8_t on);         /* 设平衡互换并保存 */
void SET_SetTimeout(uint16_t sec);         /* 设熄屏秒数并保存(0=永不) */
void SET_SetOracleN(uint8_t n);            /* 设神谕条数并保存 */
void SET_SetOracleWin(uint8_t idx);        /* 设神谕时段索引并保存 */
void SET_SetCursor(uint8_t style);         /* 设光标样式并保存(UI_CURSOR_*) */
void SET_ShowInfo(void);                   /* 全屏系统信息页(4 页: 系统/签收/电量/战绩, 上下键翻页) */
void SET_InfoNav(uint8_t evt);             /* 系统信息页翻页(1=UP 3=DOWN, 与 main.c EVT_* 一致) */
void SET_SaveBatchBegin(void);             /* 网页批量保存开始: SET_Set* 只改 RAM 不落盘 */
void SET_SaveBatchEnd(void);               /* 批量结束: 全部设置统一落盘一次(配对调用, 可嵌套) */

void SET_SubmenuEnter(void);               /* 生成设置项并初始化子菜单 */
uint8_t SET_SubmenuCount(void);            /* 设置项数(含"退出") */
void SET_SubmenuSelect(uint8_t sel);       /* 处理选中(循环/切换并更新文字) */

#endif
