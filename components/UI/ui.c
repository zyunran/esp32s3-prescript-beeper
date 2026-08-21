/* UI 组件: 左侧指令图标 + 右侧滚动菜单(按键4/5/6)
 * 284×76 横屏, 像素坐标
 *   - 左侧 0..108: 状态栏(日期/网络/电量/时钟/天气); 指令图标居中 110..173(64×64)
 *   - 右侧 140..283: 滚动菜单, 3 个槽位, 中间槽为当前功能
 * 滚动模型(与需求一致): 按下"下"键, 整体内容向下移动一格(时间→中, 计时→下);
 *   环形循环, 3 项无限滑动。
 * 选中效果: 当前项左移一个汉字距离 + 主题色矩形线框(UI_CURSOR_COLOR, 带间距);
 *   取消选中时水平偏移缓动回退(可见动画)。
 * 动画: 全屏帧缓冲(双缓冲) + 原子批量刷新, 消除滑动黑屏/闪烁;
 *   项逐像素裁剪, 顶部/底部平滑滑出屏幕。
 * 本组件同时对外暴露帧缓冲绘制接口(UI_ScrClear/UI_ScrGlyph/UI_ScrBlit),
 * 供 INSTRUCTION 组件做全屏乱码破译显示。
 */
#include "ui.h"
#include "LCD.h"
#include "lcdchinese.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

/* ascii_1608 定义在 LCD 组件的 lcdfont.h(由 lcd.c 包含, 外部链接),
 * 此处只声明引用, 避免重复定义 */
extern const unsigned char ascii_1608[][16];

/* 文字像素宽(定义于通用菜单渲染节, 此处前置声明供 ScrText 等早段使用) */
static int16_t ui_text_width(const char *s);
static int16_t ui_menu_item_x(const char *s, int16_t xoff);
static uint8_t ui_sub_center;   /* 子菜单居中标志(0=右对齐 1=逐项居中 2=块左对齐以最长项居中), 定义于此供菜单渲染使用 */

/* 界面/动画可调参数定义在 ui.h 的"界面可调参数"节, 改那里即可 */
#define MENU_MID        (UI_MENU_Y1 + 8)   /* 选中行中心线(派生) */

/* ================= 左侧指令图标 =================
 * 64×64, 1bit 逐列取模:
 *   img[(行/8)*宽+列], bit0=该8行组最上一行, 1=点亮. */
static const uint8_t icon_cmd[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x80, 0x40, 0x40, 0xC0, 0x80, 0x08, 0x9C,
    0x26, 0x78, 0x20, 0x80, 0xC0, 0x40, 0xC0, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x80, 0x00, 0x01, 0x03, 0x22, 0xF2, 0x1C, 0x00, 0x99,
    0x07, 0xF4, 0x08, 0x0A, 0xF2, 0xA2, 0x02, 0x01, 0x01, 0x40, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0xE0, 0x30, 0x1C, 0x06, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x80, 0x2E, 0x00,
    0x00, 0x7F, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x0C, 0x18, 0x70,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC,
    0xC7, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x60, 0x28, 0x22, 0x26, 0x04,
    0x01, 0x11, 0x95, 0x15, 0x23, 0x7A, 0xB5, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xFE,
    0x01, 0xFF, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x1F, 0x78, 0xE1, 0x87, 0x1C, 0x30, 0x60, 0xC0, 0x80, 0x80, 0x01, 0x1E, 0x70, 0xC0, 0x00, 0x00,
    0x01, 0x03, 0x8C, 0xF1, 0x13, 0x08, 0x07, 0x00, 0x80, 0xC0, 0x40, 0x60, 0x18, 0x0C, 0xC7, 0xE0,
    0x3C, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x07, 0x06, 0x0C, 0x18, 0x30, 0x31, 0x21, 0x63, 0x62, 0x66, 0x46, 0xC6,
    0xC6, 0xC6, 0x46, 0x46, 0x62, 0x62, 0x63, 0x31, 0x31, 0x18, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* ================= 右侧菜单 =================
 * 标题与功能集中配置于 ui_menu_cfg(ui.h 声明), 改这里即可调整菜单项与其对应功能.
 * 文字须在全字库 lcdzh.c 内, 或会显示成方框 */
/* 自定义子菜单项(集中配置, 改这里即可改文字) */
static const char *cfg_net_items[] = {
    [UI_NET_CONNECT] = "连接网络",   /* 标签进入子菜单后由 UI_SubMenuSetItem 动态改"联网:开/关" */
    [UI_NET_AP]      = "开启配网",
    [UI_NET_WEATHER] = "查看天气",
    [UI_NET_IP]      = "显示IP",
    [UI_NET_EXIT]    = "退出",
};
static const char *cfg_ttl_items[] = {
    [UI_TTL_FUTURE] = "计时",
    [UI_TTL_PAST]   = "闹钟",
    [UI_TTL_EXIT]   = "退出",
};
static const char *cfg_loom_items[] = {
    [UI_LOOM_SPIN]   = "纺织时间",   /* 彩蛋: made in heaven 时间加速 */
    [UI_LOOM_MEMORY] = "纺织记忆",   /* 彩蛋: 全系统白框滤镜(唯一退出=再按一次) */
    [UI_LOOM_EXIT]   = "退出",
};
/* ================= 使用者列表(运行期可变) =================
 * 网页端可添加新使用者, NVS "ins2"/"ulist" 持久化(与指令库同模式);
 * 设备端子菜单(主菜单「使用者」)与网页下拉共用此列表. */
#define UI_USER_MAX        20
static char      ui_user_names[UI_USER_MAX][UI_USER_NAME_MAX];
static const char *ui_user_ptr[UI_USER_MAX + 1];   /* 指针数组(末项"退出") */
static uint8_t   ui_user_n = 0;
static const char *const ui_user_defaults[] = { "李箱", "里恩", "浮士德" };   /* 首次无 NVS 时的内置 */
#define UI_USER_DEFAULT_N (sizeof(ui_user_defaults) / sizeof(ui_user_defaults[0]))

static void ui_user_rebuild(void)
{
    uint8_t i;
    for (i = 0; i < ui_user_n; i++) ui_user_ptr[i] = ui_user_names[i];
    ui_user_ptr[ui_user_n] = "退出";
    ui_menu_cfg[UI_MENU_USER].items = (const char *const *)ui_user_ptr;
    ui_menu_cfg[UI_MENU_USER].item_count = ui_user_n + 1;
}

static void ui_user_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ins2", NVS_READWRITE, &h) == ESP_OK)
    {
        char buf[UI_USER_MAX * UI_USER_NAME_MAX] = {0};   /* 20×(23B+换行)+NUL 恰好装满: 用累计写防越界 */
        uint8_t i;
        size_t wp = 0;
        for (i = 0; i < ui_user_n && wp < sizeof(buf) - 1; i++)
        {
            wp += (size_t)snprintf(buf + wp, sizeof(buf) - wp, "%s%s",
                                   wp ? "\n" : "", ui_user_names[i]);
        }
        nvs_set_str(h, "ulist", buf);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 加载使用者列表(NVS 有则用, 无则内置默认); 在 UI_Init 前调用 */
void UI_UserInit(void)
{
    nvs_handle_t h;
    ui_user_n = 0;
    if (nvs_open("ins2", NVS_READONLY, &h) == ESP_OK)
    {
        char buf[UI_USER_MAX * UI_USER_NAME_MAX];
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "ulist", buf, &len) == ESP_OK && len > 1)
        {
            char *p = buf, *nl;
            while (*p && ui_user_n < UI_USER_MAX)
            {
                nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (*p)
                {
                    snprintf(ui_user_names[ui_user_n], sizeof(ui_user_names[0]), "%s", p);
                    ui_user_n++;
                }
                if (!nl) break;
                p = nl + 1;
            }
        }
        nvs_close(h);
    }
    if (ui_user_n == 0)   /* 无 NVS: 内置默认 */
    {
        uint8_t i;
        for (i = 0; i < UI_USER_DEFAULT_N && i < UI_USER_MAX; i++)
        {
            snprintf(ui_user_names[ui_user_n], sizeof(ui_user_names[0]), "%s", ui_user_defaults[i]);
            ui_user_n++;
        }
        ui_user_save();
    }
    ui_user_rebuild();
}

/* 添加使用者(去重), 返回 1=成功/已存在, 0=参数错/满 */
uint8_t UI_UserAdd(const char *name)
{
    uint8_t i;
    if (!name || name[0] == '\0') return 0;
    for (i = 0; i < ui_user_n; i++)
    {
        if (strcmp(ui_user_names[i], name) == 0) return 1;
    }
    if (ui_user_n >= UI_USER_MAX) return 0;
    snprintf(ui_user_names[ui_user_n], sizeof(ui_user_names[0]), "%s", name);
    ui_user_n++;
    ui_user_rebuild();
    ui_user_save();
    return 1;
}

/* 使用者列表(不含末项"退出"; 供网页端下拉选择, 与设备端子菜单一致) */
const char *const *UI_UserList(uint8_t *count)
{
    *count = ui_user_n;
    return (const char *const *)ui_user_ptr;
}

static char ui_user_title[UI_USER_NAME_MAX] = "使用者";   /* 主菜单「使用者」项动态标题 = 当前使用者名(UI_SetUserTitle 更新) */

ui_menu_cfg_t ui_menu_cfg[UI_MENU_COUNT] = {
    [UI_MENU_INS]     = { "神谕", UI_FN_INS,      0, NULL,              0 },
    [UI_MENU_ASK]     = { "询问", UI_FN_ASK,      0, NULL,              0 },
    [UI_MENU_GACHA]   = { "观测", UI_FN_GACHA,    0, NULL,              0 },
    [UI_MENU_TODO]    = { "待办", UI_FN_TODO,     0, NULL,              0 },
    [UI_MENU_USER]    = { ui_user_title, UI_FN_USER, 0, NULL, 0 },   /* items/item_count 由 UI_UserInit 运行时填 */
    [UI_MENU_SETTING] = { "设置", UI_FN_SETTING,  0, NULL,              0 },
    [UI_MENU_NET]     = { "联网", UI_FN_NET,      0, cfg_net_items,     UI_NET_EXIT + 1 },
    [UI_MENU_LOOM]    = { "织机", UI_FN_LOOM,     0, cfg_loom_items,    UI_LOOM_EXIT + 1 },
    [UI_MENU_TTL]     = { "TTL协议", UI_FN_TTL,   0, cfg_ttl_items,     UI_TTL_EXIT + 1 },
};

/* 主菜单「使用者」项标题 = 当前使用者名(动态缓冲, 下次渲染生效) */
void UI_SetUserTitle(const char *name)
{
    strncpy(ui_user_title, name, sizeof(ui_user_title) - 1);
    ui_user_title[sizeof(ui_user_title) - 1] = '\0';
}

static const char *menu_items[UI_MENU_COUNT];   /* 渲染用标题指针(由配置同步) */
#define MENU_COUNT UI_MENU_COUNT

static void ui_menu_items_sync(void)
{
    uint8_t i;
    for (i = 0; i < UI_MENU_COUNT; i++)
    {
        menu_items[i] = ui_menu_cfg[i].title;
    }
}

static uint8_t menu_cur = 1;                /* 当前选中项(中间槽) = 询问 */
static uint8_t ui_cursor_style = UI_CURSOR_DEFAULT;

/* ================= 主页面顶部状态栏(左上角): 月份-日期 / 网络 / 电量 =================
 * 图标居中(110..173), 左侧 0..108 空白, 供状态栏+时钟+天气显示
 * 时钟: 左区中间略靠左(8×16); 天气: 时钟下方 */
#define UI_DATE_X        2                  /* 月份-日期 x(状态栏最左, 5×7 紧凑字体) */
#define UI_DATE_Y        2                  /* 月份-日期 y */
#define UI_WEEK_X        34                 /* 星期 x(日期右侧, 同 5×7 字号) */
#define UI_WEEK_Y        2                  /* 星期 y(与日期同行) */
#define UI_WIFI_X        56                 /* 网络图标 x(星期右侧顺延) */
#define UI_WIFI_Y        3                  /* 网络图标 y */
#define UI_BAT_X         70                 /* 电量图标 x(网络右侧) */
#define UI_BAT_Y         1                  /* 电量图标 y */
#define UI_TIME_X        32                 /* 时钟 x(左区中间略靠左) */
#define UI_TIME_Y        30                 /* 时钟 y(垂直居中) */
#define UI_TIME_BOTTOM   48                 /* 时间区下缘(日期+时钟), 天气在其下 */
#define UI_TIME_CLEAR_W  110                /* 清屏宽度(直到图标左缘) */
#define UI_WEATHER_X     2                  /* 天气 x(左对齐, 3字天气词+温度不溢出图标) */
#define UI_WEATHER_Y     52                 /* 天气 y(时钟下方) */
#define UI_WEATHER_H     20                 /* 天气区高度(一行16px+边距) */
static char   ui_time_date[8];              /* "MM-DD" */
static char   ui_time_week[8];              /* "Mon".."Sun"(未同步 "--") */
static char   ui_time_clk[12];              /* "HH:MM:SS" */
static uint8_t ui_time_valid = 0;
static uint8_t ui_wifi_on = 0xFF;           /* WiFi 图标状态: 0/1=已设, 0xFF=未初始化 */
static uint8_t ui_bat_pct = 0xFF;           /* 电量: 0-100=已设, 0xFF=未初始化 */
static char   ui_weather[24];               /* "晴 36/24"(可能被截断的原始输入) */
static char   ui_weather_src[sizeof(ui_weather)];  /* 最近一次输入原样(供变化判定, 见 UI_WeatherSet) */
static uint8_t ui_weather_valid = 0;

/* ================= 帧缓冲(双缓冲) =================
 * 所有绘制先进 RAM 缓冲, 再一次性刷屏, 保证动画原子更新无闪烁. */
static uint8_t ui_fb[LCD_WIDTH * LCD_HEIGHT * 2];

/* 白框滤镜(纺织记忆彩蛋): 1 时字符/图标整体白框化.
 * 全局持续, 所有界面生效; 再进织机→纺织记忆 关闭即恢复.
 * 规则: 每个字符串(菜单项等)随机保留其中【1个字符】真字作提示, 其余白框;
 * 该保留位由字符串内容确定 → 同串永远同位置, 滚动/刷新不乱跳.
 * 图标/日期星期小字/缩放字: 整体白框(不保留). */
uint8_t ui_box_mode = 0;

/* 1=当前绘制字符保留真字(由 fb_draw_string 按"随机保留位"设置); 0=白框 */
static uint8_t ui_box_keep_now = 0;

/* 由字符串内容确定"保留第几个字符"(随机位置但稳定): 内容不变→位置不变 */
static uint8_t ui_box_keep_idx(const char *s)
{
    uint32_t h = 2166136261u;
    uint8_t n = 0;
    while (*s)
    {
        if ((*s & 0xC0) != 0x80) n++;           /* 统计字符数(跳过UTF-8延续字节) */
        h = (h ^ (uint8_t)*s) * 16777619u;
        s++;
    }
    if (n <= 1) return 0;
    return (uint8_t)(h % n);
}

/* 进入白框模式时初始化 */
void UI_BoxModeSet(uint8_t on)
{
    ui_box_mode = on ? 1 : 0;
    ui_box_keep_now = 0;
}

uint8_t UI_BoxModeGet(void)
{
    return ui_box_mode;
}

static void fb_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    uint32_t i;
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
    {
        return;
    }
    i = ((uint32_t)y * LCD_WIDTH + x) * 2;
    ui_fb[i]     = (color >> 8) & 0xFF;
    ui_fb[i + 1] =  color       & 0xFF;
}

static void fb_clear(uint16_t color)
{
    uint32_t n = (uint32_t)LCD_WIDTH * LCD_HEIGHT;
    uint32_t i;
    uint8_t hi = (color >> 8) & 0xFF, lo = color & 0xFF;
    for (i = 0; i < n; i++) { ui_fb[i * 2] = hi; ui_fb[i * 2 + 1] = lo; }
}

static void fb_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    int16_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            fb_pixel(x + i, y + j, color);
}

/* 画一个字模单元到缓冲, 裁剪到 [cx0..cx1]×[cy0..cy1](须已与屏幕求交).
 * w=16 汉字(列式, 每列2字节, 低位为上行); w=8 ASCII(行式, 低位在前). */
static void fb_draw_cell(uint16_t x, int16_t y, const uint8_t *data, uint8_t w, uint8_t h,
                         uint16_t fc, uint16_t bc,
                         int16_t cx0, int16_t cy0, int16_t cx1, int16_t cy1)
{
    uint8_t r, c, byte;

    /* 白框滤镜(纺织记忆): 非保留位字符 -> 改画空心框(边框用原色, 内部主题背景) */
    if (ui_box_mode && !ui_box_keep_now)
    {
        int16_t px, py;
        for (py = 0; py < h; py++)
        {
            for (px = 0; px < w; px++)
            {
                int16_t dx = x + px, dy = y + py;
                if (dx < cx0 || dx > cx1 || dy < cy0 || dy > cy1) continue;
                if (px == 0 || px == w - 1 || py == 0 || py == h - 1)
                    fb_pixel((uint16_t)dx, (uint16_t)dy, fc);
                else
                    fb_pixel((uint16_t)dx, (uint16_t)dy, UI_COLOR_BG);
            }
        }
        return;
    }

    for (r = 0; r < h; r++)
    {
        for (c = 0; c < w; c++)
        {
            int16_t px = x + c, py = y + r;
            if (px < cx0 || px > cx1 || py < cy0 || py > cy1)
            {
                continue;
            }
            if (w == 16)
            {
                byte = (r < 8) ? data[c] : data[c + 16];
                fb_pixel(px, py, (byte & (0x01 << (r & 7))) ? fc : bc);
            }
            else
            {
                byte = data[r];
                fb_pixel(px, py, (byte & (0x01 << c)) ? fc : bc);
            }
        }
    }
}

/* 画单个 UTF-8 字符到缓冲(中文 16×16 或 ASCII 8×16), 返回字符宽度 */
static uint16_t fb_draw_glyph(uint16_t x, int16_t y, const char *ch,
                              uint16_t fc, uint16_t bc,
                              int16_t cx0, int16_t cy0, int16_t cx1, int16_t cy1)
{
    if (ch[0] & 0x80)
    {
        fb_draw_cell(x, y, lcd_font_get(ch), 16, 16, fc, bc, cx0, cy0, cx1, cy1);
        return 16;
    }
    if (ch[0] >= ' ' && ch[0] <= '~')
    {
        fb_draw_cell(x, y, ascii_1608[ch[0] - ' '], 8, 16, fc, bc, cx0, cy0, cx1, cy1);
    }
    return 8;
}

/* 按目标尺寸缩放画字模(最近邻采样): 支持 16/24/32px(1x/1.5x/2x).
 * 源字模 16×16 汉字或 8×16 ASCII, 目标尺寸任意, 像素按比例取源. */
static void fb_draw_cell_f(uint16_t x, int16_t y, const uint8_t *data, uint8_t w_src, uint8_t h_src,
                           uint8_t w_dst, uint8_t h_dst, uint16_t fc, uint16_t bc,
                           int16_t cx0, int16_t cy0, int16_t cx1, int16_t cy1)
{
    uint16_t px, py;

    /* 白框滤镜(纺织记忆): 缩放字格整体改画空心框(不保留提示) */
    if (ui_box_mode)
    {
        for (py = 0; py < h_dst; py++)
        {
            for (px = 0; px < w_dst; px++)
            {
                int16_t dx = x + (int16_t)px, dy = y + (int16_t)py;
                if (dx < cx0 || dx > cx1 || dy < cy0 || dy > cy1) continue;
                if (px == 0 || px == w_dst - 1 || py == 0 || py == h_dst - 1)
                    fb_pixel((uint16_t)dx, (uint16_t)dy, fc);
                else
                    fb_pixel((uint16_t)dx, (uint16_t)dy, UI_COLOR_BG);
            }
        }
        return;
    }

    for (py = 0; py < h_dst; py++)
    {
        for (px = 0; px < w_dst; px++)
        {
            uint8_t sx = (uint8_t)((uint32_t)px * w_src / w_dst);
            uint8_t sy = (uint8_t)((uint32_t)py * h_src / h_dst);
            uint8_t byte;
            uint16_t color;
            int16_t dx = x + (int16_t)px, dy = y + (int16_t)py;
            if (dx < cx0 || dx > cx1 || dy < cy0 || dy > cy1)
            {
                continue;
            }
            if (w_src == 16)
            {
                byte = (sy < 8) ? data[sx] : data[sx + 16];
                color = (byte & (0x01 << (sy & 7))) ? fc : bc;
            }
            else
            {
                byte = data[sy];
                color = (byte & (0x01 << sx)) ? fc : bc;
            }
            fb_pixel((uint16_t)dx, (uint16_t)dy, color);
        }
    }
}

/* 5×7 紧凑小字体(列式取模: 每列 1 字节, bit r=行 r, bit0=顶行).
 * 含数字 0-9 + 短横 + 大小写字母(星期英文缩写用), 与日期同字号. */
static const uint8_t font5x7_date[][5] = {
    {0x3E, 0x41, 0x41, 0x41, 0x3E},   /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00},   /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46},   /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31},   /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10},   /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39},   /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30},   /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03},   /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36},   /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E},   /* '9' */
    {0x08, 0x08, 0x08, 0x08, 0x08},   /* '-' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E},   /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36},   /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22},   /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C},   /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41},   /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01},   /* 'F' */
    {0x3E, 0x41, 0x41, 0x51, 0x72},   /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F},   /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00},   /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01},   /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41},   /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40},   /* 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},   /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F},   /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E},   /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06},   /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E},   /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46},   /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31},   /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01},   /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F},   /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F},   /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F},   /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63},   /* 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07},   /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43},   /* 'Z' */
    {0x20, 0x54, 0x54, 0x54, 0x78},   /* 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38},   /* 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20},   /* 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F},   /* 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18},   /* 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02},   /* 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E},   /* 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78},   /* 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00},   /* 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00},   /* 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00},   /* 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00},   /* 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78},   /* 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78},   /* 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38},   /* 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08},   /* 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C},   /* 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08},   /* 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20},   /* 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20},   /* 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C},   /* 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C},   /* 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C},   /* 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44},   /* 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C},   /* 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44},   /* 'z' */
};

/* 画 5×7 小字符到缓冲(bit r=行 r, bit0=顶行); 数字/短横/大小写字母, 其余留白 */
static void fb_draw_5x7(uint16_t x, int16_t y, char ch, uint16_t fc, uint16_t bc)
{
    const uint8_t *g;
    uint8_t c, r;
    if (ch >= '0' && ch <= '9')
    {
        g = font5x7_date[ch - '0'];
    }
    else if (ch == '-')
    {
        g = font5x7_date[10];
    }
    else if (ch >= 'A' && ch <= 'Z')
    {
        g = font5x7_date[11 + ch - 'A'];
    }
    else if (ch >= 'a' && ch <= 'z')
    {
        g = font5x7_date[37 + ch - 'a'];
    }
    else
    {
        return;
    }
    /* 白框滤镜(纺织记忆): 5×7 小字格整体改画空心框(不保留提示) */
    if (ui_box_mode)
    {
        for (c = 0; c < 5; c++)
        {
            for (r = 0; r < 7; r++)
            {
                uint16_t px = x + c, py = y + r;
                if (c == 0 || c == 4 || r == 0 || r == 6)
                    fb_pixel(px, py, fc);
                else
                    fb_pixel(px, py, UI_COLOR_BG);
            }
        }
        return;
    }
    for (c = 0; c < 5; c++)
    {
        for (r = 0; r < 7; r++)
        {
            fb_pixel(x + c, y + r, (g[c] & (1 << r)) ? fc : bc);
        }
    }
}

/* 画单色图到缓冲(1bit 逐列取模:
 *   img[(行/8)*宽+列], bit0=该8行组最上一行, 1=点亮). */
static void fb_draw_image_mono(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               const uint8_t *img, uint16_t fc, uint16_t bc)
{
    uint16_t i, j;

    /* 白框滤镜(纺织记忆): 图标整体改画空心框(边框用图标本色 fc, 内部主题背景; 不参与单字提示) */
    if (ui_box_mode)
    {
        for (i = 0; i < h; i++)
        {
            for (j = 0; j < w; j++)
            {
                if (j == 0 || j == w - 1 || i == 0 || i == h - 1)
                    fb_pixel(x + j, y + i, fc);
                else
                    fb_pixel(x + j, y + i, UI_COLOR_BG);
            }
        }
        return;
    }

    for (i = 0; i < h; i++)
    {
        for (j = 0; j < w; j++)
        {
            if (img[(i / 8) * w + j] & (0x01 << (i % 8)))
            {
                fb_pixel(x + j, y + i, fc);
            }
            else
            {
                fb_pixel(x + j, y + i, bc);
            }
        }
    }
}

/* 画 UTF-8 字符串(ASCII+中文混排)到缓冲, 支持裁剪 */
static void fb_draw_string(uint16_t x, int16_t y, const char *string,
                           uint16_t fc, uint16_t bc,
                           int16_t cx0, int16_t cy0, int16_t cx1, int16_t cy1)
{
    uint16_t i = 0, xoff = 0;
    uint8_t keep_idx = 0, idx = 0;
    char single[5];

    if (ui_box_mode) keep_idx = ui_box_keep_idx(string);   /* 本串随机保留位 */

    while (string[i] != '\0')
    {
        if ((string[i] & 0x80) == 0x00)         /* 1 字节 ASCII */
        {
            single[0] = string[i++];
            single[1] = '\0';
        }
        else if ((string[i] & 0xE0) == 0xC0)    /* 2 字节 */
        {
            single[0] = string[i++];
            if (string[i] == '\0')
            {
                break;
            }
            single[1] = string[i++];
            single[2] = '\0';
        }
        else if ((string[i] & 0xF0) == 0xE0)    /* 3 字节: 汉字 */
        {
            single[0] = string[i++];
            if (string[i] == '\0')
            {
                break;
            }
            single[1] = string[i++];
            if (string[i] == '\0')
            {
                break;
            }
            single[2] = string[i++];
            single[3] = '\0';
        }
        else if ((string[i] & 0xF0) == 0xF0)    /* 4 字节序列: 字库无此字形, 整序列按 1 字宽画方框占位(与 ui_text_width 对齐) */
        {
            uint8_t k;
            for (k = 1; k < 4 && string[i + k] != '\0'; k++) { /* 吞掉续字节(断串即止, 不过 NUL) */ }
            single[0] = string[i];
            i += k;
            single[1] = '\0';
        }
        else { i++; continue; }

        ui_box_keep_now = (ui_box_mode && idx == keep_idx);  /* 保留位字符画真字 */
        idx++;
        xoff += fb_draw_glyph(x + xoff, y, single, fc, bc, cx0, cy0, cx1, cy1);
    }
}

/* 整屏一次性刷到 LCD(分块发送, 避免单次大事务) */
static void fb_blit(void)
{
    uint32_t len = (uint32_t)LCD_WIDTH * LCD_HEIGHT * 2;
    uint32_t i = 0;
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    while (i < len)
    {
        uint32_t n = (len - i > 4096) ? 4096 : (len - i);
        lcd_write_datan(&ui_fb[i], (uint16_t)n);
        i += n;
    }
}

/* ================= 屏幕绘制接口(供 INSTRUCTION 等组件) ================= */
void UI_ScrClear(uint16_t color)
{
    fb_clear(color);
}

uint16_t UI_ScrGlyph(uint16_t x, int16_t y, const char *ch, uint16_t fc, uint16_t bc)
{
    return fb_draw_glyph(x, y, ch, fc, bc, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
}

/* 按指定字号(16/24/32px)缩放画字符: 中文宽=高, ASCII 宽=高/2; 返回字符宽度 */
uint16_t UI_ScrGlyphF(uint16_t x, int16_t y, const char *ch, uint8_t font_h, uint16_t fc, uint16_t bc)
{
    if (ch[0] & 0x80)
    {
        fb_draw_cell_f(x, y, lcd_font_get(ch), 16, 16, font_h, font_h, fc, bc,
                       0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
        return font_h;
    }
    if (ch[0] >= ' ' && ch[0] <= '~')
    {
        fb_draw_cell_f(x, y, ascii_1608[ch[0] - ' '], 8, 16, (uint8_t)(font_h / 2), font_h, fc, bc,
                       0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    }
    return (uint16_t)(font_h / 2);
}

void UI_ScrRect(uint16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    fb_fill_rect(x, y, w, h, color);
}

/* 画 RGB565 图片到缓冲(大端字节序: 每像素2字节, 高位在前, 同帧缓冲).
 * 纯黑(0x0000)像素=透明不写(与 UI 背景融合, 消除图片黑边色差);
 * gray=1 时整图转灰度(硬币反面用, 非黑像素按亮度转灰). */
void UI_ScrImage(uint16_t x, int16_t y, uint16_t w, uint16_t h,
                 const uint8_t *img, uint8_t gray)
{
    uint16_t i, j;
    for (i = 0; i < h; i++)
    {
        for (j = 0; j < w; j++)
        {
            uint32_t k = ((uint32_t)i * w + j) * 2;
            uint16_t c = (uint16_t)((img[k] << 8) | img[k + 1]);
            if (c == 0x0000)
            {
                continue;              /* 纯黑背景 = 透明 */
            }
            if (gray)
            {
                uint8_t r = (uint8_t)((c >> 11) & 0x1F);
                uint8_t g = (uint8_t)((c >> 5) & 0x3F);
                uint8_t b = (uint8_t)(c & 0x1F);
                uint8_t yv = (uint8_t)(((uint32_t)r * 3 + (uint32_t)g * 6 + (uint32_t)b) >> 4);
                c = (uint16_t)((yv << 11) | (yv << 6) | yv);
            }
            fb_pixel(x + j, y + i, c);
        }
    }
}

uint16_t UI_ScrText(uint16_t x, int16_t y, const char *s, uint16_t fc, uint16_t bc)
{
    fb_draw_string(x, y, s, fc, bc, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    return (uint16_t)ui_text_width(s);
}

uint16_t UI_ScrTextCenter(int16_t y, const char *s, uint16_t fc, uint16_t bc)
{
    int16_t x = (LCD_WIDTH - ui_text_width(s)) / 2;   /* 超宽串: 钳到 0 左对齐, 防负值回绕成 655xx 从屏外画起 */
    if (x < 0) x = 0;
    fb_draw_string((uint16_t)x, y, s, fc, bc, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    return (uint16_t)ui_text_width(s);
}

void UI_ScrBlit(void)
{
    fb_blit();
}

/* ================= 左侧时间显示(顶部状态栏: 月份-日期 / 网络 / 电量) ================= */

/* 电量小图标: 外框+右侧端子+按电量填充; pct=255 只画空框(未接电池) */
static void ui_draw_battery(uint16_t bx, uint8_t by, uint8_t pct)
{
    uint16_t c = UI_COLOR_FRAME;                       /* 白框滤镜: 外框保持原色(不刺眼) */
    uint8_t i, fill;
    fb_fill_rect(bx + 14, by + 2, 2, 4, c);            /* 正极端子 */
    fb_fill_rect(bx, by, 14, 1, c);                    /* 上边 */
    fb_fill_rect(bx, by + 7, 14, 1, c);                /* 下边 */
    fb_fill_rect(bx, by, 1, 8, c);                     /* 左边 */
    fb_fill_rect(bx + 13, by, 1, 8, c);                /* 右边 */
    if (pct <= 100 && !ui_box_mode)                    /* 白框滤镜: 不画内部填充 */
    {
        fill = (uint8_t)(11u * pct / 100);             /* 内腔 11px 填充 */
        for (i = 0; i < fill; i++)
        {
            fb_fill_rect(bx + 1 + i, by + 1, 1, 6, c);
        }
    }
}

/* 把时间文字画进帧缓冲(不刷屏), 供全界面重绘时复用 */
static void ui_time_draw_text(void)
{
    const char *p;
    uint16_t xoff = 0;
    fb_fill_rect(0, 0, UI_TIME_CLEAR_W, UI_TIME_BOTTOM, UI_COLOR_BG);   /* 只清时间区, 不动天气 */
    /* 月份-日期(5×7 紧凑字体, 字距1px) */
    for (p = ui_time_date; *p; p++)
    {
        fb_draw_5x7(UI_DATE_X + xoff, UI_DATE_Y, *p, UI_COLOR_DATE, UI_COLOR_BG);
        xoff += 6;
    }
    /* 网络图标(日期右侧): 绿=已连, 灰=未连 */
    if (ui_wifi_on != 0xFF)
    {
        uint16_t c = ui_wifi_on ? UI_COLOR_FRAME : 0x3186;
        if (ui_box_mode)   /* 白框滤镜: WiFi 图标改画空心框(边框用原图标色) */
        {
            uint8_t u, v;
            for (v = 0; v < 5; v++)
                for (u = 0; u < 8; u++)
                    if (u == 0 || u == 7 || v == 0 || v == 4)
                        fb_pixel(UI_WIFI_X + u, UI_WIFI_Y + v, c);
                    else
                        fb_pixel(UI_WIFI_X + u, UI_WIFI_Y + v, UI_COLOR_BG);
        }
        else
        {
            fb_fill_rect(UI_WIFI_X,     UI_WIFI_Y + 4, 1, 1, c);
            fb_fill_rect(UI_WIFI_X + 2, UI_WIFI_Y + 3, 1, 2, c);
            fb_fill_rect(UI_WIFI_X + 4, UI_WIFI_Y + 2, 1, 3, c);
            fb_fill_rect(UI_WIFI_X + 6, UI_WIFI_Y + 1, 1, 4, c);
        }
    }
    /* 电量图标(网络右侧): 填充=电量, 空框=未接电池 */
    ui_draw_battery(UI_BAT_X, UI_BAT_Y, ui_bat_pct);
    /* 星期(日期右侧, 5×7 同字号, 字距1px; "Wed" 等英文缩写) */
    xoff = 0;
    for (p = ui_time_week; *p; p++)
    {
        fb_draw_5x7(UI_WEEK_X + xoff, UI_WEEK_Y, *p, UI_COLOR_DATE, UI_COLOR_BG);
        xoff += 6;
    }
    /* 左区中间时钟(8×16) */
    fb_draw_string(UI_TIME_X, UI_TIME_Y, ui_time_clk, UI_COLOR_TIME, UI_COLOR_BG,
                   0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
}

/* 把天气串按像素宽截断(整字符, 不裁半个字)到 dst; 不改写 ui_weather 存储(供下次变化判定) */
static void ui_weather_clip_to(char *dst, size_t cap, const char *src)
{
    int16_t w = 0;
    size_t o = 0;
    while (*src && o < cap - 1)
    {
        size_t len = 1, i;
        int16_t cw = 8;
        unsigned char c = (unsigned char)*src;
        if (c & 0x80)
        {
            len = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 1;
            cw = 16;
            for (i = 1; i < len; i++)   /* 残缺序列(异常数据)按单字节处理, 绝不越过 NUL 读 */
            {
                if (src[i] == '\0' || (src[i] & 0xC0) != 0x80) { len = 1; cw = 8; break; }
            }
        }
        if (UI_WEATHER_X + w + cw > UI_TIME_CLEAR_W - 2) break;
        memcpy(dst + o, src, len);
        o += len;
        w += cw;
        src += len;
    }
    dst[o] = '\0';
}

static void ui_weather_draw_text(void)
{
    char clip[sizeof(ui_weather)];
    ui_weather_clip_to(clip, sizeof(clip), ui_weather);   /* 截断过长串(3字天气词+湿度会超宽) */
    fb_fill_rect(0, UI_WEATHER_Y, UI_TIME_CLEAR_W, UI_WEATHER_H, UI_COLOR_BG);
    fb_draw_string(UI_WEATHER_X, UI_WEATHER_Y, clip, UI_COLOR_TIME, UI_COLOR_BG,
                   0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
}

/* 更新左侧时间显示; 仅内容变化才重绘(未变化直接返回) */
void UI_TimeSet(const char *date, const char *time, const char *week)
{
    if (ui_time_valid &&
        strcmp(ui_time_date, date) == 0 &&
        strcmp(ui_time_clk, time) == 0 &&
        strcmp(ui_time_week, week) == 0)
    {
        return;
    }
    strncpy(ui_time_date, date, sizeof(ui_time_date) - 1);
    ui_time_date[sizeof(ui_time_date) - 1] = '\0';
    strncpy(ui_time_week, week, sizeof(ui_time_week) - 1);
    ui_time_week[sizeof(ui_time_week) - 1] = '\0';
    strncpy(ui_time_clk, time, sizeof(ui_time_clk) - 1);
    ui_time_clk[sizeof(ui_time_clk) - 1] = '\0';
    ui_time_valid = 1;
    ui_time_draw_text();
    fb_blit();
}

/* 更新左侧 WiFi 状态图标(绿=已连/灰=未连); 状态变化才重绘 */
void UI_WifiSet(uint8_t on)
{
    if (ui_wifi_on == on)
    {
        return;
    }
    ui_wifi_on = on ? 1 : 0;
    ui_time_draw_text();
    fb_blit();
}

/* 更新顶部电量图标(0-100 填充; 255=空框); 数值变化才重绘 */
void UI_BatterySet(uint8_t pct)
{
    if (ui_bat_pct == pct)
    {
        return;
    }
    ui_bat_pct = pct;
    ui_time_draw_text();
    fb_blit();
}

/* 更新左侧天气显示(时钟下方); 内容变化才重绘; str=NULL 表示清除。
 * 注意: 输入串可能超过显示缓冲(被截断), 变化判定用"与上次完整输入比较",
 * 而不是和截断后的存储比较 —— 否则超长串每次调用都判"不同"而每帧全屏重绘刷屏 */
void UI_WeatherSet(const char *str)
{
    if (!str)
    {
        if (ui_weather_valid)
        {
            ui_weather_valid = 0;
            fb_fill_rect(0, UI_WEATHER_Y, UI_TIME_CLEAR_W, UI_WEATHER_H, UI_COLOR_BG);
            fb_blit();
        }
        return;
    }
    if (ui_weather_valid && strncmp(ui_weather_src, str, sizeof(ui_weather_src) - 1) == 0)
    {
        return;
    }
    strncpy(ui_weather_src, str, sizeof(ui_weather_src) - 1);
    ui_weather_src[sizeof(ui_weather_src) - 1] = '\0';
    strncpy(ui_weather, str, sizeof(ui_weather) - 1);
    ui_weather[sizeof(ui_weather) - 1] = '\0';
    ui_weather_valid = 1;
    ui_weather_draw_text();
    fb_blit();
}

/* ================= 通用菜单渲染(主界面与子菜单共用) ================= */

/* 某 y 处的项是否跨过选中线(即当前功能) */
static int ui_is_current(int16_t y)
{
    return (y <= MENU_MID && MENU_MID <= y + 15);
}

/* 文字像素宽(中文16 / ASCII8); 兼容 2/3 字节 UTF-8, 截断/孤立首字节也不回跳出缓冲 */
static int16_t ui_text_width(const char *s)
{
    int16_t w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        if (*p & 0x80)
        {
            uint8_t len = 1;
            if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) len = 2;
            else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80
                     && (p[2] & 0xC0) == 0x80) len = 3;
            else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80
                     && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) len = 4;   /* 4字节: 整序列按 1 字宽 */
            w += 16;
            p += len;
        }
        else
        {
            w += 8;
            p += 1;
        }
    }
    return w;
}

/* 屏幕黑底画一项(越界裁剪; x = 基准 + 该水平偏移) */
static void ui_draw_item(const char *s, int16_t x, int16_t y)
{
    fb_draw_string(x, y, s, UI_COLOR_MENU, UI_COLOR_BG, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
}

/* 当前项光标(白线/白块/角框, 颜色统一 UI_CURSOR_COLOR; 与文字留 UI_MENU_FRAME_GAP 间距) */
static void ui_draw_frame(const char *s, int16_t x, int16_t y)
{
    int16_t w = ui_text_width(s);
    int16_t m = UI_MENU_FRAME_GAP;

    if (ui_cursor_style == UI_CURSOR_BLOCK)      /* 白块: 反白填充后重画文字 */
    {
        fb_fill_rect(x - m, y - m, w + 2 * m, 16 + 2 * m, UI_CURSOR_COLOR);
        fb_draw_string(x, y, s, UI_COLOR_BG, UI_CURSOR_COLOR,
                       0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
        return;
    }
    if (ui_cursor_style == UI_CURSOR_CORNER)     /* 角框: 四角 L 形括号 */
    {
        int16_t x0 = x - m, y0 = y - m;
        int16_t x1 = x + w + m - 1, y1 = y + 15 + m;
        int16_t c = 3;
        fb_fill_rect(x0, y0, c, 1, UI_CURSOR_COLOR);          /* 左上 */
        fb_fill_rect(x0, y0, 1, c, UI_CURSOR_COLOR);
        fb_fill_rect(x1 - c + 1, y0, c, 1, UI_CURSOR_COLOR);  /* 右上 */
        fb_fill_rect(x1, y0, 1, c, UI_CURSOR_COLOR);
        fb_fill_rect(x0, y1, c, 1, UI_CURSOR_COLOR);          /* 左下 */
        fb_fill_rect(x0, y1 - c + 1, 1, c, UI_CURSOR_COLOR);
        fb_fill_rect(x1 - c + 1, y1, c, 1, UI_CURSOR_COLOR);  /* 右下 */
        fb_fill_rect(x1, y1 - c + 1, 1, c, UI_CURSOR_COLOR);
        return;
    }

    fb_fill_rect(x - m, y - m, w + 2 * m, 1, UI_CURSOR_COLOR);          /* 上边 */
    fb_fill_rect(x - m, y + 15 + m, w + 2 * m, 1, UI_CURSOR_COLOR);     /* 下边 */
    fb_fill_rect(x - m, y - m, 1, 16 + 2 * m, UI_CURSOR_COLOR);         /* 左边 */
    fb_fill_rect(x + w + m - 1, y - m, 1, 16 + 2 * m, UI_CURSOR_COLOR); /* 右边 */
}

/* 公共光标绘制: 按当前光标样式(白线/白块/角框)框住 (x,y) 处的文字 s.
 * 写同一帧缓冲(与 UI_Scr* 共用), 供 ALARM 等自定义界面复用, 保证全局光标风格一致.
 * 白块样式会反白重画文字(文字本身由调用方已画, 此处重画成反白), 与主界面行为一致. */
void UI_DrawCursor(const char *s, int16_t x, int16_t y)
{
    ui_draw_frame(s, x, y);
}

/* 通用菜单静止渲染(3槽视口 + 选中白框 + 水平偏移)
 *   items/nitems/cur/xoff: 菜单上下文
 *   ease: 1=偏移缓动(滚动后), 0=直接到位
 * 返回 1 = 仍在缓动 */
static uint8_t ui_menu_draw(const char *const *items, uint8_t nitems,
                            uint8_t cur, int16_t *xoff, uint8_t ease)
{
    uint8_t idxs[3], n, moving = 0;
    int16_t ys[3];

    for (n = 0; n < 3; n++)
    {
        idxs[n] = (cur + n + nitems - 1) % nitems;
        ys[n]   = UI_MENU_Y0 + n * UI_MENU_ROW_H;
    }

    if (ease)
    {
        /* 各水平偏移向目标缓动(选中→左移, 取消→回退) */
        for (n = 0; n < nitems; n++)
        {
            int16_t target = 0, k;
            for (k = 0; k < 3; k++)
            {
                if (idxs[k] == n && ui_is_current(ys[k]))
                {
                    target = -UI_MENU_SHIFT;
                    break;
                }
            }
            if (xoff[n] < target) { xoff[n] += UI_EASE_STEP; if (xoff[n] > target) xoff[n] = target; moving = 1; }
            else if (xoff[n] > target) { xoff[n] -= UI_EASE_STEP; if (xoff[n] < target) xoff[n] = target; moving = 1; }
        }
    }
    else
    {
        for (n = 0; n < nitems; n++) xoff[n] = (n == cur) ? -UI_MENU_SHIFT : 0;
    }

    /* 清右半区(居中时清全屏) + 画 3 槽 + 当前项白线框 */
    if (ui_sub_center)
        fb_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, UI_COLOR_BG);
    else
        fb_fill_rect(UI_MENU_CLEAR_X, 0, LCD_WIDTH - UI_MENU_CLEAR_X, LCD_HEIGHT, UI_COLOR_BG);
    for (n = 0; n < 3; n++) ui_draw_item(items[idxs[n]], ui_menu_item_x(items[idxs[n]], xoff[idxs[n]]), ys[n]);
    for (n = 0; n < 3; n++)
    {
        if (ui_is_current(ys[n]))
        {
            ui_draw_frame(items[idxs[n]], ui_menu_item_x(items[idxs[n]], xoff[idxs[n]]), ys[n]);
            break;
        }
    }
    fb_blit();   /* 静止渲染也要刷屏: 否则退出子菜单/初始化后屏幕停在旧帧 */
    return moving;
}

/* 垂直滑动动画帧(水平偏移冻结, 顶部/底部项滑出屏) */
static void ui_menu_scroll_frame(const char *const *items, uint8_t nitems,
                                 uint8_t cur, int8_t dir, int16_t off, int16_t *xoff)
{
    uint8_t idxs[4], n, iin;
    int16_t ys[4], yin;

    for (n = 0; n < 3; n++)
    {
        idxs[n] = (cur + n + nitems - 1) % nitems;
        ys[n]   = UI_MENU_Y0 + n * UI_MENU_ROW_H - dir * off;
    }
    if (dir > 0) { yin = UI_MENU_Y2 + UI_MENU_ROW_H - off; iin = (cur + 2) % nitems; }
    else         { yin = UI_MENU_Y0 - UI_MENU_ROW_H + off; iin = (cur + nitems - 2) % nitems; }
    idxs[3] = iin; ys[3] = yin;

    if (ui_sub_center)
        fb_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, UI_COLOR_BG);
    else
        fb_fill_rect(UI_MENU_CLEAR_X, 0, LCD_WIDTH - UI_MENU_CLEAR_X, LCD_HEIGHT, UI_COLOR_BG);
    for (n = 0; n < 4; n++) ui_draw_item(items[idxs[n]], ui_menu_item_x(items[idxs[n]], xoff[idxs[n]]), ys[n]);
    for (n = 0; n < 4; n++)
    {
        if (ui_is_current(ys[n]))
        {
            ui_draw_frame(items[idxs[n]], ui_menu_item_x(items[idxs[n]], xoff[idxs[n]]), ys[n]);
            break;
        }
    }
    fb_blit();
}

/* 通用滚动(主界面与子菜单共用): 垂直滑动动画 + 更新选中 + 水平缓动到位 */
static void ui_scroll_common(const char *const *items, uint8_t nitems,
                             uint8_t *cur, int16_t *xoff, int8_t dir)
{
    uint8_t cur0 = *cur;
    int16_t off;

    if (dir == 0) return;
    dir = (dir > 0) ? 1 : -1;

    for (off = UI_ANIM_STEP; off <= UI_MENU_ROW_H; off += UI_ANIM_STEP)
    {
        ui_menu_scroll_frame(items, nitems, cur0, dir, off, xoff);
        vTaskDelay(0);
    }
    *cur = (cur0 + dir + nitems) % nitems;

    while (ui_menu_draw(items, nitems, *cur, xoff, 1))
    {
        vTaskDelay(UI_EASE_DELAY / portTICK_PERIOD_MS);
    }
}

/* ================= 主界面菜单 ================= */
static int16_t item_xoff[16];   /* 主界面各项水平偏移 */
static int16_t sub_xoff[24];    /* 子菜单各项水平偏移(容量=子项上限) */

/* 全界面重绘: 左侧时间+天气 + 图标居中 + 右侧菜单 */
static void ui_render_all(void)
{
    ui_sub_center = 0;   /* 主界面恒右对齐(防子菜单居中标志泄漏) */
    fb_clear(UI_COLOR_BG);
    if (ui_time_valid)
    {
        ui_time_draw_text();   /* 先写时间(仅左区), 由下方 ui_menu_draw 一次刷屏 */
    }
    if (ui_weather_valid)
    {
        ui_weather_draw_text();   /* 天气在时钟下方 */
    }
    fb_draw_image_mono(UI_ICON_X, UI_ICON_Y, 64, 64, icon_cmd, UI_COLOR_ICON, UI_COLOR_BG);
    ui_menu_draw(menu_items, MENU_COUNT, menu_cur, item_xoff, 0);
}

void UI_RenderScreen(void)
{
    ui_render_all();
}

void UI_SetCursorStyle(uint8_t style)
{
    if (style >= UI_CURSOR_N) style = UI_CURSOR_DEFAULT;
    ui_cursor_style = style;
}

uint8_t UI_GetCursorStyle(void)
{
    return ui_cursor_style;
}

/* ================= 对外接口 ================= */

void UI_Init(void)
{
    lcd_init();
    ui_menu_items_sync();
    ui_render_all();
}

void UI_Scroll(int8_t dir)
{
    ui_scroll_common(menu_items, MENU_COUNT, &menu_cur, item_xoff, dir);
}

uint8_t UI_GetSelect(void)
{
    return menu_cur;
}

/* 全屏两行居中显示(计时/倒计时): 行1标题, 行2数值 */
void UI_FullScreen(const char *line1, const char *line2)
{
    UI_ScrClear(UI_COLOR_BG);
    if (line1)
    {
        fb_draw_string((LCD_WIDTH - ui_text_width(line1)) / 2, 14, line1,
                       UI_COLOR_TIME, UI_COLOR_BG, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    }
    if (line2)
    {
        fb_draw_string((LCD_WIDTH - ui_text_width(line2)) / 2, 36, line2,
                       UI_COLOR_TIME, UI_COLOR_BG, 0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    }
    fb_blit();
}

/* ================= 通用子菜单(复用主界面样式) =================
 * 非阻塞状态机: 由 RTOS 主任务调用 Init/Scroll/Cur 驱动.
 * 子项 = {title}01 ~ {title}NN + "退出"; 位置/光标/滚动与主界面一致. */
static char ui_sub_items[24][48];         /* 子项文字缓冲(最多 23 子项 + 退出; 48B 容纳长人格名+数值) */
static const char *ui_sub_items_p[24];    /* 指针数组(通用渲染需要) */
static uint8_t ui_sub_nitems, ui_sub_cur;
static int16_t ui_sub_center_x;           /* 模式2的公共左对齐 x */
static int16_t ui_sub_center_dx = 0;      /* 模式2整体右移量 px(默认0) */
/* sub_xoff 已在主界面菜单节定义 */

/* 子菜单某项的 x(右对齐 / 逐项居中 / 块左对齐) */
static int16_t ui_menu_item_x(const char *s, int16_t xoff)
{
    if (ui_sub_center == 1)
    {
        return (LCD_WIDTH - ui_text_width(s)) / 2 + xoff;
    }
    if (ui_sub_center == 2)
    {
        return ui_sub_center_x + xoff;   /* 全体左对齐, 块以最长项居中 */
    }
    return UI_MENU_X + xoff;
}

void UI_SubMenuInit(const char *title, uint8_t count)
{
    uint8_t i;
    if (count > 22) count = 22;
    ui_sub_nitems = count + 1;
    ui_sub_cur = 0;
    ui_sub_center = 0;

    for (i = 0; i < count; i++)
    {
        snprintf(ui_sub_items[i], sizeof(ui_sub_items[i]), "%s%02u", title, (unsigned)i + 1);
    }
    snprintf(ui_sub_items[count], sizeof(ui_sub_items[count]), "退出");
    for (i = 0; i < ui_sub_nitems; i++)
    {
        ui_sub_items_p[i] = ui_sub_items[i];
        sub_xoff[i] = 0;
    }

    /* 进入子菜单: 清屏 + 右侧菜单(与主界面同位置/光标) */
    UI_ScrClear(UI_COLOR_BG);
    ui_menu_draw(ui_sub_items_p, ui_sub_nitems, ui_sub_cur, sub_xoff, 0);
}

/* 块左对齐(center=2)整体右移量(px), 供调用方微调对齐位置 */
void UI_SubMenuSetCenterDx(int16_t dx)
{
    ui_sub_center_dx = dx;
}

/* 用给定完整项列表渲染子菜单; center=0 右对齐 / 1 逐项居中(长文字不裁边) / 2 块左对齐居中 */
void UI_SubMenuInitItemsC(const char *const items[], uint8_t nitems, uint8_t center)
{
    uint8_t i;
    if (nitems > 23) nitems = 23;   /* 上限=缓冲容量(闹钟列表等长列表可环形滚动) */
    ui_sub_nitems = nitems;
    ui_sub_cur = 0;
    if (center >= 2)
    {
        ui_sub_center = 2;                       /* 块左对齐: 全体同 x, 以最长项水平居中 */
        int16_t maxw = 0, w;
        for (i = 0; i < nitems; i++)
        {
            w = ui_text_width(items[i]);
            if (w > maxw) maxw = w;
        }
        ui_sub_center_x = (LCD_WIDTH - maxw) / 2 + ui_sub_center_dx;
    }
    else
    {
        ui_sub_center = center ? 1 : 0;
    }

    for (i = 0; i < nitems; i++)
    {
        snprintf(ui_sub_items[i], sizeof(ui_sub_items[i]), "%s", items[i]);
        ui_sub_items_p[i] = ui_sub_items[i];
        sub_xoff[i] = 0;
    }

    /* 进入子菜单: 清屏 + 居中/右侧菜单 */
    UI_ScrClear(UI_COLOR_BG);
    ui_menu_draw(ui_sub_items_p, ui_sub_nitems, ui_sub_cur, sub_xoff, 0);
}

void UI_SubMenuInitItems(const char *const items[], uint8_t nitems)
{
    UI_SubMenuInitItemsC(items, nitems, 0);
}

void UI_SubMenuScroll(int8_t dir)
{
    ui_scroll_common(ui_sub_items_p, ui_sub_nitems, &ui_sub_cur, sub_xoff, dir);
}

uint8_t UI_SubMenuCur(void)
{
    return ui_sub_cur;
}

/* 更新子菜单单项文字(保持当前选中), 用于设置项显示当前值 */
void UI_SubMenuSetItem(uint8_t idx, const char *text)
{
    if (idx >= ui_sub_nitems)
    {
        return;
    }
    snprintf(ui_sub_items[idx], sizeof(ui_sub_items[idx]), "%s", text);
    ui_sub_items_p[idx] = ui_sub_items[idx];
    UI_ScrClear(UI_COLOR_BG);
    ui_menu_draw(ui_sub_items_p, ui_sub_nitems, ui_sub_cur, sub_xoff, 0);
}

/* 设置子菜单光标位置并重绘(无动画), 用于子流程返回后保持原选中项 */
void UI_SubMenuSetCur(uint8_t idx)
{
    if (idx >= ui_sub_nitems)
    {
        return;
    }
    ui_sub_cur = idx;
    UI_ScrClear(UI_COLOR_BG);
    ui_menu_draw(ui_sub_items_p, ui_sub_nitems, ui_sub_cur, sub_xoff, 0);
}
