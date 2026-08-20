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
#include <string.h>
#include <stdio.h>

#define TODO_MAX   12          /* 最大待办条数(UI 子菜单最多 12 项+退出) */
#define TODO_TEXT_MAX_W 264    /* 列表显示最大宽 px(284 内留边) */

typedef struct {
    char text[TODO_TEXT_MAX];  /* 待办文本 */
    uint8_t done;              /* 1=已PASS */
    uint32_t created;          /* 创建时刻(开机秒, 仅备将来显示) */
} todo_item_t;

static todo_item_t todo[TODO_MAX];
static uint8_t todo_busy = 0;

/* 列表项显示缓冲(含 PASS 前缀, 按像素宽截断, 防出屏) */
static char todo_items[TODO_MAX + 1][64];
static const char *todo_item_p[TODO_MAX + 1];

static void todo_item_fill(uint8_t i, char *out, size_t outsz);   /* 前置声明(定义在下文) */

/* ================= NVS 持久化 ================= */
static void todo_save(void)
{
    nvs_handle_t h;
    if (nvs_open("todo", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_blob(h, "todo", todo, sizeof(todo));
        nvs_commit(h);
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
    memset(todo, 0, sizeof(todo));
    if (nvs_open("todo", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_blob(h, "todo", todo, &sz);
        nvs_close(h);
    }
}

/* 追加一条(去重: 已有相同文本不重复加); 满则不存 */
uint8_t TODO_Add(const char *text)
{
    size_t len = strlen(text);
    uint8_t i, n;
    if (len == 0 || len >= TODO_TEXT_MAX) return 0;
    n = todo_count();
    for (i = 0; i < n; i++)
    {
        if (strcmp(todo[i].text, text) == 0) return 0;
    }
    if (n >= TODO_MAX) return 0;
    strncpy(todo[n].text, text, TODO_TEXT_MAX - 1);
    todo[n].text[TODO_TEXT_MAX - 1] = '\0';
    todo[n].done = 0;
    todo[n].created = (uint32_t)(esp_timer_get_time() / 1000);
    todo_save();
    return 1;
}

/* ================= 对外查询 ================= */
uint8_t TODO_Count(void) { return todo_count(); }

const char *TODO_Text(uint8_t i)
{
    return (i < todo_count()) ? todo[i].text : "";
}

uint8_t TODO_Done(uint8_t i)
{
    return (i < todo_count()) ? todo[i].done : 0;
}

/* ================= 网页操作 ================= */
void TODO_Toggle(uint8_t i)
{
    uint8_t cur;
    if (i >= todo_count()) return;
    todo[i].done = !todo[i].done;
    todo_save();
    cur = UI_SubMenuCur();
    if (todo_busy && cur == i)   /* 界面正显示该项: 就地刷新文字(保选中) */
    {
        char buf[64];
        todo_item_fill(i, buf, sizeof(buf));
        UI_SubMenuSetItem(i, buf);
    }
}

void TODO_Del(uint8_t i)
{
    uint8_t n = todo_count();
    if (i >= n) return;
    for (; i < n - 1; i++) todo[i] = todo[i + 1];
    memset(&todo[n - 1], 0, sizeof(todo[0]));
    todo_save();
}

void TODO_Clear(void)
{
    memset(todo, 0, sizeof(todo));
    todo_save();
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
        uint8_t len = (*p & 0x80) ? 3 : 1;
        uint8_t cw = (*p & 0x80) ? 16 : 8;
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
    uint8_t n = todo_count(), i;
    for (i = 0; i < n; i++)
    {
        todo_item_fill(i, todo_items[i], sizeof(todo_items[0]));
        todo_item_p[i] = todo_items[i];
    }
    strcpy(todo_items[n], "退出");
    todo_item_p[n] = todo_items[n];
    UI_SubMenuInitItemsC(todo_item_p, n + 1, 2);
    todo_busy = 1;
}

void TODO_Enter(void)
{
    todo_enter();
}

uint8_t TODO_Key(uint8_t up, uint8_t ok, uint8_t down, uint8_t lng)
{
    uint8_t n, cur;
    if (!todo_busy) return TODO_KEY_NONE;
    n = todo_count();
    cur = UI_SubMenuCur();
    if (up) { UI_SubMenuScroll(1); return TODO_KEY_NONE; }
    if (down) { UI_SubMenuScroll(-1); return TODO_KEY_NONE; }
    if (lng)                             /* 长按OK: PASS/恢复(在"退出"上忽略) */
    {
        if (cur < n) TODO_Toggle(cur);
        return TODO_KEY_NONE;
    }
    if (ok)
    {
        if (cur >= n)                    /* 选"退出" */
        {
            todo_busy = 0;
            return TODO_KEY_EXIT;
        }
        return TODO_KEY_SHOW;            /* 选待办: 需重新破译显示 */
    }
    return TODO_KEY_NONE;
}

const char *TODO_CurText(void)
{
    uint8_t cur = UI_SubMenuCur();
    if (cur >= todo_count()) return NULL;
    return todo[cur].text;
}
