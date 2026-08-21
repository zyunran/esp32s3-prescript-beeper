#ifndef __GACHA_H
#define __GACHA_H

#include <stdint.h>
#include "LCD.h"        /* 颜色宏(GRAY/RED/YELLOW...) */

/* ================= 卡池条目 ================= */
typedef struct {
    const char *sinner;   /* 罪人前缀(如"浮士德") */
    const char *name;     /* 条目名(如"执柄者") */
    uint8_t rarity;       /* GACHA_RAR_* */
} gacha_card_t;

/* 抽取语音(金人格 ★3, 已并入 coin_skills[].voice, 见 GACHA.c) */

/* 稀有度: 人格按官方灯级 ★1灰/★2红/★3金; EGO 单独归类(统一金) */
#define GACHA_RAR_GRAY   0
#define GACHA_RAR_RED    1
#define GACHA_RAR_GOLD   2
#define GACHA_RAR_EGO    3
#define GACHA_RAR_COUNT  4

/* ================= 概率(千分比, 总和1000, 改这里即可调整) ================= */
#define GACHA_PCT_GRAY   830     /* 灰人格 83.0% */
#define GACHA_PCT_RED    128     /* 红人格 12.8% */
#define GACHA_PCT_GOLD   29      /* 金人格 2.9% */
#define GACHA_PCT_EGO    13      /* EGO 1.3% */

/* ================= 稀有度颜色(固定三色) ================= */
#define GACHA_COLOR_GRAY  GRAY
#define GACHA_COLOR_RED   0xE249   /* ★2红人格: 清晰红(#E04848), 与★1灰区分明显又不刺眼 */
#define GACHA_COLOR_GOLD  0xEE08   /* ★3金/EGO: 亮金(#E8C040), 醒目又不刺眼 */

/* ================= 十连动画可调参数 ================= */
#define GACHA_BOX_SIZE    16      /* 方框边长 px */
#define GACHA_BOX_PITCH   24      /* 相邻方框中心间距 px */
#define GACHA_FRAME_MS    16      /* 动画帧间隔 ms */
#define GACHA_HOLD_MS     900     /* 动画停稳后停留 ms 再出结果 */
#define GACHA_LINE_STEP   3       /* 竖线扫描每帧移动 px(越小越丝滑, 固定步长不跳变) */

/* ================= 十连结果列表可调参数 ================= */
#define GACHA_ROW_H       24      /* 结果列表行距 px(一次移动一位) */
#define GACHA_RESULT_ROWS 3       /* 每屏可见结果行数 */
#define GACHA_ROLL_STEP   3       /* 滚动每帧移动 px(越小越丝滑) */
#define GACHA_ROLL_MS     15      /* 滚动帧间隔 ms */

/* ================= 抽取语音可调参数 ================= */
#define GACHA_TYPE_STEP   1       /* 语音打字机每帧字符数 */
#define GACHA_TYPE_MS     25      /* 打字机帧间隔 ms(越小越快) */
#define GACHA_VOICE_COLOR 0xCF18  /* 语音文字色(淡绿白, 与主题一致) */

/* ================= 抽卡方框/扫描线颜色(主题色, 定义于 lcd.h) ================= */
#define GACHA_COLOR_BOX   UI_COLOR_FRAME   /* 空方框边框(柔和绿) */
#define GACHA_COLOR_LINE  UI_COLOR_ICON    /* 扫描线(柔和青) */

/* ================= API(由 RTOS 主任务驱动, 不阻塞界面; httpd 侧图鉴接口首次调用含 NVS 加载) =================
 * 事件码与 main.c 的 EVT_UP/EVT_OK/EVT_DOWN/EVT_LONG_OK 一致: 1=上 2=确认 3=下 4=OK长按
 * 流程: Enter 渲染"十连/拼点/单抽/积分/图鉴/退出"子菜单 -> OnEvent 选十连进动画
 *       -> Tick 推进扫描/语音/结果滚动 -> 结果确认回子菜单, 子菜单"退出"回主界面 */
void    GACHA_Enter(void);       /* 进入抽卡界面(渲染子菜单) */
void    GACHA_ForceExit(void);   /* 强制退出(OK 长按返回主界面) */
void    GACHA_OnEvent(uint8_t evt); /* 按键事件(1=UP 2=OK 3=DOWN) */
void    GACHA_Tick(void);        /* 推进抽卡动画/语音/结果滚动, 每主循环调用 */
uint8_t GACHA_Busy(void);        /* 1=抽卡界面运行中 */
void    GACHA_Init(void);        /* 创建抽卡/图鉴跨任务递归互斥量, 须在 WEB_Init 之前调用 */

/* ================= 图鉴/拼点人格表访问(网页图鉴用; 调用会加载 NVS 已抽标记) ================= */
uint16_t GACHA_CoinTotal(void);               /* 人格总数(120) */
uint16_t GACHA_CoinOwnedCount(void);          /* 已抽中人格数 */
uint8_t  GACHA_CoinOwned(uint16_t idx);       /* 1=该人格已抽中 */
const char *GACHA_CoinName(uint16_t idx);     /* 该人格名 */
uint8_t  GACHA_CoinSinnerN(void);             /* 罪人数(12) */
const char *GACHA_CoinSinnerName(uint8_t i);  /* 第 i 个罪人名 */
uint16_t GACHA_CoinSinnerOff(uint8_t i);      /* 第 i 罪人在人格表起始下标 */
uint16_t GACHA_CoinSinnerCount(uint8_t i);    /* 第 i 罪人人格数 */

/* 卡池数据(定义于 gacha_data.c, 由 gen_gacha.py 生成) */
extern const gacha_card_t gacha_cards[];
extern const uint16_t gacha_pool_start[GACHA_RAR_COUNT]; /* 各稀有度在 gacha_cards 中的起始下标 */
extern const uint16_t gacha_pool_count[GACHA_RAR_COUNT]; /* 各稀有度条目数 */

#endif
