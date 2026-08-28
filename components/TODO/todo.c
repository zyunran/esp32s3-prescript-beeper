/* TODO 组件: 待办/指令日志(主菜单"待办")
 *  - 产生: 指令文本 {TODO} 前缀(INSTRUCTION.INS_Show 解析时自动 TODO_Add, 去重)
 *  - 界面: 子菜单列表(块左对齐, 全文), OK 重新破译显示(由 main 调 INS_Show),
 *    长按OK 标记 PASS/恢复(加 "PASS " 前缀), 末项"退出"回主界面
 *  - 网页: GET /api/todo 列表; POST /api/todo {op:add/toggle/del/clear}
 * 数据 NVS "todo" 持久化; 绘制用 UI 组件子菜单接口. */
#include "TODO.h"
#include "UI.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TODO_MAX   12          /* 最大待办条数(UI 子菜单最多 12 项+退出) */
#define TODO_TEXT_MAX_W 244    /* 列表文本最大宽 px(284 屏宽 - "PASS " 前缀 40px - 余量; 保证整项不出屏且 UTF-8 截断不切字) */

typedef struct {
    char text[TODO_TEXT_MAX];  /* 待办文本 */
    uint8_t done;              /* 1=已PASS */
    uint32_t created;          /* 创建时刻(ms). 保留占位: 与 NVS 旧 blob 布局兼容 */
    uint8_t remind_en;         /* 1=启用提醒 */
    uint8_t remind_hh;         /* 提醒小时 0-23 */
    uint8_t remind_mm;         /* 提醒分钟 0-59 */
    char remind_date[8];       /* 上次提醒日期 "MM-DD", 同一天不重复提醒 */
} todo_item_t;

static todo_item_t todo[TODO_MAX];
static uint8_t todo_busy = 0;

/* 跨任务互斥: 网页(httpd 任务)直调 TODO_* 与设备端(ui_task)并发改同一 todo[],
 * 无锁时 Add/Toggle/Del 交错会撕裂数组并整表落盘脏数据(PASS 错行/丢条/重启后字段错配).
 * 递归互斥: 组件内函数互相调用(Add 内 todo_count/save)不重入死锁. */
static SemaphoreHandle_t todo_mux = NULL;
static void todo_lock(void)   { if (todo_mux) xSemaphoreTakeRecursive(todo_mux, portMAX_DELAY); }
static void todo_unlock(void) { if (todo_mux) xSemaphoreGiveRecursive(todo_mux); }

/* 列表项显示缓冲(含 PASS 前缀, 按像素宽截断, 防出屏) */
static char todo_remind_text[TODO_TEXT_MAX];  /* 当前到点提醒文本快照 */
static char todo_items[TODO_MAX + 1][64];
static const char *todo_item_p[TODO_MAX + 1];

static void todo_item_fill(uint8_t i, char *out, size_t outsz);   /* 前置声明(定义在下文) */

/* ================= NVS 持久化 ================= */
static const char *TAG = "TODO";
static void todo_save(void)
{
    nvs_handle_t h;
    if (nvs_open("todo", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_blob(h, "todo", todo, sizeof(todo));
        if (nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "todo nvs commit failed");
        nvs_close(h);
    }
}

static uint8_t todo_count(void)
{
    uint8_t i, n = 0;
    for (i = 0; i < TODO_MAX; i++)
    {
        if (todo[i].text[0]) n++;
    }
    return n;
}

void TODO_Init(void)
{
    nvs_handle_t h;
    size_t sz = sizeof(todo);
    todo_mux = xSemaphoreCreateRecursiveMutex();   /* 先于任何任务创建(httpd/ui) */
    memset(todo, 0, sizeof(todo));
    if (nvs_open("todo", NVS_READONLY, &h) == ESP_OK)
    {
        if (nvs_get_blob(h, "todo", todo, &sz) == ESP_OK && sz < sizeof(todo))
        {
            /* 旧版本 blob 较小: 新字段(提醒)置零, 保留旧数据 */
            memset((uint8_t *)todo + sz, 0, sizeof(todo) - sz);
        }
        nvs_close(h);
    }
}

/* 追加一条(去重: 已有相同文本不重复加); 满则不存 */
uint8_t TODO_Add(const char *text)
{
    size_t len = strlen(text);
    uint8_t i;
    uint8_t n, r = 0;
    if (len == 0 || len >= TODO_TEXT_MAX) return 0;
    todo_lock();
    n = todo_count();
    for (i = 0; i < n; i++)
    {
        if (strcmp(todo[i].text, text) == 0) goto out;   /* 去重: 已有 */
    }
    if (n >= TODO_MAX) goto out;                          /* 满 */
    strncpy(todo[n].text, text, TODO_TEXT_MAX - 1);
    todo[n].text[TODO_TEXT_MAX - 1] = '\0';
    todo[n].done = 0;
    todo[n].created = (uint32_t)(esp_timer_get_time() / 1000);
    todo_save();
    r = 1;
out:
    todo_unlock();
    return r;
}

/* ================= 对外查询 ================= */
uint8_t TODO_Count(void)
{
    uint8_t n;
    todo_lock();
    n = todo_count();
    todo_unlock();
    return n;
}

const char *TODO_Text(uint8_t i)
{
    const char *r;
    todo_lock();
    r = (i < todo_count()) ? todo[i].text : "";
    todo_unlock();
    return r;   /* 注意: 返回 todo 内部指针, 调用方须立即使用(锁已释放) */
}

uint8_t TODO_Done(uint8_t i)
{
    uint8_t r;
    todo_lock();
    r = (i < todo_count()) ? todo[i].done : 0;
    todo_unlock();
    return r;
}

/* ================= 网页操作 =================
 * redraw=1: 设备端(ui_task)调用, 列表就地刷新; redraw=0: 网页(httpd 任务)调用,
 * 只改数据不绘屏 —— 绘制统一在 ui_task(web_dirty 触发 TODO_Enter 重建列表), 防帧缓冲跨任务撕裂 */
void TODO_Toggle(uint8_t i, uint8_t redraw)
{
    uint8_t cur;
    todo_lock();
    if (i >= todo_count())
    {
        todo_unlock();
        return;
    }
    todo[i].done = !todo[i].done;
    todo_save();
    if (!redraw)
    {
        todo_unlock();
        return;
    }
    cur = UI_SubMenuCur();
    if (todo_busy && cur == i)   /* 界面正显示该项: 就地刷新文字(保选中) */
    {
        char buf[64];
        todo_item_fill(i, buf, sizeof(buf));
        UI_SubMenuSetItem(i, buf);
    }
    todo_unlock();
}

void TODO_Del(uint8_t i)
{
    uint8_t n;
    todo_lock();
    n = todo_count();
    if (i >= n)
    {
        todo_unlock();
        return;
    }
    for (; i < n - 1; i++) todo[i] = todo[i + 1];
    memset(&todo[n - 1], 0, sizeof(todo[0]));
    todo_save();
    todo_unlock();
}

void TODO_Clear(void)
{
    todo_lock();
    memset(todo, 0, sizeof(todo));
    todo_save();
    todo_unlock();
}

/* 填充列表项文字: PASS 前缀 + 文本(按像素宽截断, UTF-8 安全) */
static void todo_item_fill(uint8_t i, char *out, size_t outsz)
{
    const char *p = todo[i].text;
    size_t o = 0;
    int16_t w = 0;
    if (todo[i].done)
    {
        memcpy(out, "PASS ", 5);
        o = 5;
        w = 40;   /* 5 ASCII × 8px */
    }
    while (*p && o < outsz - 4)
    {
        uint8_t len = 1, cw = 8;
        unsigned char c = (unsigned char)*p;
        if (c & 0x80)
        {
            /* 2/3 字节 UTF-8 须区分: 一律按 3 字节复制会把 2 字节字符(如 ·)的下一字首字节吞进本字 */
            len = ((c & 0xE0) == 0xC0) ? 2 : 3;
            cw = 16;
            if (p[1] == '\0' || (len == 3 && p[2] == '\0')) break;   /* 残缺尾: 停, 不复制半个字 */
        }
        if (w + cw > TODO_TEXT_MAX_W) break;
        memcpy(out + o, p, len);
        o += len;
        w += cw;
        p += len;
    }
    out[o] = '\0';
}

/* ================= 界面(复用 UI 通用子菜单, 块左对齐居中) ================= */
static void todo_enter(void)
{
    uint8_t n, i;
    todo_lock();   /* 重建列表期间 httpd 侧改动(Add/Toggle/Del)不得撕裂 todo[] 读取 */
    n = todo_count();
    for (i = 0; i < n; i++)
    {
        todo_item_fill(i, todo_items[i], sizeof(todo_items[0]));
        todo_item_p[i] = todo_items[i];
    }
    strcpy(todo_items[n], "退出");
    todo_item_p[n] = todo_items[n];
    UI_SubMenuSetCenterDx(0);   /* 归零残留偏移(GACHA 图鉴/罪人选择设过 24~32px 且不复位): 防列表整体右移 */
    UI_SubMenuInitItemsC(todo_item_p, n + 1, 2);
    todo_busy = 1;
    todo_unlock();
}

void TODO_Enter(void)
{
    todo_enter();
}

uint8_t TODO_Key(uint8_t up, uint8_t ok, uint8_t down, uint8_t lng)
{
    uint8_t n, cur, r = TODO_KEY_NONE;
    todo_lock();
    if (!todo_busy) goto out;
    n = todo_count();
    cur = UI_SubMenuCur();
    if (up) { UI_SubMenuScroll(1); goto out; }
    if (down) { UI_SubMenuScroll(-1); goto out; }
    if (lng)                             /* 长按OK: PASS/恢复(在"退出"上忽略) */
    {
        if (cur < n) TODO_Toggle(cur, 1);   /* 设备端: 就地刷新 */
        goto out;
    }
    if (ok)
    {
        if (cur >= n)                    /* 选"退出" */
        {
            todo_busy = 0;
            r = TODO_KEY_EXIT;
            goto out;
        }
        r = TODO_KEY_SHOW;               /* 选待办: 需重新破译显示 */
    }
out:
    todo_unlock();
    return r;
}

const char *TODO_CurText(void)
{
    const char *r;
    uint8_t cur;
    todo_lock();
    cur = UI_SubMenuCur();
    r = (cur < todo_count()) ? todo[cur].text : NULL;
    todo_unlock();
    return r;   /* 立即使用(锁已释放), 与 TODO_Text 同约束 */
}

/* ================= 待办提醒时间(每天一次) ================= */
void TODO_SetRemind(uint8_t i, uint8_t en, uint8_t hh, uint8_t mm)
{
    todo_lock();
    if (i < todo_count())
    {
        todo[i].remind_en = en ? 1 : 0;
        todo[i].remind_hh = (hh > 23) ? 0 : hh;
        todo[i].remind_mm = (mm > 59) ? 0 : mm;
        todo[i].remind_date[0] = '\0';
        todo_save();
    }
    todo_unlock();
}

uint8_t TODO_RemindEn(uint8_t i)
{
    uint8_t r = 0;
    todo_lock();
    if (i < todo_count()) r = todo[i].remind_en;
    todo_unlock();
    return r;
}

uint8_t TODO_RemindHH(uint8_t i)
{
    uint8_t r = 0;
    todo_lock();
    if (i < todo_count()) r = todo[i].remind_hh;
    todo_unlock();
    return r;
}

uint8_t TODO_RemindMM(uint8_t i)
{
    uint8_t r = 0;
    todo_lock();
    if (i < todo_count()) r = todo[i].remind_mm;
    todo_unlock();
    return r;
}

/* 每秒检查: 有待办启用提醒且当前时:分匹配、当天未提醒过 -> 置当天日期并返回 1 */
uint8_t TODO_RemindDue(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    char today[8];
    uint8_t mon, day;
    uint8_t i, n, due = 0;

    if (!localtime_r(&now, &tmv)) return 0;
    mon = (uint8_t)(tmv.tm_mon + 1);
    day = (uint8_t)tmv.tm_mday;
    snprintf(today, sizeof(today), "%02u-%02u", (unsigned)mon, (unsigned)day);

    todo_lock();
    n = todo_count();
    for (i = 0; i < n; i++)
    {
        if (!todo[i].text[0] || !todo[i].remind_en) continue;
        if (todo[i].remind_hh == (uint8_t)tmv.tm_hour &&
            todo[i].remind_mm == (uint8_t)tmv.tm_min &&
            strcmp(todo[i].remind_date, today) != 0)
        {
            strncpy(todo[i].remind_date, today, sizeof(todo[i].remind_date) - 1);
            todo[i].remind_date[sizeof(todo[i].remind_date) - 1] = '\0';
            strncpy(todo_remind_text, todo[i].text, TODO_TEXT_MAX - 1);
            todo_remind_text[TODO_TEXT_MAX - 1] = '\0';
            todo_save();
            due = 1;
            break;
        }
    }
    todo_unlock();
    return due;
}

const char *TODO_RemindText(void)
{
    return todo_remind_text;
}
