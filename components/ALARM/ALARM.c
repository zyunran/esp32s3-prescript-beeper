/* ALARM 组件: 闹钟(最多 16 段, 每天重复/按星期/一次性 时:分)
 *  - 二级菜单: 添加闹钟 / 当前闹钟 / 退出
 *  - 添加(单屏设定): 左侧 时/分 竖向数字滚轮(偏左, 光标=中间当前值高亮+角框)
 *    + 右侧 确认/重试/退出 菜单. 上下键改当前光标处值/移菜单选择,
 *    OK 移光标(时->分->菜单), 长按OK 取消回二级菜单.
 *  - 当前闹钟: 列表(时间+开/关), OK 切换开关, 长按OK 删除, "退出"回二级菜单
 *  - 触发: 到点且开启且当日未触发 -> ALM_Check 返回 1(待机 tick 中也会查); 调用方 ALM_Show 乱码破译指令
 * 数据 NVS 持久化; 绘制用 UI 组件帧缓冲/子菜单接口. */
#include "ALARM.h"
#include "UI.h"
#include "INSTRUCTION.h"
#include "NET.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ALM_MAX  16   /* 闹钟最大段数(旧固件存的 3 段 NVS 自动兼容: 按实际长度读取) */

/* ================= 闹钟专属指令(代码集中配置, 响时随机取一条; 显示自动加"致当前使用者") =================
 * 改这里即可增删/修改; 文字须在全字库 lcdzh.c 内 */
static const char *alm_presets[] = {
    "起床。",
    "喝水。",
    "关灯睡觉。",
    "出门走走。",
    "读书一刻钟。",
};
#define ALM_PRESET_COUNT (sizeof(alm_presets) / sizeof(alm_presets[0]))

/* ================= 闹钟槽 ================= */
typedef struct {
    uint8_t en;        /* 1=开启 */
    uint8_t hh;        /* 时 0-23 */
    uint8_t mm;        /* 分 0-59 */
    uint8_t days;      /* 星期位掩码: bit0=周日..bit6=周六; 0x7F=每天 */
    uint8_t once;      /* 1=一次性(到时自动关) */
    uint32_t lastday;  /* 上次触发日(epoch 天数, 每日去重; 0=重设后未触发) */
} alm_slot_t;
static alm_slot_t alm[ALM_MAX];

/* 跨任务互斥: 网页 httpd 的 ALM_GetSlot/SetSlot 逐字段读写 vs ui_task 每秒 ALM_Check 判触发,
 * 无锁时撕裂读可致一次性闹钟错响/漏响/该关没关, 双侧并发 alm_save 还会落盘撕裂槽. */
static SemaphoreHandle_t alm_mux = NULL;
static void alm_lock(void)   { if (alm_mux) xSemaphoreTakeRecursive(alm_mux, portMAX_DELAY); }
static void alm_unlock(void) { if (alm_mux) xSemaphoreGiveRecursive(alm_mux); }

/* ================= 重复模式(设定屏滚轮): 每天/工作日(一~五)/周末/一次性 ================= */
#define ALM_MODE_N  4
static const char  *alm_mode_names[ALM_MODE_N] = { "每天", "工作日", "周末", "一次性" };
static const uint8_t alm_mode_days[ALM_MODE_N] = { 0x7F, 0x3E, 0x41, 0x7F };
static const uint8_t alm_mode_once[ALM_MODE_N] = { 0, 0, 0, 1 };

/* 由存储值推断模式显示名(网页"自定义"星期也显示为 自定义) */
static const char *alm_mode_label(uint8_t days, uint8_t once)
{
    if (once) return "一次性";
    if (days == 0x7F) return "每天";
    if (days == 0x3E) return "工作日";
    if (days == 0x41) return "周末";
    return "自定义";
}

/* ================= 界面状态 ================= */
typedef enum { AL_MENU, AL_SETUP, AL_LIST } alm_phase_t;
static alm_phase_t alm_ph = AL_MENU;
static uint8_t alm_busy = 0;
static uint8_t al_cur;          /* 设定屏光标: 0=时 1=分 2=重复 3=右侧菜单 */
static uint8_t al_hh, al_mm;    /* 设定中的值 */
static uint8_t al_mode;         /* 重复模式(0=每天 1=工作日 2=周末 3=一次性) */
static uint8_t al_menu_sel;     /* 右侧菜单选中: 0=确认 1=重试 2=退出 */
static int16_t al_slide_hh, al_slide_mm, al_slide_mode;  /* 改值滑动偏移(从 8 归 0) */
static uint32_t al_slide_last;

/* 二级菜单项 */
static const char *alm_menu_items[] = { "添加闹钟", "当前闹钟", "退出" };
#define AL_MENU_ADD    0
#define AL_MENU_LIST   1
#define AL_MENU_EXIT   2

/* 设定屏布局(竖向滚轮): 左=时/分/重复 各一列竖向(当前值居中+上下邻值), 右=确认/重试/退出
 * 整体上移使"时-光标-重复-分"停在屏幕垂直正中一行(76 高 -> 行顶 y=30) */
#define AL_SET_HH_CX    46               /* 时列中心 x */
#define AL_SET_MM_CX    118              /* 分列中心 x */
#define AL_SET_MODE_CX  196              /* 重复(模式)列中心 x */
#define AL_SET_LBL_OFF  34               /* 标签在数字中心左侧偏移 */
#define AL_SET_R0       6                /* 上邻值 y */
#define AL_SET_R1       30               /* 当前值 y(光标处, 垂直居中) */
#define AL_SET_R2       54               /* 下邻值 y */
#define AL_SET_MENU_X   232              /* 右栏菜单 x */
#define AL_SET_MENU_Y0  12               /* 菜单第0项 y */
#define AL_SET_MENU_GAP 18               /* 菜单项行距 */

static uint32_t alm_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t alm_today(void)
{
    /* 本地日(本地零点起的 epoch 天数): 与 ALM_Check 的 hh/mm/wday 触发判断同一时区基准,
     * 避免用 UTC 天数做去重键在本地子夜附近错位(漏触发/重复触发/一次性不关) */
    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);
    t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
    t.tm_isdst = -1;                     /* 交给 mktime 按当前 TZ/DST 规则归一 */
    return (uint32_t)(mktime(&t) / 86400);
}

/* ================= NVS 持久化 ================= */
static const char *TAG = "ALM";
static void alm_save(void)
{
    nvs_handle_t h;
    alm_lock();   /* 整表快照落盘与过路写串行(嵌套/递归调用安全) */
    if (nvs_open("alarm", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_blob(h, "alm", alm, sizeof(alm));
        if (nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "alarm nvs commit failed");
        nvs_close(h);
    }
    alm_unlock();
}

/* ================= 绘制工具 ================= */

/* 画一列竖向滚轮: 标签(中间行) + 上邻/当前/下邻 三个值, 当前值高亮+统一光标(active) */
static void alm_draw_col(int16_t cx, const char *label, uint8_t v,
                         uint8_t minv, uint8_t maxv, int16_t slide, uint8_t active)
{
    int8_t vs[3] = { (int8_t)v - 1, (int8_t)v, (int8_t)v + 1 };
    int16_t ys[3] = { AL_SET_R0, AL_SET_R1, AL_SET_R2 };
    uint8_t i;
    UI_ScrText((uint16_t)(cx - AL_SET_LBL_OFF), AL_SET_R1, label, UI_COLOR_DATE, UI_COLOR_BG);
    for (i = 0; i < 3; i++)
    {
        char s[4], c[2];
        int16_t x, w;
        uint16_t fc;
        if (vs[i] < (int8_t)minv || vs[i] > (int8_t)maxv)
        {
            continue;
        }
        fc = (i == 1) ? (active ? UI_COLOR_FRAME : UI_COLOR_TIME) : UI_COLOR_DATE;
        c[1] = '\0';
        snprintf(s, sizeof(s), "%d", vs[i]);
        w = (vs[i] >= 10) ? 16 : 8;
        x = cx - w / 2;
        for (char *p = s; *p; p++)
        {
            c[0] = *p;
            x += UI_ScrGlyph((uint16_t)x, ys[i] + slide, c, fc, UI_COLOR_BG);
        }
        if (i == 1 && active)
        {
            UI_DrawCursor(s, cx - w / 2, ys[i] + slide);   /* 统一光标样式(白线/白块/角框随设置) */
        }
    }
}

/* 重复模式滚轮: 上邻/当前/下邻 三个模式名, 当前值高亮+统一光标(active); slide=切换滑动偏移 */
static void alm_draw_mode(int16_t cx, int16_t slide, uint8_t active)
{
    int8_t vs[3];
    int16_t ys[3] = { AL_SET_R0, AL_SET_R1, AL_SET_R2 };
    uint8_t i;
    vs[0] = (int8_t)((al_mode + ALM_MODE_N - 1) % ALM_MODE_N);
    vs[1] = (int8_t)al_mode;
    vs[2] = (int8_t)((al_mode + 1) % ALM_MODE_N);
    for (i = 0; i < 3; i++)
    {
        const char *s = alm_mode_names[vs[i]];
        uint16_t fc = (i == 1) ? (active ? UI_COLOR_FRAME : UI_COLOR_TIME) : UI_COLOR_DATE;
        int16_t w = (int16_t)(strlen(s) / 3 * 16);   /* 全中文, 每字16px */
        int16_t x = cx - w / 2;
        UI_ScrText((uint16_t)x, ys[i] + slide, s, fc, UI_COLOR_BG);
        if (i == 1 && active)
        {
            UI_DrawCursor(s, x, ys[i] + slide);      /* 统一光标样式 */
        }
    }
}

/* 右栏菜单: 确认/重试/退出(光标在菜单即 al_cur==3 时选中项高亮+统一光标) */
static void alm_draw_menu(void)
{
    static const char *items[3] = { "确认", "重试", "退出" };
    uint8_t i;
    for (i = 0; i < 3; i++)
    {
        int16_t y = AL_SET_MENU_Y0 + i * AL_SET_MENU_GAP;
        uint16_t fc = UI_COLOR_TIME;
        if (al_cur == 3 && i == al_menu_sel)
        {
            fc = UI_COLOR_FRAME;
        }
        UI_ScrText(AL_SET_MENU_X, y, items[i], fc, UI_COLOR_BG);
        if (al_cur == 3 && i == al_menu_sel)
        {
            UI_DrawCursor(items[i], AL_SET_MENU_X, y);   /* 统一光标样式 */
        }
    }
}

/* 设定屏(竖向滚轮): 左时/分/重复三列滚轮 + 右菜单 */
static void alm_render_setup(void)
{
    UI_ScrClear(UI_COLOR_BG);
    alm_draw_col(AL_SET_HH_CX, "时", al_hh, 0, 23, al_slide_hh, (al_cur == 0));
    alm_draw_col(AL_SET_MM_CX, "分", al_mm, 0, 59, al_slide_mm, (al_cur == 1));
    alm_draw_mode(AL_SET_MODE_CX, al_slide_mode, (al_cur == 2));
    alm_draw_menu();
    UI_ScrBlit();
}

/* ================= 子界面切换 ================= */
static void alm_menu_enter(void)
{
    UI_SubMenuInitItems(alm_menu_items, 3);
    alm_ph = AL_MENU;
}

static void alm_setup_enter(void)
{
    alm_ph = AL_SETUP;
    al_cur = 0;
    al_menu_sel = 0;
    al_mode = 0;             /* 默认每天 */
    al_slide_hh = 0;
    al_slide_mm = 0;
    al_slide_mode = 0;
    al_slide_last = alm_now();
    alm_render_setup();
}

/* 保存当前设定值到闹钟槽(找空槽, 全满则覆盖第 0 个) */
static void alm_add_save(void)
{
    uint8_t i;
    alm_lock();   /* 找槽+写入原子, 防与网页 SetSlot 并发选重槽/撕裂 */
    for (i = 0; i < ALM_MAX; i++)
    {
        if (!alm[i].en) break;
    }
    if (i >= ALM_MAX) i = 0;
    alm[i].en = 1;
    alm[i].hh = al_hh;
    alm[i].mm = al_mm;
    alm[i].days = alm_mode_days[al_mode];
    alm[i].once = alm_mode_once[al_mode];
    alm[i].lastday = 0;
    alm_save();
    alm_unlock();
}

/* ================= 当前闹钟列表 =================
 * 只列已设闹钟(未设的槽不显示), 列表位置 -> 槽位索引 由 alm_list_map 映射 */
static char alm_list_items[ALM_MAX + 1][24];
static const char *alm_list_p[ALM_MAX + 1];
static uint8_t alm_list_map[ALM_MAX];
static uint8_t alm_list_n;              /* 已设闹钟数(不含"退出") */

static void alm_list_refresh(void)
{
    uint8_t i, n = 0;
    alm_lock();   /* 建列表与 httpd SetSlot/Check 串行, 防读到半新旧槽 */
    for (i = 0; i < ALM_MAX; i++)
    {
        /* 已设(开启 或 曾设置过)的槽都显示并标 开/关; 从未设置的槽隐藏
         * (days==0 才是"从未设置": 所有写入路径 days≥1, 删除/memset 才为 0;
         *   避免把"关闭的 00:00 闹钟"误判为未设置而无法重新开启) */
        if (alm[i].days == 0) continue;
        alm_list_map[n] = i;
        snprintf(alm_list_items[n], sizeof(alm_list_items[n]),
                 "%02u:%02u %s %s", (unsigned)alm[i].hh, (unsigned)alm[i].mm,
                 alm_mode_label(alm[i].days, alm[i].once),
                 alm[i].en ? "开" : "关");
        n++;
    }
    alm_list_n = n;
    snprintf(alm_list_items[n], sizeof(alm_list_items[n]), "退出");
    for (i = 0; i <= n; i++) alm_list_p[i] = alm_list_items[i];
    alm_unlock();
}

static void alm_list_enter(void)
{
    alm_list_refresh();
    UI_SubMenuSetCenterDx(0);   /* 归零残留偏移(GACHA 图鉴/罪人选择设过 24~32px 且不复位): 防列表整体右移 */
    UI_SubMenuInitItemsC(alm_list_p, alm_list_n + 1, 2);   /* 只列已设闹钟 + 退出 */
    alm_ph = AL_LIST;
}

static void alm_list_key_ok(void)
{
    uint8_t sel = UI_SubMenuCur();
    alm_lock();   /* 开关切换原子化(读-写-save), 防与 httpd SetSlot 并发 */
    if (sel >= alm_list_n)
    {
        alm_unlock();
        alm_menu_enter();          /* 选中"退出" -> 回二级菜单 */
        return;
    }
    /* 开关切换: 关闭的闹钟仍在列表(标"关"), 可再次开启(不再"关了就从设备消失") */
    alm[alm_list_map[sel]].en = alm[alm_list_map[sel]].en ? 0 : 1;
    if (alm[alm_list_map[sel]].en) alm[alm_list_map[sel]].lastday = 0;   /* 重新开启: 重置当日去重 */
    alm_save();
    alm_unlock();
    alm_list_enter();               /* 重建列表 */
}

/* 网页改闹钟(web_dirty): 若正停在"当前闹钟"列表, 就地重建(内容随 alm[] 更新即时反映) */
void ALM_WebChanged(void)
{
    /* 仅 ui_task 调用(web_dirty 分支), 不跨任务访问 alm[]: 列表重建的锁在 alm_list_refresh 内 */
    if (alm_busy && alm_ph == AL_LIST)
    {
        alm_list_enter();
    }
}

static void alm_list_key_long(void)   /* 长按OK 删除该闹钟 */
{
    uint8_t sel = UI_SubMenuCur();
    alm_lock();
    if (sel >= alm_list_n)
    {
        alm_unlock();
        return;
    }
    memset(&alm[alm_list_map[sel]], 0, sizeof(alm[0]));
    alm_save();
    alm_unlock();
    alm_list_enter();                /* 重建列表 */
}

/* ================= 对外接口 ================= */
void ALM_Init(void)
{
    nvs_handle_t h;
    size_t sz = sizeof(alm);
    uint8_t i;
    alm_mux = xSemaphoreCreateRecursiveMutex();   /* 先于任何任务创建(httpd/ui) */
    memset(alm, 0, sizeof(alm));
    if (nvs_open("alarm", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_blob(h, "alm", alm, &sz);
        nvs_close(h);
    }
    for (i = 0; i < ALM_MAX; i++)       /* 旧数据(无 days/once): 默认每天, 一次性关 */
    {
        if (alm[i].en && alm[i].days == 0) alm[i].days = 0x7F;
        if (alm[i].hh > 23) alm[i].hh = 23;   /* 与 ALM_SetSlot 同款钳位: 防 NVS 坏值(99:99 永不触发/显示异常) */
        if (alm[i].mm > 59) alm[i].mm = 59;
    }
}

/* ================= 槽位读写(WEB 配置用) ================= */
uint8_t ALM_Max(void)
{
    return ALM_MAX;
}

void ALM_GetSlot(uint8_t i, uint8_t *en, uint8_t *hh, uint8_t *mm, uint8_t *days, uint8_t *once)
{
    alm_lock();   /* httpd 读槽与 ui_task 触发/改槽串行: 防撕裂读 */
    if (i >= ALM_MAX)
    {
        alm_unlock();
        return;
    }
    *en = alm[i].en;
    *hh = alm[i].hh;
    *mm = alm[i].mm;
    *days = alm[i].days;
    *once = alm[i].once;
    alm_unlock();
}

void ALM_SetSlot(uint8_t i, uint8_t en, uint8_t hh, uint8_t mm, uint8_t days, uint8_t once)
{
    alm_lock();   /* httpd 写槽与 ui_task 触发/列表操作串行: 防撕裂写与同分钟重复触发判定 */
    if (i >= ALM_MAX)
    {
        alm_unlock();
        return;
    }
    /* 防御性钳位: 非法输入不落库(网页已校验, 双保险防 99:99 之类坏数据) */
    if (hh > 23) hh = 23;
    if (mm > 59) mm = 59;
    if (days > 0x7F) days &= 0x7F;      /* 只保留星期位 bit0..bit6 */
    en = en ? 1 : 0;
    once = once ? 1 : 0;
    days = days ? days : 0x7F;          /* 无星期(0)视为每天 */
    /* 值与旧槽相同则直接返回: 不重置当日触发标记(防网页整表重写致同分钟重复响铃), 也不写 NVS(防刷写磨损) */
    if (alm[i].en == en && alm[i].hh == hh && alm[i].mm == mm &&
        alm[i].days == days && alm[i].once == once)
    {
        alm_unlock();
        return;
    }
    alm[i].en = en;
    alm[i].hh = hh;
    alm[i].mm = mm;
    alm[i].days = days;
    alm[i].once = once;
    alm[i].lastday = 0;   /* 真变更才重置当日触发标记 */
    alm_save();           /* 嵌套锁(递归); 落盘与"改动"同锁, 防交叉写 */
    alm_unlock();
}

void ALM_Enter(void)
{
    alm_busy = 1;
    alm_menu_enter();
}

void ALM_Key(uint8_t up, uint8_t ok, uint8_t down, uint8_t lng)
{
    if (!alm_busy)
    {
        return;
    }
    switch (alm_ph)
    {
        case AL_MENU:
            if (up) UI_SubMenuScroll(1);
            else if (down) UI_SubMenuScroll(-1);
            else if (lng) alm_busy = 0;                 /* 长按OK: 回 TTL 子菜单 */
            else if (ok)
            {
                uint8_t sel = UI_SubMenuCur();
                if (sel == AL_MENU_ADD) alm_setup_enter();
                else if (sel == AL_MENU_LIST) alm_list_enter();
                else alm_busy = 0;                      /* 退出 */
            }
            break;

        case AL_SETUP:
            if (up || down)
            {
                int8_t d = up ? 1 : -1;
                if (al_cur == 0)
                {
                    al_hh = (uint8_t)((al_hh + 24 + d) % 24);
                    al_slide_hh = (int16_t)(up ? 8 : -8);
                }
                else if (al_cur == 1)
                {
                    al_mm = (uint8_t)((al_mm + 60 + d) % 60);
                    al_slide_mm = (int16_t)(up ? 8 : -8);
                }
                else if (al_cur == 2)
                {
                    al_mode = (uint8_t)((al_mode + ALM_MODE_N + d) % ALM_MODE_N);   /* 重复模式 */
                    al_slide_mode = (int16_t)(up ? 8 : -8);                         /* 切换滑动动画 */
                }
                else
                {
                    al_menu_sel = (uint8_t)((al_menu_sel + 3 + d) % 3);
                }
                al_slide_last = alm_now();
                alm_render_setup();
            }
            else if (lng) alm_menu_enter();              /* 长按OK: 取消设定回二级菜单 */
            else if (ok)
            {
                if (al_cur == 0) { al_cur = 1; alm_render_setup(); }        /* 光标: 时 -> 分 */
                else if (al_cur == 1) { al_cur = 2; alm_render_setup(); }   /* 分 -> 重复 */
                else if (al_cur == 2) { al_cur = 3; alm_render_setup(); }   /* 重复 -> 右侧菜单 */
                else if (al_menu_sel == 0) { alm_add_save(); alm_menu_enter(); } /* 确认保存 -> 回二级菜单 */
                else if (al_menu_sel == 1) { al_cur = 0; alm_render_setup(); }   /* 重试 -> 回时重新设定 */
                else alm_menu_enter();                                   /* 退出: 放弃 -> 回二级菜单 */
            }
            break;

        case AL_LIST:
            if (up) UI_SubMenuScroll(1);
            else if (down) UI_SubMenuScroll(-1);
            else if (lng) alm_list_key_long();
            else if (ok) alm_list_key_ok();
            break;
    }
}

void ALM_Tick(void)
{
    uint32_t now;
    uint8_t chg = 0;
    if (!alm_busy || alm_ph != AL_SETUP)
    {
        return;
    }
    now = alm_now();
    if (now - al_slide_last < 15)
    {
        return;
    }
    al_slide_last = now;
    if (al_slide_hh != 0)
    {
        al_slide_hh += (al_slide_hh > 0) ? -3 : 3;
        if (al_slide_hh >= -3 && al_slide_hh <= 3) al_slide_hh = 0;
        chg = 1;
    }
    if (al_slide_mm != 0)
    {
        al_slide_mm += (al_slide_mm > 0) ? -3 : 3;
        if (al_slide_mm >= -3 && al_slide_mm <= 3) al_slide_mm = 0;
        chg = 1;
    }
    if (al_slide_mode != 0)
    {
        al_slide_mode += (al_slide_mode > 0) ? -3 : 3;
        if (al_slide_mode >= -3 && al_slide_mode <= 3) al_slide_mode = 0;
        chg = 1;
    }
    if (chg)
    {
        alm_render_setup();
    }
}

uint8_t ALM_Busy(void)
{
    return alm_busy;
}

/* 每秒检查: 有开启且到时且当日未触发的闹钟 -> 标记当日已触发并返回 1 */
uint8_t ALM_Check(void)
{
    time_t now;
    struct tm t;
    uint32_t day;
    uint8_t i, r = 0;
    if (!NET_TimeOk())
    {
        return 0;                 /* 未校时, 无时间概念 */
    }
    alm_lock();   /* 判触发整槽读 + 改 lastday/en 原子, 防与网页 SetSlot 撕裂 */
    time(&now);
    localtime_r(&now, &t);
    day = alm_today();
    for (i = 0; i < ALM_MAX; i++)
    {
        if (alm[i].en && (alm[i].days & (1 << t.tm_wday)) &&     /* 星期匹配(bit0=周日) */
            alm[i].hh == t.tm_hour && alm[i].mm == t.tm_min && alm[i].lastday != day)
        {
            alm[i].lastday = day; /* 标记当日已触发, 防止同分钟重复 */
            if (alm[i].once) alm[i].en = 0;   /* 一次性: 到时自动关闭 */
            alm_save();
            r = 1;
            break;
        }
    }
    alm_unlock();
    return r;
}

/* 闹钟到点: 乱码破译显示一条闹钟专属指令(自动加"致当前使用者") */
void ALM_Show(void)
{
    INS_ShowIns(alm_presets[esp_random() % ALM_PRESET_COUNT]);
}
