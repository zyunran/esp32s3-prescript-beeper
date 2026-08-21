#ifndef __INSTRUCTION_H
#define __INSTRUCTION_H

#include <stdint.h>
#include "LCD.h"        /* 颜色宏(RED/BLUE...) */

/* 运行期指令库/使用者上限(与 WEB 输入校验共用) */
#define INS_PRESET_MAX      40
#define INS_PRESET_LEN      110
#define INS_USER_NAME_MAX   24  /* 当前使用者名最大字节数(与使用者列表上限 UI_USER_NAME_MAX 对齐) */

/* ================= 破译/蜂鸣可调参数(改这里即可调整) ================= */
/* 破译显示 */
/* 破译真字色/乱码色/速度(运行时可改, WEB配置, 存NVS "ins2") */
extern volatile uint16_t INS_SCR_DEFAULT;      /* 破译真字色 */
extern volatile uint16_t INS_SCR_GARBLE;       /* 未破译乱码色 */
extern volatile uint16_t INS_SCR_DELAY_MS;     /* 乱码刷新间隔 ms */
extern volatile uint16_t INS_REVEAL_DELAY_MS;  /* 逐字揭示间隔 ms(越大解码越慢) */
#define INS_SCR_FRAMES      30        /* 全乱码帧数(越大乱码越久) */
#define INS_REVERT_PCT      10         /* 已解码字回退乱码概率% */
#define INS_SLIDE_START     10         /* 新解码字起始右移 px */
#define INS_SLIDE_STEP      2          /* 解码字每帧左移 px */
#define INS_WOBBLE          6          /* 全乱码阶段整块左右抖动 px */
/* 蜂鸣器(GPIO15, 有源, 低电平有效: 低=响) */
#define INS_BEEP_MS         220        /* 每次蜂鸣时长 ms */
#define INS_BEEP_MIN        150        /* 相邻蜂鸣最短间隔 ms */
#define INS_BEEP_MAX        500        /* 相邻蜂鸣最长间隔 ms */

/* ================= API ================= */
void INS_Init(void);                    /* 初始化蜂鸣器 GPIO */
void INS_SetBeep(uint8_t on);           /* 蜂鸣总开关(1=响, 0=静音), 默认开 */
void INS_BeepTick(void);                /* 蜂鸣推进, 由 UI 主任务每循环调用 */
void INS_BeepTimes(uint8_t n);          /* 独立蜂鸣 n 下(倒计时结束等) */
void INS_Show(const char *text);        /* 显示一条自定义指令(乱码→逐字破译) */
void INS_ShowIns(const char *text);     /* 指令显示: 已有"致X:"原样; 无则自动加"致{当前使用者}:" */
void INS_ShowByIndex(uint8_t idx);      /* 显示预设指令(对应菜单项索引) */
void INS_ShowRandom(void);              /* 随机抽取一条预设指令并破译显示 */
void INS_ShowGenerated(void);           /* 用模板现场生成随机指令(含当前使用者) */
/* 当前使用者名称(神谕指令"致X:"对象; 默认李箱, 存 NVS "ins2"/"user") */
const char *INS_UserName(void);
void INS_SetUserName(const char *name); /* 设使用者名称并保存 */
void INS_GetParams(uint16_t *def, uint16_t *gb, uint16_t *dl, uint16_t *rv);  /* 破译参数读 */
void INS_SetParams(uint16_t def, uint16_t gb, uint16_t dl, uint16_t rv);      /* 破译参数写(WEB) */
uint8_t INS_Font(void);                  /* 破译字号: 0=16 1=24 2=32px */
void INS_SetFont(uint8_t f);             /* 设破译字号并保存(NVS "ins2"/"fnt"; 行数自动匹配) */
/* 指令库(WEB 配置): 取当前列表 / 用 '\n' 分隔文本重建并持久化 */
const char *const *INS_Presets(uint8_t *count);
void INS_PresetsFromText(const char *text);
void INS_Tick(void);                    /* 推进动画 + 蜂鸣, 每主循环调用一次 */
uint8_t INS_Finished(void);             /* 1=本次破译已完成(全文已显示) */
void INS_Exit(void);                    /* 提前退出破译, 回到 UI 主界面 */

#endif
