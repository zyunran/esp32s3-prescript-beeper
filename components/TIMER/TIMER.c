/* TIMER 组件: 倒计时(未来到达)
 * 设定屏: "你想到达几分钟后的未来?" + 横向滑动条(上下改分钟, 数据左右滑动 + 角光标扩散)
 * 确认: 角光标扩散 -> 倒计时(标题不变, 中间显示 M:SS 正常字号)
 * 归零: 蜂鸣2下 + 标题换成"你已到达X分钟后的未来!"(按键退出)
 * 倒计时中按确认: 极速快进到归零并显示到达消息(不蜂鸣) */
#include "TIMER.h"
#include "LCD.h"   /* 仅用 LCD_WIDTH 屏幕几何宏(显式声明驱动依赖) */
#include "UI.h"
#include "INSTRUCTION.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>

typedef enum { PH_SET, PH_CONFIRM, PH_COUNT, PH_FAST, PH_DONE } tim_phase_t;

static tim_phase_t tim_ph;
static uint16_t tim_set_min;      /* 设定分钟 1..TIMER_MAX_MIN */
static uint32_t tim_start;        /* 倒计时开始时刻 ms */
static uint32_t tim_dur;          /* 倒计时总时长 ms */
static uint32_t tim_last_sec;     /* 上次显示秒 */
static uint64_t tim_fast_ms;     /* 快进累计补偿 ms(避免直接改 tim_start 造成下溢; uint64 防长按快进累积溢出) */
static uint32_t tim_anim_start;   /* 角光标扩散动画开始时刻 */
static int16_t  tim_corner_exp;   /* 角光标扩散量(0=收缩) */
static int16_t  tim_slide;        /* 滑动条动画偏移(0=静止) */
static uint32_t tim_slide_last;   /* 滑动动画上次时刻 */
static uint32_t tim_fast_last;    /* 快进上次时刻 */
static uint8_t  tim_exit_req;     /* 1=请求退出(OK 长按), 下次 Tick 返回 TIM_EXIT */

#define TIM_SET_Y_TITLE  12
#define TIM_SET_Y_BAR    44

static uint32_t tim_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 剩余时长 ms(含快进补偿; int64 避免下溢/回绕: 须先扩宽再相减, 防 uint32 差先回绕) */
static int64_t tim_remain(void)
{
    return (int64_t)tim_dur - ((int64_t)tim_now() - (int64_t)tim_start + (int64_t)tim_fast_ms);
}

/* ---- 绘制小工具 ---- */

static void tim_draw_digits(int16_t x, int16_t y, int v, uint16_t fc, uint16_t bc)
{
    char s[4];
    char c[2];
    uint16_t xo = 0;
    snprintf(s, sizeof(s), "%d", v);
    c[1] = '\0';
    for (char *p = s; *p; p++)
    {
        c[0] = *p;
        xo += UI_ScrGlyph(x + xo, y, c, fc, bc);
    }
}

/* 角光标: 四个直角框住 (x0,y0)-(x1,y1), 扩散量 e 向外 */
static void tim_draw_corners(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t e)
{
    UI_ScrRect((uint16_t)(x0 - e), (int16_t)(y0 - e), 4, 1, UI_COLOR_FRAME);
    UI_ScrRect((uint16_t)(x0 - e), (int16_t)(y0 - e), 1, 4, UI_COLOR_FRAME);
    UI_ScrRect((uint16_t)(x1 + e - 3), (int16_t)(y0 - e), 4, 1, UI_COLOR_FRAME);
    UI_ScrRect((uint16_t)(x1 + e - 1), (int16_t)(y0 - e), 1, 4, UI_COLOR_FRAME);
    UI_ScrRect((uint16_t)(x0 - e), (int16_t)(y1 + e - 1), 4, 1, UI_COLOR_FRAME);
    UI_ScrRect((uint16_t)(x0 - e), (int16_t)(y1 + e - 3), 1, 4, UI_COLOR_FRAME);
    UI_ScrRect((uint16_t)(x1 + e - 3), (int16_t)(y1 + e - 1), 4, 1, UI_COLOR_FRAME);
    UI_ScrRect((uint16_t)(x1 + e - 1), (int16_t)(y1 + e - 3), 1, 4, UI_COLOR_FRAME);
}

/* ================= 标题乱码破译 =================
 * 设定屏标题"你想到达几分钟后的未来?"与到达屏"你已到达X分钟后的未来!"
 * 以"全乱码 -> 逐字揭示"方式显示, 与指令破译(INSTRUCTION)风格一致.
 * 独立状态机, 由 TIM_Tick 推进; 数字条/角光标照常绘制互不干扰. */
#define TIM_TITLE_MAX   20
static char    tim_title_ch[TIM_TITLE_MAX][4];   /* 标题字符(UTF-8) */
static uint8_t tim_title_w[TIM_TITLE_MAX];        /* 每字符像素宽 */
static uint8_t tim_title_n;                       /* 字数 */
static uint8_t tim_title_reveal;                  /* 已揭示字数 */
static uint8_t tim_title_phase;                   /* 0=全乱码 1=揭示 2=完成 */
static uint8_t tim_title_frames;                  /* 已乱码帧数 */
static uint32_t tim_title_last;                   /* 乱码帧时刻 */
static uint32_t tim_title_rlast;                  /* 揭示时刻 */

/* 解析标题为字符数组(汉字 16px / ASCII 8px) */
static void tim_title_parse(const char *s)
{
    uint16_t i = 0;
    tim_title_n = 0;
    while (s[i] != '\0' && tim_title_n < TIM_TITLE_MAX)
    {
        uint8_t len, k;
        if (s[i] & 0x80)
        {
            len = 3;
            if ((s[i] & 0xE0) == 0xC0) len = 2;   /* 兼容 2 字节 UTF-8 */
        }
        else
        {
            len = 1;
        }
        for (k = 0; k < len && s[i + k] != '\0'; k++) tim_title_ch[tim_title_n][k] = s[i + k];
        tim_title_ch[tim_title_n][k] = '\0';
        tim_title_w[tim_title_n] = (s[i] & 0x80) ? 16 : 8;
        tim_title_n++;
        if (k == 0) break;        /* 串已尽(截断多字节): 不再前推 */
        i += k;
    }
}

/* 生成单个乱码(中文槽 2 个 ASCII, ASCII 槽 1 个) */
static void tim_title_garble(char *buf, uint8_t cjk)
{
    static const char SET[] = "ABCDEF@HIJ_LM%OPQR^WX#YZab#cdefgh*iqrxyz0123456789";
    buf[0] = SET[esp_random() % (sizeof(SET) - 1)];
    buf[1] = cjk ? SET[esp_random() % (sizeof(SET) - 1)] : '\0';
    buf[2] = '\0';
}

/* 画标题一行(居中, 按破译进度: 已揭示字真色, 未揭示乱码钢蓝) */
static void tim_title_render(int16_t y)
{
    int16_t total = 0, x;
    uint8_t c;
    for (c = 0; c < tim_title_n; c++) total += tim_title_w[c];
    x = (LCD_WIDTH - total) / 2;
    for (c = 0; c < tim_title_n; c++)
    {
        if (c < tim_title_reveal)
        {
            UI_ScrGlyph((uint16_t)x, y, tim_title_ch[c], UI_COLOR_TIME, UI_COLOR_BG);
        }
        else
        {
            char gb[4];
            tim_title_garble(gb, (tim_title_w[c] == 16));
            UI_ScrGlyph((uint16_t)x, y, gb, INS_SCR_GARBLE, UI_COLOR_BG);
            if (gb[1]) UI_ScrGlyph((uint16_t)(x + 8), y, &gb[1], INS_SCR_GARBLE, UI_COLOR_BG);
        }
        x += tim_title_w[c];
    }
}

/* 推进标题乱码, 返回 1=内容有变化(调用方应重绘) */
static uint8_t tim_title_tick(void)
{
    uint32_t now = tim_now();
    if (tim_title_phase == 0)
    {
        if (now - tim_title_last < INS_SCR_DELAY_MS) return 0;
        tim_title_last = now;
        tim_title_frames++;
        if (tim_title_frames >= INS_SCR_FRAMES)
        {
            tim_title_phase = 1;
            tim_title_rlast = now;
        }
        return 1;
    }
    if (tim_title_phase == 1)
    {
        if (now - tim_title_rlast < INS_REVEAL_DELAY_MS) return 0;
        tim_title_rlast = now;
        if (tim_title_reveal < tim_title_n) tim_title_reveal++;
        if (tim_title_reveal >= tim_title_n) tim_title_phase = 2;
        return 1;
    }
    return 0;
}

/* 启动一段标题乱码破译 */
static void tim_title_start(const char *s)
{
    tim_title_parse(s);
    tim_title_reveal = 0;
    tim_title_frames = 0;
    tim_title_phase = 0;
    tim_title_last = tim_now();
    tim_title_rlast = tim_now();
}

/* 强制标题立即显示真字(确认/倒计时阶段) */
static void tim_title_finish(void)
{
    if (tim_title_phase != 2)
    {
        tim_title_reveal = tim_title_n;
        tim_title_phase = 2;
    }
}

/* 设定屏: 标题(乱码破译) + 滑动条(带滑动偏移, 尽量铺满屏宽) + 角光标 */
static void tim_render_set(void)
{
    int16_t i, cx = LCD_WIDTH / 2;
    int16_t w = (tim_set_min >= 10) ? 16 : 8;
    UI_ScrClear(UI_COLOR_BG);
    tim_title_render(TIM_SET_Y_TITLE);

    for (i = -5; i <= 5; i++)     /* 显示 v-5..v+5, 铺到屏幕边缘 */
    {
        int v = (int)tim_set_min + i;
        int vw;
        if (v < 1 || v > TIMER_MAX_MIN) continue;
        vw = (v >= 10) ? 16 : 8;
        tim_draw_digits(cx + i * 26 - vw / 2 + tim_slide, TIM_SET_Y_BAR, v,
                        UI_COLOR_TIME, UI_COLOR_BG);
    }
    tim_draw_corners(cx - w / 2 - 3, TIM_SET_Y_BAR - 3, cx + w / 2 + 2, TIM_SET_Y_BAR + 15 + 3,
                     tim_corner_exp);
    UI_ScrBlit();
}

/* 确认瞬间: 标题立即真字 + 画一帧向外扩散 e=6 的角框(200ms 过渡动画期间不再逐帧推进) */
static void tim_render_set_confirm(void)
{
    int16_t cx = LCD_WIDTH / 2;
    int16_t w = (tim_set_min >= 10) ? 16 : 8;
    tim_title_finish();          /* 确认时标题强制显示真字 */
    UI_ScrClear(UI_COLOR_BG);
    tim_title_render(TIM_SET_Y_TITLE);
    tim_draw_digits(cx - w / 2, TIM_SET_Y_BAR, tim_set_min, UI_COLOR_TIME, UI_COLOR_BG);
    tim_draw_corners(cx - w / 2 - 3, TIM_SET_Y_BAR - 3, cx + w / 2 + 2, TIM_SET_Y_BAR + 15 + 3, 6);
    UI_ScrBlit();
}

/* 倒计时显示: 标题不变, 中间 M:SS 正常字号, 角光标左右扩大框住时间 */
static void tim_render_count(void)
{
    int64_t rem = tim_remain();
    uint32_t remain = (rem > 0) ? (uint32_t)rem : 0;
    uint32_t sec = remain / 1000;
    char buf[16];
    int16_t cx = LCD_WIDTH / 2;
    int16_t tw;
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    tw = (int16_t)strlen(buf) * 8;

    UI_ScrClear(UI_COLOR_BG);
    tim_title_render(TIM_SET_Y_TITLE);
    UI_ScrTextCenter(46, buf, UI_COLOR_TIME, UI_COLOR_BG);
    /* 角光标: 比时间左右各宽 15px 框住(左右扩大) */
    tim_draw_corners(cx - tw / 2 - 15, 43, cx + tw / 2 + 15, 61, 0);
    UI_ScrBlit();
}

/* 到达画面: 标题换成"你已到达X分钟后的未来!"(乱码破译) + 下方 00:00 */
static void tim_render_done(void)
{
    UI_ScrClear(UI_COLOR_BG);
    tim_title_render(30);
    UI_ScrTextCenter(52, "00:00", UI_COLOR_TIME, UI_COLOR_BG);
    UI_ScrBlit();
}

static void tim_show_done(uint8_t render)
{
    char buf[40];
    tim_ph = PH_DONE;
    snprintf(buf, sizeof(buf), "你已到达%u分钟后的未来!", (unsigned)tim_set_min);
    tim_title_start(buf);        /* 到达标题也走乱码破译 */
    if (render) tim_render_done();
}

/* ================= 对外接口 ================= */
void TIM_Enter(void)
{
    tim_ph = PH_SET;
    tim_set_min = 6;          /* 默认 6 分钟 */
    tim_corner_exp = 0;
    tim_slide = 0;
    tim_anim_start = 0;
    tim_fast_ms = 0;
    tim_exit_req = 0;
    tim_title_start("你想到达几分钟后的未来?");   /* 标题乱码破译 */
    tim_render_set();
}

/* 强制退出(OK 长按返回), 由 UI 主任务调用 */
void TIM_Exit(void)
{
    tim_exit_req = 1;
}

tim_ret_t TIM_Key(uint8_t up, uint8_t ok, uint8_t down)
{
    switch (tim_ph)
    {
        case PH_SET:
            if (up && tim_set_min < TIMER_MAX_MIN)
            {
                tim_set_min++;
                tim_corner_exp = 2;      /* 角扩散 */
                tim_slide = 26;          /* 数字从右滑入 */
                tim_anim_start = tim_now();
                tim_slide_last = tim_now();
                tim_render_set();
            }
            else if (down && tim_set_min > 1)
            {
                tim_set_min--;
                tim_corner_exp = 2;
                tim_slide = -26;         /* 数字从左滑入 */
                tim_anim_start = tim_now();
                tim_slide_last = tim_now();
                tim_render_set();
            }
            else if (ok)              /* 确认: 角扩散后开始倒计时 */
            {
                tim_ph = PH_CONFIRM;
                tim_anim_start = tim_now();
                tim_render_set_confirm();
            }
            break;

        case PH_CONFIRM:              /* 过渡动画期间忽略按键 */
            break;

        case PH_COUNT:
            if (ok)                   /* 倒计时中确认: 快进归零(显示到达消息, 不蜂鸣) */
            {
                tim_ph = PH_FAST;
                tim_fast_last = tim_now() - 30;   /* 让第一帧立即快进 */
            }
            break;

        case PH_FAST:
            break;

        case PH_DONE:                 /* 到达消息: 任意键退出 */
            return TIM_EXIT;
    }
    return TIM_RUN;
}

tim_ret_t TIM_Tick(uint8_t render)
{
    uint32_t now = tim_now();

    if (tim_exit_req)          /* OK 长按: 退出 */
    {
        tim_exit_req = 0;
        return TIM_EXIT;
    }

    switch (tim_ph)
    {
        case PH_SET:
            if (!render) break;                 /* 熄屏: 保持状态, 不推进动画/不重绘 */
            /* 标题乱码破译推进 */
            if (tim_title_tick()) tim_render_set();
            /* 滑动条滑动动画 */
            if (tim_slide != 0 && now - tim_slide_last >= 15)
            {
                tim_slide_last = now;
                tim_slide += (tim_slide > 0) ? -3 : 3;
                if ((tim_slide > 0 && tim_slide <= 3) || (tim_slide < 0 && tim_slide >= -3))
                {
                    tim_slide = 0;
                }
                tim_render_set();
            }
            /* 角光标 150ms 后收缩 */
            if (tim_anim_start && now - tim_anim_start >= 150)
            {
                tim_anim_start = 0;
                tim_corner_exp = 0;
                tim_render_set();
            }
            break;

        case PH_CONFIRM:
            /* 确认扩散 200ms 后开始倒计时 */
            if (now - tim_anim_start >= 200)
            {
                tim_ph = PH_COUNT;
                tim_start = now;
                tim_fast_ms = 0;
                tim_dur = (uint32_t)tim_set_min * 60000;
                tim_last_sec = 0xFFFFFFFF;   /* 强制先显示 */
                if (render) tim_render_count();
            }
            break;

        case PH_COUNT:
        {
            if (tim_remain() <= 0)    /* 自然归零: 到达画面 + 蜂鸣提示 */
            {
                tim_show_done(render);
                return TIM_DONE;
            }
            if (render)               /* 熄屏不重绘; 亮屏后 last_sec 已过期会强制刷新 */
            {
                uint32_t sec = (uint32_t)(tim_remain() / 1000);
                if (sec != tim_last_sec)
                {
                    tim_last_sec = sec;
                    tim_render_count();
                }
            }
            break;
        }

        case PH_FAST:
            /* 快进: 每 10ms 补偿 5 秒剩余(500x), 归零进到达画面(不蜂鸣) */
            if (now - tim_fast_last >= 10)
            {
                tim_fast_last = now;
                tim_fast_ms += 5000;
                if (tim_remain() <= 0)
                {
                    tim_show_done(render);   /* 快进归零: 同样显示到达消息, 但不返回 TIM_DONE(不蜂鸣) */
                    return TIM_RUN;
                }
                if (render) tim_render_count();
            }
            break;

        case PH_DONE:
            if (render && tim_title_tick()) tim_render_done();   /* 到达标题乱码破译推进 */
            break;
    }
    return TIM_RUN;
}
