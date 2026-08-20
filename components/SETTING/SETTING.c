/* SETTING 组件: 设置(NVS 持久化) + 设置子菜单交互
 * 值: 熄屏时长 / 音量 / 蜂鸣开关 / 摇动翻页 / 神谕条数 / 神谕时段 / 光标样式
 * 子菜单项文字含当前值, 选中即修改并重绘 */
#include "SETTING.h"
#include "UI.h"
#include "INSTRUCTION.h"
#include "SOUND.h"
#include "MPU6050.h"
#include "ORACLE.h"
#include "GACHA.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static uint16_t set_timeout_sec = 60;
static uint8_t  set_vol = 100;
static uint8_t  set_beep = 1;               /* 蜂鸣器总开关(有源) */
static uint8_t  set_shake = 1;              /* 摇动翻页开关(MPU6050) */
static uint8_t  set_oracle_n = 3;
static uint8_t  set_oracle_win = 0;
static uint8_t  set_cursor = UI_CURSOR_DEFAULT;
static uint32_t set_boot_count = 0;         /* 开机次数(NVS "info") */

static const uint16_t set_timeout_opt[] = { 30, 60, 300, 0 };
static const char *set_timeout_name[] = { "30秒", "1分", "5分", "永" };
#define SET_TIMEOUT_N (sizeof(set_timeout_opt) / sizeof(set_timeout_opt[0]))

static const uint8_t set_vol_opt[] = { 0, 20, 40, 60, 80, 100 };   /* 音量 0~100 每20一跳 */
#define SET_VOL_N (sizeof(set_vol_opt) / sizeof(set_vol_opt[0]))

static const uint8_t set_oracle_n_opt[] = { 0, 1, 3, 5, 9 };
#define SET_ORACLE_N_N (sizeof(set_oracle_n_opt) / sizeof(set_oracle_n_opt[0]))

static const uint16_t set_oracle_win_opt[][2] = {
    { 480, 1320 },    /* 0 白天 8-22 */
    { 0,   1440 },    /* 1 全天 0-24 */
    { 1200, 1440 },   /* 2 晚上 20-24 */
    { 0,   480 },     /* 3 凌晨 0-8 */
};
static const char *set_oracle_win_name[] = { "白天", "全天", "晚上", "凌晨" };
#define SET_ORACLE_WIN_N (sizeof(set_oracle_win_opt) / sizeof(set_oracle_win_opt[0]))

static const char *set_cursor_name[] = { "白线", "白块", "角框" };

static char settings_buf[SET_IDX_COUNT][24];   /* 24B: "接收指令 白天" 等长项不被截断 */
static const char *settings_items[SET_IDX_COUNT];

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("set", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u16(h, "to", set_timeout_sec);
        nvs_set_u8(h, "vol", set_vol);
        nvs_set_u8(h, "bep", set_beep);
        nvs_set_u8(h, "shk", set_shake);
        nvs_set_u8(h, "on", set_oracle_n);
        nvs_set_u8(h, "ow", set_oracle_win);
        nvs_set_u8(h, "cur", set_cursor);
        nvs_commit(h);
        nvs_close(h);
    }
}

void SET_Init(void)
{
    nvs_handle_t h;
    if (nvs_open("set", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u16(h, "to", &set_timeout_sec);
        nvs_get_u8(h, "vol", &set_vol);
        nvs_get_u8(h, "bep", &set_beep);
        nvs_get_u8(h, "shk", &set_shake);
        nvs_get_u8(h, "on", &set_oracle_n);
        nvs_get_u8(h, "ow", &set_oracle_win);
        nvs_get_u8(h, "cur", &set_cursor);
        nvs_close(h);
    }
    /* NVS 载入后范围钳位(防损坏/手改越界: to 用选项集合, on/ow 用作数组上限/索引, 见 SET_OracleN/WinRange) */
    {
        uint8_t i;
        for (i = 0; i < SET_TIMEOUT_N; i++)
            if (set_timeout_sec == set_timeout_opt[i]) break;
        if (i == SET_TIMEOUT_N) set_timeout_sec = 60;
        for (i = 0; i < SET_ORACLE_N_N; i++)
            if (set_oracle_n == set_oracle_n_opt[i]) break;
        if (i == SET_ORACLE_N_N) set_oracle_n = 3;
    }
    if (set_oracle_win >= SET_ORACLE_WIN_N) set_oracle_win = 0;
    if (set_vol > 100) set_vol = 100;
    set_beep = set_beep ? 1 : 0;
    set_shake = set_shake ? 1 : 0;
    if (set_cursor >= UI_CURSOR_N) set_cursor = UI_CURSOR_DEFAULT;
    INS_SetBeep(set_beep);
    SOUND_SetVolume(set_vol);
    MPU_SetShake(set_shake);
    UI_SetCursorStyle(set_cursor);

    /* 开机次数: 读 +1 再存 */
    if (nvs_open("info", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_get_u32(h, "boot", &set_boot_count);
        set_boot_count++;
        nvs_set_u32(h, "boot", set_boot_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ================= 对外查询 ================= */
uint16_t SET_TimeoutSec(void) { return set_timeout_sec; }
uint8_t  SET_Vol(void)        { return set_vol; }
uint8_t  SET_Beep(void)       { return set_beep; }
uint8_t  SET_OracleN(void)    { return set_oracle_n; }
uint8_t  SET_OracleWin(void)  { return set_oracle_win; }
uint32_t SET_BootCount(void)  { return set_boot_count; }
const uint16_t *SET_OracleWinRange(void) { return set_oracle_win_opt[set_oracle_win]; }

/* 音量 0~100(WEB 配置用) */
void SET_SetVol(uint8_t v)
{
    if (v > 100) v = 100;
    set_vol = v;
    SOUND_SetVolume(set_vol);
    settings_save();
}

/* 蜂鸣器开关(WEB 配置用) */
void SET_SetBeep(uint8_t on)
{
    set_beep = on ? 1 : 0;
    INS_SetBeep(set_beep);
    settings_save();
}

/* 摇动翻页开关 */
void SET_SetShake(uint8_t on)
{
    set_shake = on ? 1 : 0;
    MPU_SetShake(set_shake);
    settings_save();
}

/* 熄屏时长秒(WEB 配置用; 0=永不) */
void SET_SetTimeout(uint16_t sec)
{
    set_timeout_sec = sec;
    settings_save();
}

/* 神谕每日条数(WEB 配置用; 0=关) */
void SET_SetOracleN(uint8_t n)
{
    if (n > 9) n = 9;
    set_oracle_n = n;
    settings_save();
}

/* 神谕时段预设索引(WEB 配置用; 0=白天 1=全天 2=晚上 3=凌晨) */
void SET_SetOracleWin(uint8_t idx)
{
    if (idx >= SET_ORACLE_WIN_N) idx = (uint8_t)(SET_ORACLE_WIN_N - 1);
    set_oracle_win = idx;
    settings_save();
}

/* 光标样式: 白线/白块/角框(WEB 配置用) */
void SET_SetCursor(uint8_t style)
{
    if (style >= UI_CURSOR_N) style = UI_CURSOR_DEFAULT;
    set_cursor = style;
    UI_SetCursorStyle(set_cursor);
    settings_save();
}

/* ================= 子菜单交互 ================= */
static uint8_t settings_timeout_idx(void)
{
    uint8_t i;
    for (i = 0; i < SET_TIMEOUT_N; i++)
    {
        if (set_timeout_opt[i] == set_timeout_sec) return i;
    }
    return 1;
}

static uint8_t settings_oracle_n_idx(void)
{
    uint8_t i;
    for (i = 0; i < SET_ORACLE_N_N; i++)
    {
        if (set_oracle_n_opt[i] == set_oracle_n) return i;
    }
    return 2;
}

static void settings_items_refresh(void)
{
    uint8_t i;
    snprintf(settings_buf[SET_IDX_SCREEN], sizeof(settings_buf[0]),
             "息屏 %s", set_timeout_name[settings_timeout_idx()]);
    snprintf(settings_buf[SET_IDX_VOL], sizeof(settings_buf[0]),
             "音量 %d", set_vol);
    snprintf(settings_buf[SET_IDX_BEEP], sizeof(settings_buf[0]),
             "蜂鸣 %s", set_beep ? "开" : "关");
    snprintf(settings_buf[SET_IDX_SHAKE], sizeof(settings_buf[0]),
             "摇动 %s", set_shake ? "开" : "关");
    snprintf(settings_buf[SET_IDX_ORACLE_N], sizeof(settings_buf[0]),
             "接收 %d条", set_oracle_n);
    snprintf(settings_buf[SET_IDX_ORACLE_WIN], sizeof(settings_buf[0]),
             "指令 %s", set_oracle_win_name[set_oracle_win]);
    snprintf(settings_buf[SET_IDX_BAL], sizeof(settings_buf[0]), "平衡");
    snprintf(settings_buf[SET_IDX_INFO], sizeof(settings_buf[0]), "系统信息");
    snprintf(settings_buf[SET_IDX_RESET], sizeof(settings_buf[0]), "初始化");
    snprintf(settings_buf[SET_IDX_INS_FONT], sizeof(settings_buf[0]),
             "破译字 %dpx", 16 + INS_Font() * 8);
    snprintf(settings_buf[SET_IDX_CURSOR], sizeof(settings_buf[0]),
             "光标 %s", set_cursor_name[set_cursor]);
    strcpy(settings_buf[SET_IDX_EXIT], "退出");
    for (i = 0; i < SET_IDX_COUNT; i++)
    {
        settings_items[i] = settings_buf[i];
    }
}

void SET_SubmenuEnter(void)
{
    settings_items_refresh();
    UI_SubMenuInitItems(settings_items, SET_IDX_COUNT);   /* 右对齐(与主菜单同位置) */
}

uint8_t SET_SubmenuCount(void)
{
    return (uint8_t)SET_IDX_COUNT;
}

void SET_SubmenuSelect(uint8_t sel)
{
    if (sel == SET_IDX_SCREEN)   /* 熄屏: 循环时长 */
    {
        uint8_t idx = (uint8_t)((settings_timeout_idx() + 1) % SET_TIMEOUT_N);
        set_timeout_sec = set_timeout_opt[idx];
        settings_save();
        snprintf(settings_buf[SET_IDX_SCREEN], sizeof(settings_buf[0]),
                 "息屏 %s", set_timeout_name[idx]);
        UI_SubMenuSetItem(SET_IDX_SCREEN, settings_buf[SET_IDX_SCREEN]);
    }
    else if (sel == SET_IDX_VOL)   /* 音量: 循环档位 */
    {
        uint8_t idx = 0, i;
        for (i = 0; i < SET_VOL_N; i++)
        {
            if (set_vol_opt[i] == set_vol) { idx = i; break; }
        }
        idx = (uint8_t)((idx + 1) % SET_VOL_N);
        set_vol = set_vol_opt[idx];
        SOUND_SetVolume(set_vol);
        settings_save();
        snprintf(settings_buf[SET_IDX_VOL], sizeof(settings_buf[0]),
                 "音量 %d", set_vol);
        UI_SubMenuSetItem(SET_IDX_VOL, settings_buf[SET_IDX_VOL]);
    }
    else if (sel == SET_IDX_BEEP)   /* 蜂鸣器: 开/关(有源) */
    {
        set_beep = set_beep ? 0 : 1;
        INS_SetBeep(set_beep);
        if (set_beep) INS_BeepTimes(1);   /* 打开时试响一下 */
        settings_save();
        snprintf(settings_buf[SET_IDX_BEEP], sizeof(settings_buf[0]),
                 "蜂鸣 %s", set_beep ? "开" : "关");
        UI_SubMenuSetItem(SET_IDX_BEEP, settings_buf[SET_IDX_BEEP]);
    }
    else if (sel == SET_IDX_SHAKE)   /* 摇动翻页: 开/关(MPU6050) */
    {
        set_shake = set_shake ? 0 : 1;
        MPU_SetShake(set_shake);
        settings_save();
        snprintf(settings_buf[SET_IDX_SHAKE], sizeof(settings_buf[0]),
                 "摇动 %s", set_shake ? "开" : "关");
        UI_SubMenuSetItem(SET_IDX_SHAKE, settings_buf[SET_IDX_SHAKE]);
    }
    else if (sel == SET_IDX_ORACLE_N)   /* 神谕条数: 循环 */
    {
        uint8_t idx = (uint8_t)((settings_oracle_n_idx() + 1) % SET_ORACLE_N_N);
        set_oracle_n = set_oracle_n_opt[idx];
        settings_save();
        snprintf(settings_buf[SET_IDX_ORACLE_N], sizeof(settings_buf[0]),
                 "接收 %d条", set_oracle_n);
        UI_SubMenuSetItem(SET_IDX_ORACLE_N, settings_buf[SET_IDX_ORACLE_N]);
    }
    else if (sel == SET_IDX_ORACLE_WIN)  /* 神谕时段: 循环 */
    {
        set_oracle_win = (uint8_t)((set_oracle_win + 1) % SET_ORACLE_WIN_N);
        settings_save();
        snprintf(settings_buf[SET_IDX_ORACLE_WIN], sizeof(settings_buf[0]),
                 "指令 %s", set_oracle_win_name[set_oracle_win]);
        UI_SubMenuSetItem(SET_IDX_ORACLE_WIN, settings_buf[SET_IDX_ORACLE_WIN]);
    }
    else if (sel == SET_IDX_INS_FONT)   /* 破译字号: 循环 16/24/32px(破译行数自动匹配) */
    {
        uint8_t f = (uint8_t)((INS_Font() + 1) % 3);
        INS_SetFont(f);                     /* INS_SetFont 内部已写 NVS "ins2"/"fnt" */
        snprintf(settings_buf[SET_IDX_INS_FONT], sizeof(settings_buf[0]),
                 "破译字 %dpx", 16 + f * 8);
        UI_SubMenuSetItem(SET_IDX_INS_FONT, settings_buf[SET_IDX_INS_FONT]);
    }
    else if (sel == SET_IDX_CURSOR)   /* 光标样式: 白线/白块/角框 循环 */
    {
        SET_SetCursor((uint8_t)((set_cursor + 1) % UI_CURSOR_N));
        snprintf(settings_buf[SET_IDX_CURSOR], sizeof(settings_buf[0]),
                 "光标 %s", set_cursor_name[set_cursor]);
        UI_SubMenuSetItem(SET_IDX_CURSOR, settings_buf[SET_IDX_CURSOR]);
    }
    /* SET_IDX_INFO(系统信息) 由 main.c 处理: 显示全屏信息页 */
}

/* ================= 系统信息页(乱码破译格式, 3 页上下翻页) =================
 * 由 main.c 进入; 上下键翻页(SET_InfoNav), 其他键返回设置子菜单.
 *  页0 系统: 堆 / 栈 / 开机次数
 *  页1 签收: 神谕接收次数 / 每日签次数 / 已抽人格数(网页图鉴共 120)
 *  页2 战绩: 拼点累计积分 / 历史最高伤害 / 历史最高连胜 */
#define SET_INFO_PAGES  3
static uint8_t set_info_page = 0;

/* 读 NVS "info"/"dsign"(main.c 每日签时 +1) */
static uint32_t set_dsign_count(void)
{
    nvs_handle_t h;
    uint32_t ds = 0;
    if (nvs_open("info", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u32(h, "dsign", &ds);
        nvs_close(h);
    }
    return ds;
}

/* 读 GACHA 拼点战绩(NVS "coin"; 与 GACHA.c coin_score_load 同键) */
static void set_gacha_stats(int32_t *total, int32_t *maxdmg, int32_t *maxstrk)
{
    nvs_handle_t h;
    *total = 0; *maxdmg = 0; *maxstrk = 0;
    if (nvs_open("coin", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_i32(h, "total", total);
        nvs_get_i32(h, "max", maxdmg);
        nvs_get_i32(h, "smax", maxstrk);
        nvs_close(h);
    }
}

static void set_info_render(void)
{
    char buf[72];
    switch (set_info_page)
    {
        case 0:
            snprintf(buf, sizeof(buf), "堆 %u\n栈 %u\n开机 %u次",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)set_boot_count);
            break;
        case 1:
            snprintf(buf, sizeof(buf), "神谕 %u次\n每日签 %u次\n已抽 %u/120",
                     (unsigned)ORACLE_Count(),
                     (unsigned)set_dsign_count(),
                     (unsigned)GACHA_CoinOwnedCount());
            break;
        default:
        {
            int32_t total, maxdmg, maxstrk;
            set_gacha_stats(&total, &maxdmg, &maxstrk);
            snprintf(buf, sizeof(buf), "积分 %d\n最高伤 %d\n最高连 %d",
                     (int)total, (int)maxdmg, (int)maxstrk);
            break;
        }
    }
    INS_Show(buf);              /* 复用乱码破译 */
}

void SET_ShowInfo(void)
{
    set_info_page = 0;
    set_info_render();
}

void SET_InfoNav(uint8_t evt)   /* evt: 1=UP 2=OK 3=DOWN(与 main.c EVT_* 一致) */
{
    if (evt == 1 && set_info_page > 0)            /* UP: 上一页 */
    {
        set_info_page--;
        set_info_render();
    }
    else if (evt == 3)                             /* DOWN: 下一页(循环) */
    {
        set_info_page = (uint8_t)((set_info_page + 1) % SET_INFO_PAGES);
        set_info_render();
    }
}
