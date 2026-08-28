/* ANSWER 组件: 「询问」答案之书
 *  - 4 分类(回答/吃什么/喝什么/玩什么)子菜单, OK 抽答案, 乱码破译显示
 *  - 答案库 = 内置默认(代码内) + 用户自定义(NVS "ans"/"c0".."c3", 网页可增删)
 *  - 显示复用 INSTRUCTION 的 INS_Show 做"乱码破译"(INSTRUCTION 是基础设施, 直接依赖)
 *  - 非阻塞状态机: ANS_MENU(分类子菜单) / ANS_DRAW(已抽, 等按键再抽/回菜单) */
#include "ANSWER.h"
#include "UI.h"
#include "INSTRUCTION.h"   /* INS_Show 破译显示答案 */
#include "evt.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

/* ================= 状态机 ================= */
typedef enum { ANS_MENU, ANS_DRAW } ans_state_t;
static ans_state_t ans_state = ANS_MENU;
static uint8_t ans_busy = 0;
static uint8_t ans_cat_cur = 0;    /* 当前分类(子菜单选中记忆) */

/* ================= 内置默认答案库(每类一数组; 数组容量 30, 实际条数见 ans_builtin_n) ================= */
static const char *const ans_builtin[ANS_CAT_N][30] = {
    /* 回答类(通用 20 + 彩蛋 10, 共 30) */
    {
    "是,但别急",
    "再等等",
    "信你的直觉",
    "答案在你身边",
    "放下,就有解",
    "去做,别错过",
    "先照顾好自己",
    "别硬来,顺其自然",
    "换个角度看",
    "问你的心",
    "今天别做决定",
    "沉默最好",
    "这条路必经",
    "就是现在",
    "说出来",
    "别想太多",
    "结果会意外",
    "你其实知道答案",
    "睡醒再想",
    "来得及",
    /* 彩蛋类 */
    "你猜",
    "去睡觉",
    "答案只有  知道",
    "你心里有数",
    "是啊,吃什么",
    "天机不可泄",
    "先喝口水",
    "再问就告诉你",
    "答案在你,不在我",
    "问得真执着",
},
/* 吃什么类 */
{
    "方便面",
    "炸酱面",
    "烧烤",
    "甜品",
    "麻辣烫",
    "饺子",
    "饭团",
    "炒饭",
    "炸鸡",
    "寿司",
    "干锅鸡",
    "汉堡",
    "沙拉",
    "披萨",
    "凉面",
},
/* 喝什么类 */
{
    "奶茶",
    "雪碧",
    "热牛奶",
    "柠檬水",
    "鲜榨果汁",
    "可乐加冰",
    "拿铁",
    "蜂蜜水",
    "啤酒",
    "绿茶",
    "白开水",
    "红豆奶茶",
},
/* 玩什么类 */
{
    "散步",
    "打游戏",
    "看电影",
    "找朋友聚聚",
    "公园发呆",
    "爬山",
    "逛书店",
    "休息",
    "骑行",
    "睡大觉",
    "拼积木",
    "江边吹风",
    "画点东西",
    "逛超市",
},
  
};
/* 每类内置条数(与上方数组实际元素数一致; 勿用 sizeof 数组, 那返回容量 30) */
static const uint8_t ans_builtin_n[ANS_CAT_N] = { 30, 15, 12, 14 };
#define ANS_BUILTIN_N(cat)  ans_builtin_n[cat]

/* ================= 有效答案库(内置默认 + 网页全量覆盖) =================
 * 存储: 每类一个换行分隔字符串(NVS "ans"/"c0".."c3")= 该分类【全部】答案;
 * 无 NVS 覆盖 -> 用内置默认(指针直接指内置字面量, 不占 RAM);
 * 有 NVS 覆盖 -> 按行拆分到 ans_nvs_buf, 指针指过去. */
static char ans_nvs_buf[ANS_CAT_N][ANS_TOTAL_MAX][ANS_LINE_MAX];  /* NVS 覆盖行的存放处 */
static const char *ans_list[ANS_CAT_N][ANS_TOTAL_MAX];            /* 有效答案指针表 */
static uint8_t ans_list_n[ANS_CAT_N];                             /* 每类有效条数 */
static uint8_t ans_loaded = 0;

/* 跨任务互斥: 网页(httpd 任务)经 ANS_FromText/ANS_Custom 读写答案库,
 * ui_task 经 ans_draw 同时读; 无锁时 httpd 逐行覆写 ans_nvs_buf/ans_list 会被
 * ui_task 抽答案读到半截串(屏上破译出乱码文本). 与 ALM/TODO/INS/GACHA 同一防护标准. */
static SemaphoreHandle_t ans_mux = NULL;
static void ans_lock(void)   { if (ans_mux) xSemaphoreTakeRecursive(ans_mux, portMAX_DELAY); }
static void ans_unlock(void) { if (ans_mux) xSemaphoreGiveRecursive(ans_mux); }

static const char *ans_nvs_key(uint8_t cat)
{
    static const char *keys[ANS_CAT_N] = { "c0", "c1", "c2", "c3" };
    return (cat < ANS_CAT_N) ? keys[cat] : "c0";
}

/* 加载某分类答案库: 默认内置; NVS 有覆盖则整类替换 */
static void ans_list_load(uint8_t cat)
{
    nvs_handle_t h;
    static char buf[ANS_TOTAL_MAX * ANS_LINE_MAX];   /* static: 不占调用栈 */
    size_t len = sizeof(buf);
    char *p, *nl;
    uint8_t n = 0, i;
    for (i = 0; i < ANS_BUILTIN_N(cat); i++)
    {
        ans_list[cat][i] = ans_builtin[cat][i];      /* 默认: 指针指内置字面量 */
    }
    n = ANS_BUILTIN_N(cat);
    if (nvs_open("ans", NVS_READONLY, &h) == ESP_OK)
    {
        if (nvs_get_str(h, ans_nvs_key(cat), buf, &len) == ESP_OK && len > 1)
        {
            p = buf;
            n = 0;
            while (*p && n < ANS_TOTAL_MAX)          /* 覆盖: 整类替换 */
            {
                size_t slen;
                nl = strchr(p, '\n');
                slen = nl ? (size_t)(nl - p) : strlen(p);
                if (slen > ANS_LINE_MAX - 1) slen = ANS_LINE_MAX - 1;   /* 截断保护 */
                if (slen > 0)
                {
                    memcpy(ans_nvs_buf[cat][n], p, slen);
                    ans_nvs_buf[cat][n][slen] = '\0';
                    ans_list[cat][n] = ans_nvs_buf[cat][n];
                    n++;
                }
                if (!nl) break;
                p = nl + 1;
            }
        }
        nvs_close(h);
    }
    ans_list_n[cat] = n;
}

/* 序列化某分类答案为换行分隔文本(buf 须 ≥ ANS_TOTAL_MAX*ANS_LINE_MAX) */
static void ans_serialize(uint8_t cat, char *buf, size_t cap)
{
    uint8_t i;
    size_t wp = 0;
    buf[0] = '\0';
    for (i = 0; i < ans_list_n[cat]; i++)
    {
        if (wp >= cap - 1) break;
        wp += (size_t)snprintf(&buf[wp], cap - wp, "%s%s",
                               (i ? "\n" : ""), ans_list[cat][i]);
    }
}

/* 立即落盘单分类(open + set/erase + commit) */
static void ans_list_save_now(uint8_t cat)
{
    nvs_handle_t h;
    static char buf[ANS_TOTAL_MAX * ANS_LINE_MAX];   /* static: 不占调用栈 */
    ans_serialize(cat, buf, sizeof(buf));
    if (nvs_open("ans", NVS_READWRITE, &h) == ESP_OK)
    {
        if (buf[0]) nvs_set_str(h, ans_nvs_key(cat), buf);
        else        nvs_erase_key(h, ans_nvs_key(cat));   /* 空 = 恢复内置 */
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 网页批量保存: Begin 后逐类只记脏掩码, End 合并为一次 open+commit */
static uint8_t ans_save_batch = 0;
static uint8_t ans_save_dirty = 0;   /* bit c = 分类 c 待落盘 */

static void ans_list_save(uint8_t cat)
{
    if (ans_save_batch)
    {
        ans_save_dirty |= (uint8_t)(1u << cat);
        return;
    }
    ans_list_save_now(cat);
}

void ANS_SaveBatchBegin(void) { ans_save_batch++; }

void ANS_SaveBatchEnd(void)
{
    nvs_handle_t h;
    static char buf[ANS_TOTAL_MAX * ANS_LINE_MAX];   /* static: 不占调用栈 */
    uint8_t c, have = 0;
    if (!ans_save_batch) return;
    ans_save_batch--;
    if (ans_save_batch || !ans_save_dirty) return;
    if (nvs_open("ans", NVS_READWRITE, &h) == ESP_OK)
    {
        for (c = 0; c < ANS_CAT_N; c++)
        {
            if (!(ans_save_dirty & (1u << c))) continue;
            ans_serialize(c, buf, sizeof(buf));
            if (buf[0]) nvs_set_str(h, ans_nvs_key(c), buf);
            else        nvs_erase_key(h, ans_nvs_key(c));   /* 空 = 恢复内置 */
            have = 1;
        }
        if (have) nvs_commit(h);
        nvs_close(h);
    }
    ans_save_dirty = 0;
}

/* ================= 内部 ================= */
/* 抽一条当前分类答案并破译显示.
 * 全程持锁: 先选串再调 INS_Show(内部拷贝进破译缓冲), 防选中串在解析中途被 httpd 覆写 */
static void ans_draw(void)
{
    uint8_t cat = ans_cat_cur;
    uint16_t total;
    const char *s;
    ans_lock();
    total = ans_list_n[cat];
    if (total == 0)
    {
        ans_unlock();
        return;
    }
    s = ans_list[cat][esp_random() % total];   /* 从有效库(内置或网页覆盖)随机 */
    INS_Show(s);            /* 乱码破译显示答案 */
    ans_unlock();
    ans_state = ANS_DRAW;
}

/* 渲染分类子菜单(回答/吃什么/喝什么/玩什么/退出) */
static void ans_menu_render(void)
{
    static const char *items[ANS_CAT_N + 1] = { ANS_CAT_NAMES, "退出" };
    UI_SubMenuInitItems(items, ANS_CAT_N + 1);
    if (ans_cat_cur < ANS_CAT_N) UI_SubMenuSetCur(ans_cat_cur);
}

/* ================= API ================= */
void ANS_Init(void)
{
    uint8_t c;
    if (ans_loaded) return;
    ans_mux = xSemaphoreCreateRecursiveMutex();   /* 先于 WEB_Init 启动 httpd(main.c 顺序保证) */
    for (c = 0; c < ANS_CAT_N; c++) ans_list_load(c);
    ans_loaded = 1;
}

void ANS_Enter(void)
{
    ans_busy = 1;
    ans_state = ANS_MENU;
    ans_menu_render();
}

void ANS_OnEvent(uint8_t evt)
{
    if (!ans_busy) return;

    if (ans_state == ANS_MENU)
    {
        if (evt == EVT_UP)                  /* 上 */
        {
            UI_SubMenuScroll(1);
            ans_cat_cur = UI_SubMenuCur();
        }
        else if (evt == EVT_DOWN)           /* 下 */
        {
            UI_SubMenuScroll(-1);
            ans_cat_cur = UI_SubMenuCur();
        }
        else if (evt == EVT_OK)             /* 确认: 选分类(前4项)或退出(末项) */
        {
            uint8_t sel = UI_SubMenuCur();
            if (sel >= ANS_CAT_N)           /* 选"退出" -> 回主界面(重绘由主循环 ui_pop 统一做) */
            {
                ans_busy = 0;
            }
            else
            {
                ans_cat_cur = sel;
                ans_draw();
            }
        }
        else if (evt == EVT_LONG_OK)        /* 长按OK: 直接退出(重绘由主循环 ui_pop 统一做) */
        {
            ans_busy = 0;
        }
    }
    else /* ANS_DRAW: 答案已显示(破译由调用方 INS_Tick 推进) */
    {
        if (evt == EVT_UP || evt == EVT_OK || evt == EVT_DOWN)   /* 上/确认/下: 再抽一条(同分类) */
        {
            ans_draw();
        }
        else if (evt == EVT_LONG_OK)        /* 长按OK: 回分类子菜单 */
        {
            ans_state = ANS_MENU;
            ans_menu_render();
        }
    }
}

uint8_t ANS_Busy(void) { return ans_busy; }

/* ================= 答案库访问(网页用) ================= */
const char *ANS_Custom(uint8_t cat)   /* 该分类【全部】答案整串文本(内置默认 或 网页覆盖) */
{
    static char out[ANS_TOTAL_MAX * ANS_LINE_MAX];
    uint8_t i;
    size_t wp = 0;
    if (cat >= ANS_CAT_N) return "";
    ans_lock();
    out[0] = '\0';
    for (i = 0; i < ans_list_n[cat]; i++)
    {
        if (wp >= sizeof(out) - 1) break;
        wp += (size_t)snprintf(&out[wp], sizeof(out) - wp, "%s%s",
                               (i ? "\n" : ""), ans_list[cat][i]);
    }
    ans_unlock();
    return out;
}

void ANS_FromText(uint8_t cat, const char *text)   /* 网页覆盖整类答案(空串=恢复内置) */
{
    const char *p = text, *nl;
    uint8_t n = 0;
    if (cat >= ANS_CAT_N) return;
    ans_lock();
    /* 按行拆分(校验: 每行 ≤ ANS_LINE_MAX-1, 总条数 ≤ ANS_TOTAL_MAX) */
    while (*p && n < ANS_TOTAL_MAX)
    {
        size_t len;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= ANS_LINE_MAX) break;     /* 超长行: 中止解析并保存已解析部分(该行及后续行丢弃) */
        if (len > 0)
        {
            while (len > 0 && p[len - 1] == '\r') len--;   /* 网页 textarea 常发 CRLF: 剥掉行尾 \r, 防显示异常 */
            if (len > 0)
            {
                memcpy(ans_nvs_buf[cat][n], p, len);
                ans_nvs_buf[cat][n][len] = '\0';
                ans_list[cat][n] = ans_nvs_buf[cat][n];
                n++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    ans_list_n[cat] = n;
    ans_list_save(cat);                     /* 空串: 只擦 NVS 键(恢复内置的语义) */
    if (n == 0)
    {
        ans_list_load(cat);                 /* 立即回填内置答案: 防"抽答案"静默失效(0 条时选该类按 OK 无反应)直到重启 */
    }
    ans_unlock();
}
