/* INSTRUCTION 组件: 指令乱码破译显示 + 解码期间蜂鸣
 *  - 全屏显示一条指令文本: 先全乱码再逐字"破译"成真字
 *  - 支持 {#RRGGBB} 颜色 / {} 恢复默认色 / {TIMER} 内联计时占位
 *  - 乱码全取 ASCII 且字符数≠真字; 已解码字带滑入位移, 小概率回退乱码
 *  - 解码期间有源蜂鸣器(GPIO15, GPIO电平)随机响 3~4 次, 破译完成提示一响
 * 绘制使用 UI 组件帧缓冲接口(UI_ScrClear/UI_ScrGlyph/UI_ScrBlit/UI_RenderScreen)。
 */
#include "INSTRUCTION.h"
#include "UI.h"
#include "LCD.h"
#include "SOUND.h"
#include "TODO.h"
#include "snd_data.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 破译参数(运行时可改, 默认护眼风格; WEB配置存NVS) */
uint16_t INS_SCR_DEFAULT     = 0xE249;   /* 破译真字: 清晰红 */
uint16_t INS_SCR_GARBLE      = 0x651D;   /* 未破译乱码: 亮钢蓝 */
uint16_t INS_SCR_DELAY_MS    = 18;       /* 乱码刷新间隔 ms */
uint16_t INS_REVEAL_DELAY_MS = 80;       /* 逐字揭示间隔 ms */

/* 破译完成提示蜂鸣(定义于下文蜂鸣器节) */
static void ins_done_beep(void);

/* 破译/蜂鸣可调参数定义在 INSTRUCTION.h 的"可调参数"节, 改那里即可 */
#define INS_GLYPH_LINES       3              /* 最多行数(内部缓冲) */
#define INS_GLYPH_MAX         40             /* 每行最多字数(内部缓冲) */
#define INS_TEXT_MAX_W        (LCD_WIDTH - 12) /* 文本最大宽 */

/* 破译字号(运行时可调, 设置菜单+WEB, 存NVS "ins2"/"fnt"; 0=16px 1=24px 2=32px) */
static uint8_t ins_font = 1;                 /* 默认 24px */

static uint8_t ins_font_h(void) { return (uint8_t)(16 + ins_font * 8); }   /* 单字高 16/24/32 */
static uint8_t ins_font_w(const char *ch) { return (uint8_t)(ins_font_h() / (ch[0] & 0x80 ? 1 : 2)); } /* 汉字=高, ASCII=高/2 */
static uint8_t ins_max_lines(void)           /* 屏高允许的最大行数(自动匹配) */
{
    uint8_t fh = ins_font_h();
    uint8_t m = (uint8_t)((LCD_HEIGHT + 8) / (fh + 8));
    if (m > INS_GLYPH_LINES) m = INS_GLYPH_LINES;
    return m ? m : 1;
}

/* ================= 预设指令(内置默认, 仅首次使用) =================
 * 运行期指令库存 NVS(命名空间 "ins"), 可用 WEB 配置页增删改.
 * 支持 {#RRGGBB}/{} 颜色、{RAND:min-max} 随机数、{TIMER} 占位; 文字须在全字库内. */
static const char *const ins_defaults[] = {
    "致拉:明日正午之前在厉利的生日蛋糕中放入3根针。",
    "致李德九:与您遇见的第3个人猜拳且出拳头,若您获胜则拔掉对方59根头发……",
    "拌以只吃泡沫塑料长大的黄粉虫,海鲜奶油意粉酱涂抹3次后用叉子将其吃掉。",
    "杀掉你画的画。",
    "将拉马库斯的脑叶搅个稀碎。",
    "致乔恩:明日3时38分在某十字路口望向东方并挥7次手。",
    "搜捕L巢内的37岁人士并抽出脊椎。",
    "将今日遇见的第14个人的左腿与第26个人的右腿互换。",
    "屠杀拇指,无时限。",
    "将巢内的拇指余孽剁去手足,穿刺于尖桩之上……",
    "忠实履行阳传令传达的指令。",
    "……致蔡宪。遇到在三岔路口处挥手7次的人时便直接跟着他直至他的家中。",
    "吃掉15个清道夫并抽取丝绸织布。",
    "斩开三次后巷夜晚的街道之光。期限为半年。",
    "斩首7名刀刃横在脖颈上时仍不放弃反抗的后巷居民。",
    "剪下663缕头发放入信封中并投入3号检票口。",
    "找到以实玛利。",
    "盯着角落看整整300秒。",
    "把一张白纸撕成正好17片。",
    "对下一个开口说话的人低语“是”。",
    "把一枚生锈的硬币放在第三级台阶上。",
    "剪断红线,留下蓝线。",
    "数一数一块死掉的微控制器上的引脚。",
    "盯着空白屏幕,直到它闪烁。",
    "在没有水的地方抛线。",
    "等待空钩上钩。",
    "指着天花板低语“是”。",
};
#define INS_DEFAULT_COUNT (sizeof(ins_defaults) / sizeof(ins_defaults[0]))

/* ================= 特殊代行者: 里恩专属指令库 =================
 * 只有当前使用者为里恩时才进入随机池, 且里恩只会收到这里的指令. */
#define INS_USER_RIEN_N 11
static const char *const ins_user_rien[] = {
    "致??:你必须更名为里恩。",
    "致里恩:扮演漆黑缄默。",
    "致里恩:潜入边狱公司。",
    "致里恩:五分钟内,只许以第三人称自称。",
    "致里恩:必要时,每句话都应以“……如指令之意。”作结。",
    "致里恩:每句话都必须以“奉我之令。”起首。",
    "致里恩:前往海边,凝望浪涛。",
    "致??:舍弃你的过去。将头发染成缀着银丝的黑,身着金纽黑西装,习得某座图书馆收尾人的举止与武艺。从今往后,你就是里恩。",
    "致里恩:买一本绘本。为良秀恰好朗读{RAND:1-14}页,随后猝然合上。无论如何都不许讲完故事,好让她为了有朝一日听完结局,而留在你身边。",
    "致里恩:与学徒空交谈时,语气温和而体恤。保持寡言,只在旁人问起时,才分享见解。",
    "致里恩:念出诗句:痛苦啊,你便是我的唯一,除了你,我皆无欲求",
};

/* ================= 特殊代行者: 浮士德专属指令库 ================= */
#define INS_USER_FAUST_N 8
static const char *const ins_user_faust[] = {
    "致浮士德:将你的声带在沸腾的盐水中烹煮14秒,并将其作为晚餐的配菜食用。",
    "致浮士德:连接Gesellschaft,并了解三件你所不知道的事。",
    "致浮士德:一周之内,在B区十字路口与苦行者们汇合,并在街上大步前行。",
    "致浮士德:登上三层以上的建筑物楼顶,向下方挥手一分钟。",
    "致浮士德:作为当日的第三位客人进入卖炸鸡的餐厅,并最晚离开。",
    "致浮士德:与三个以上的人一起玩捉迷藏后,与“鬼”一起回家。",
    "致浮士德:读六本书,并到访所读到的最后一本书中所出现的区域。期限为无限。",
    "致浮士德:杀死巷子的巷子内的欺骗者。",
};

/* ================= 运行期指令库(NVS 持久化, WEB 可改) ================= */
static char ins_presets[INS_PRESET_MAX][INS_PRESET_LEN];
static uint8_t ins_preset_count = 0;

/* 当前使用者名称(神谕指令"致X:"对象; 默认李箱, NVS "ins2"/"user") */
static char ins_user[INS_USER_NAME_MAX] = "李箱";

/* ================= 破译状态 ================= */
typedef struct {
    char ch[4];                             /* 真字(UTF-8) */
    uint16_t color;                         /* 该字颜色 */
    int8_t  slide;                          /* 解码字滑入位移(px, 右移为+, 0=到位) */
} ins_glyph_t;

static ins_glyph_t ins_gl[INS_GLYPH_LINES][INS_GLYPH_MAX];
static uint8_t  ins_gl_num[INS_GLYPH_LINES];  /* 每行字数 */
static int16_t  ins_gl_y[INS_GLYPH_LINES];    /* 每行 top y */
static uint8_t  ins_gl_lines;                 /* 行数 */
static uint8_t  ins_gl_total;                 /* 总字数 */
static uint8_t  ins_scr_rev[INS_GLYPH_LINES][INS_GLYPH_MAX]; /* 本帧回退乱码标记 */

static uint8_t  ins_scr_on;                   /* 1=破译显示中 */
static uint8_t  ins_scr_phase;                /* 0=全乱码 1=逐字揭示 2=完成 */
static uint8_t  ins_scr_frames;               /* 已乱码帧数 */
static uint8_t  ins_reveal_idx;               /* 已破译字数 */
static int8_t   ins_xoff;                     /* 全乱码阶段整块左右偏移 */
static uint32_t ins_scr_last;                 /* 上次乱码帧刷新时刻 ms */
static uint32_t ins_reveal_last;              /* 上次逐字揭示时刻 ms */

static uint32_t ins_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int ins_is_timer_tag(const char *s)
{
    return (strncmp(s, "{TIMER}", 7) == 0);
}

static uint16_t ins_hex_to_565(const char *hex)
{
    long v = strtol(hex, NULL, 16);
    uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (uint16_t)(b >> 3);
}

/* ---- 解析: 逐字放入 ins_gl, 处理 {#RRGGBB} / {} / {TIMER}, 自动换行 ---- */
static uint8_t ins_p_line, ins_p_col;
static int16_t ins_p_x;

static void ins_glyph_put(uint16_t color, const char *ch, uint8_t w)
{
    if (ins_p_line >= INS_GLYPH_LINES)
    {
        return;
    }
    if (ins_p_x + w > INS_TEXT_MAX_W && ins_p_col > 0)
    {
        ins_p_line++;
        ins_p_col = 0;
        ins_p_x = 0;
    }
    if (ins_p_line >= INS_GLYPH_LINES)
    {
        return;
    }
    ins_gl[ins_p_line][ins_p_col].color = color;
    strncpy(ins_gl[ins_p_line][ins_p_col].ch, ch, 3);
    ins_gl[ins_p_line][ins_p_col].ch[3] = '\0';
    ins_gl[ins_p_line][ins_p_col].slide = 0;
    ins_p_col++;
    ins_gl_num[ins_p_line] = ins_p_col;
    ins_p_x += w;
}

static void ins_scr_parse(const char *text)
{
    uint16_t i = 0;
    uint16_t color = INS_SCR_DEFAULT;

    ins_p_line = 0;
    ins_p_col = 0;
    ins_p_x = 0;
    memset(ins_gl_num, 0, sizeof(ins_gl_num));

    while (text[i] != '\0')
    {
        if (strncmp(&text[i], "{#", 2) == 0)            /* {#RRGGBB} */
        {
            char hex[7] = {0};
            uint16_t j = i + 2, k = 0;
            while (k < 6 && text[j] != '\0' && text[j] != '}')
            {
                hex[k] = text[j];
                k++;
                j++;
            }
            color = ins_hex_to_565(hex);
            i = (text[j] == '}') ? (j + 1) : j;
        }
        else if (strncmp(&text[i], "{}", 2) == 0)       /* {} 恢复默认色 */
        {
            color = INS_SCR_DEFAULT;
            i += 2;
        }
        else if (strncmp(&text[i], "{RAND:", 6) == 0)   /* {RAND:min-max} 随机数 */
        {
            const char *p = &text[i + 6];
            int minv = 0, maxv = 0, v;
            while (*p >= '0' && *p <= '9') { minv = minv * 10 + (*p - '0'); p++; }
            if (*p == '-')
            {
                p++;
                while (*p >= '0' && *p <= '9') { maxv = maxv * 10 + (*p - '0'); p++; }
            }
            if (*p == '}')
            {
                if (maxv < minv) { int t = minv; minv = maxv; maxv = t; }
                if (maxv <= minv) v = minv;
                else v = minv + (int)(esp_random() % (uint32_t)(maxv - minv + 1));
                {
                    char rbuf[8];
                    uint8_t k;
                    snprintf(rbuf, sizeof(rbuf), "%d", v);
                    for (k = 0; rbuf[k] != '\0'; k++)
                    {
                        char c[2] = { rbuf[k], '\0' };
                        ins_glyph_put(color, c, (uint8_t)(ins_font_h() / 2));
                    }
                }
                i = (uint16_t)(p - text) + 1;
            }
            else
            {
                i += 6;   /* 畸形标签: 跳过 {RAND: */
            }
        }
        else if (ins_is_timer_tag(&text[i]))            /* {TIMER} 静态占位 */
        {
            static const char t[] = "10:00";
            uint8_t k;
            for (k = 0; t[k] != '\0'; k++)
            {
                char c[2] = { t[k], '\0' };
                ins_glyph_put(color, c, (uint8_t)(ins_font_h() / 2));
            }
            i += 7;
        }
        else if (text[i] == '\n')                       /* 手动换行 */
        {
            ins_p_line++;
            ins_p_col = 0;
            ins_p_x = 0;
            i++;
        }
        else                                            /* 普通字符 */
        {
            char ch[4] = {0};
            uint8_t len = 1, w;
            if (text[i] & 0x80)
            {
                ch[0] = text[i];
                w = ins_font_h();
                if ((text[i] & 0xE0) == 0xC0)                 /* 2 字节 UTF-8(如 · 、 Ö): 此前一律按 3 字节会错位 */
                {
                    if (text[i + 1] == '\0') break;           /* 截断 UTF-8: 停止, 防画半个字 */
                    len = 2;
                    ch[1] = text[i + 1];
                }
                else if ((text[i] & 0xE0) == 0xE0)            /* 3 字节 UTF-8(CJK 汉字) */
                {
                    if (text[i + 1] == '\0' || text[i + 2] == '\0') break;
                    len = 3;
                    ch[1] = text[i + 1];
                    ch[2] = text[i + 2];
                }
                else break;                                   /* 非法/孤立首字节: 停止, 不画半个字 */
            }
            else
            {
                ch[0] = text[i];
                w = (uint8_t)(ins_font_h() / 2);
            }
            ins_glyph_put(color, ch, w);
            i += len;
        }
    }

    /* 行数自动匹配文本(最多屏高允许, 无需用户设置) */
    ins_gl_lines = ins_p_line + 1;
    {
        uint8_t m = ins_max_lines();
        if (ins_gl_lines > m) ins_gl_lines = m;
    }
    ins_gl_total = 0;
    for (i = 0; i < ins_gl_lines; i++)
    {
        ins_gl_total += ins_gl_num[i];
    }

    /* 垂直居中, 行高 = 字高 + 间距8(动态适配字号) */
    {
        int16_t fh = (int16_t)ins_font_h();
        int16_t block_h = (int16_t)ins_gl_lines * fh + ((int16_t)ins_gl_lines - 1) * 8;
        int16_t start_y = (LCD_HEIGHT - block_h) / 2;
        for (i = 0; i < ins_gl_lines; i++)
        {
            ins_gl_y[i] = start_y + (int16_t)i * (fh + 8);
        }
    }
}

static int16_t ins_line_width(uint8_t line)
{
    int16_t w = 0;
    uint8_t c;
    for (c = 0; c < ins_gl_num[line]; c++)
    {
        w += (int16_t)ins_font_w(ins_gl[line][c].ch);
    }
    return w;
}

/* 生成乱码串: 中文槽→2个ASCII, ASCII槽→1个ASCII, 空格保留. */
static void ins_garble_make(char *buf, const ins_glyph_t *g)
{
    static const char SET[] = "ABCDEF@HIJ_LM%OPQR^WX#YZab#cdefgh*iqrxyz0123456789";
    if (g->ch[0] == ' ')
    {
        buf[0] = ' ';
        buf[1] = '\0';
    }
    else if (g->ch[0] & 0x80)
    {
        buf[0] = SET[esp_random() % (sizeof(SET) - 1)];
        buf[1] = SET[esp_random() % (sizeof(SET) - 1)];
        buf[2] = '\0';
    }
    else
    {
        buf[0] = SET[esp_random() % (sizeof(SET) - 1)];
        buf[1] = '\0';
    }
}

/* 渲染一帧:
 *   pass1 画乱码(未解码字 + 本帧回退的已解码字);
 *   pass2 画已解码字(带滑入位移)在最上层, 使其从右侧滑入到位. */
static void ins_scr_render(void)
{
    uint8_t l, c;
    uint16_t idx = 0;

    UI_ScrClear(UI_COLOR_BG);

    /* 本帧各已解码字是否回退乱码 */
    idx = 0;
    for (l = 0; l < ins_gl_lines; l++)
    {
        for (c = 0; c < ins_gl_num[l]; c++)
        {
            ins_scr_rev[l][c] = (idx < ins_reveal_idx && ins_scr_phase == 1 &&
                                 (esp_random() % 100) < INS_REVERT_PCT);
            idx++;
        }
    }

    /* pass1: 乱码 */
    idx = 0;
    for (l = 0; l < ins_gl_lines; l++)
    {
        int16_t x = (LCD_WIDTH - ins_line_width(l)) / 2 + ins_xoff;
        for (c = 0; c < ins_gl_num[l]; c++)
        {
            char gbuf[4];
            if (!(idx < ins_reveal_idx) || ins_scr_rev[l][c])
            {
                ins_garble_make(gbuf, &ins_gl[l][c]);
                UI_ScrGlyphF(x, ins_gl_y[l], gbuf, ins_font_h(), INS_SCR_GARBLE, UI_COLOR_BG);
                if (gbuf[1] != '\0')
                {
                    UI_ScrGlyphF(x + ins_font_h() / 2, ins_gl_y[l], &gbuf[1],
                                 ins_font_h(), INS_SCR_GARBLE, UI_COLOR_BG);
                }
            }
            x += (int16_t)ins_font_w(ins_gl[l][c].ch);
            idx++;
        }
    }

    /* pass2: 已解码字(滑入)画最上层 */
    idx = 0;
    for (l = 0; l < ins_gl_lines; l++)
    {
        int16_t x = (LCD_WIDTH - ins_line_width(l)) / 2 + ins_xoff;
        for (c = 0; c < ins_gl_num[l]; c++)
        {
            if ((idx < ins_reveal_idx) && !ins_scr_rev[l][c])
            {
                UI_ScrGlyphF(x + ins_gl[l][c].slide, ins_gl_y[l], ins_gl[l][c].ch,
                             ins_font_h(), ins_gl[l][c].color, UI_COLOR_BG);
            }
            x += (int16_t)ins_font_w(ins_gl[l][c].ch);
            idx++;
        }
    }
    UI_ScrBlit();
}

/* 推进破译动画一帧(由 INS_Tick 每主循环调用) */
static void ins_scr_step(void)
{
    uint32_t now = ins_now_ms();
    if (ins_scr_phase == 0)
    {
        if (now - ins_scr_last >= INS_SCR_DELAY_MS)
        {
            ins_scr_last = now;
            ins_scr_frames++;
            ins_xoff = (ins_scr_frames & 1) ? INS_WOBBLE : -INS_WOBBLE;   /* 整块左右抖动 */
            ins_scr_render();
            if (ins_scr_frames >= INS_SCR_FRAMES)
            {
                ins_scr_phase = 1;
                ins_scr_last = now;
                ins_reveal_last = now;
                ins_xoff = 0;
            }
        }
    }
    else if (ins_scr_phase == 1)
    {
        uint8_t l, c;
        uint16_t idx;

        /* 高频乱码帧(18ms): 刷新乱码 + 已解码字滑入推进, 与全乱码阶段同帧率 */
        if (now - ins_scr_last >= INS_SCR_DELAY_MS)
        {
            ins_scr_last = now;

            idx = 0;
            for (l = 0; l < ins_gl_lines; l++)
            {
                for (c = 0; c < ins_gl_num[l]; c++)
                {
                    if (idx < ins_reveal_idx && ins_gl[l][c].slide > 0)
                    {
                        ins_gl[l][c].slide -= INS_SLIDE_STEP;
                    }
                    idx++;
                }
            }
            ins_scr_render();
        }

        /* 逐字揭示(80ms 一字) */
        if (now - ins_reveal_last >= INS_REVEAL_DELAY_MS)
        {
            ins_reveal_last = now;

            if (ins_reveal_idx < ins_gl_total)
            {
                ins_reveal_idx++;

                /* 新解码的字: 从右侧滑入 */
                {
                    uint16_t gidx = ins_reveal_idx - 1;
                    uint8_t l2 = 0;
                    while (l2 < ins_gl_lines && gidx >= ins_gl_num[l2])
                    {
                        gidx -= ins_gl_num[l2];
                        l2++;
                    }
                    ins_gl[l2][gidx].slide = INS_SLIDE_START;
                }
            }
            if (ins_reveal_idx >= ins_gl_total)
            {
                /* 收尾: 所有字归位, 渲染最终对齐的一帧 */
                ins_scr_phase = 2;
                for (l = 0; l < ins_gl_lines; l++)
                {
                    for (c = 0; c < ins_gl_num[l]; c++)
                    {
                        ins_gl[l][c].slide = 0;
                    }
                }
                ins_scr_render();
                SOUND_Stop();                              /* 停进行音 */
                ins_done_beep();                           /* 破译完成: 蜂鸣器提示一响 */
            }
        }
    }
}

/* ================= 蜂鸣器(有源, GPIO15, 低电平有效: 低=响 高=静) ================= */
static uint8_t  ins_beep_remain;              /* 剩余蜂鸣次数 */
static uint8_t  ins_beep_on;                  /* 正在响 */
static uint32_t ins_beep_stop;                /* 本次蜂鸣结束时刻 */
static uint32_t ins_beep_next;                /* 下次蜂鸣开始时刻 */
static uint8_t  ins_beep_enabled = 1;         /* 蜂鸣总开关(设置可调) */

static void ins_buzzer_init(void)
{
    gpio_config_t io = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << GPIO_NUM_15,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* 上拉: 引脚空闲高=静, 防上电一直响 */
    };
    gpio_config(&io);
    gpio_set_level(GPIO_NUM_15, 1);        /* 初始高=静(低电平有效) */
    /* 最大驱动能力, 提高蜂鸣器音量 */
    gpio_set_drive_capability(GPIO_NUM_15, GPIO_DRIVE_CAP_3);
}

static void ins_buzzer_on(void)
{
    gpio_set_level(GPIO_NUM_15, 0);   /* 有源低电平有效: 低=响 */
}

static void ins_buzzer_off(void)
{
    gpio_set_level(GPIO_NUM_15, 1);
}

/* 破译完成提示: 蜂鸣器响 1~2 下 */
static void ins_done_beep(void)
{
    ins_beep_remain = 1 + (esp_random() & 1);   /* 1~2 下 */
    ins_beep_next = ins_now_ms() + 30;          /* 立即开始 */
    ins_beep_on = 0;
}

static void ins_beep_tick(void)
{
    uint32_t now = ins_now_ms();
    if (!ins_beep_enabled)
    {
        return;                       /* 蜂鸣已关, 不启动蜂鸣 */
    }
    if (!ins_beep_on && ins_beep_remain > 0 && now >= ins_beep_next)
    {
        ins_buzzer_on();
        ins_beep_on = 1;
        ins_beep_stop = now + INS_BEEP_MS;
        ins_beep_remain--;
    }
    else if (ins_beep_on && now >= ins_beep_stop)
    {
        ins_buzzer_off();
        ins_beep_on = 0;
        ins_beep_next = now + INS_BEEP_MIN +
                        esp_random() % (INS_BEEP_MAX - INS_BEEP_MIN);
    }
}

/* ================= 对外接口 ================= */
static void ins_presets_load(void);   /* 前置声明(定义在下文) */

void INS_Init(void)
{
    ins_buzzer_init();
    ins_presets_load();
    /* 破译参数 + 当前使用者(NVS "ins2") */
    {
        nvs_handle_t h;
        if (nvs_open("ins2", NVS_READONLY, &h) == ESP_OK)
        {
            nvs_get_u16(h, "def", &INS_SCR_DEFAULT);
            nvs_get_u16(h, "gb", &INS_SCR_GARBLE);
            nvs_get_u16(h, "dl", &INS_SCR_DELAY_MS);
            nvs_get_u16(h, "rv", &INS_REVEAL_DELAY_MS);
            nvs_get_u8(h, "fnt", &ins_font);
            {
                size_t n = sizeof(ins_user);
                if (nvs_get_str(h, "user", ins_user, &n) != ESP_OK || ins_user[0] == '\0')
                {
                    strcpy(ins_user, "李箱");
                }
            }
            nvs_close(h);
        }
        if (ins_font > 2) ins_font = 2;   /* 钳位: 0=16 1=24 2=32px */
    }
}

const char *INS_UserName(void) { return ins_user; }

void INS_SetUserName(const char *name)
{
    /* 按 UTF-8 完整字符截断到缓冲(ins_user[16]): 绝不在多字节字符中间切断,
     * 避免超长使用者名(网页列表允许 23B)被截出非法 UTF-8 尾巴 */
    size_t n = strnlen(name, sizeof(ins_user) - 1);
    size_t used = 0;
    while (used < sizeof(ins_user) - 1)
    {
        uint8_t c = (uint8_t)name[used];
        uint8_t len = (c < 0x80) ? 1 :
                      ((c & 0xE0) == 0xC0) ? 2 :
                      ((c & 0xF0) == 0xE0) ? 3 : 4;
        if (used + len > n || used + len > sizeof(ins_user) - 1) break;   /* 到串尾/超界: 停下 */
        memcpy(ins_user + used, name + used, len);
        used += len;
    }
    ins_user[used] = '\0';
    if (ins_user[0] == '\0') strcpy(ins_user, "李箱");   /* 空回退默认 */
    nvs_handle_t h;
    if (nvs_open("ins2", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_str(h, "user", ins_user);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ================= 随机指令生成器(按当前使用者定制) =================
 * 模板里的 {USER} 会被替换成当前使用者名, 其余 {RAND}/{TIMER}/{TODO} 由破译解析器处理. */
static const char *const ins_gen_templates[] = {
    "致{USER}:明日{RAND:1-23}时{RAND:0-59}分,在第三个路口朝东方挥{RAND:3-7}次手。",
    "致{USER}:把第{RAND:1-99}个与你目光相遇的陌生人记住,直到午夜。",
    "致{USER}:从现在起{RAND:10-60}分钟内不碰任何屏幕。",
    "{TODO}致{USER}:给最近的一株植物浇水,并绕它走三圈。",
    "致{USER}:今晚{RAND:19-23}点前完成一件一直拖延的事。",
    "致{USER}:数自己的心跳{RAND:1-5}分钟,把结果写在纸上。",
    "致{USER}:找一本从未读过的书,翻到第{RAND:1-300}页,大声读第一句。",
    "致{USER}:将{RAND:1-20}枚硬币放进左口袋,直到明天日出。",
    "致{USER}:在没有水的地方抛一次线,等三分钟再收。",
    "致{USER}:对下一个开口说话的人低语“是”。",
};
#define INS_GEN_TEMPLATE_N (sizeof(ins_gen_templates) / sizeof(ins_gen_templates[0]))

static void ins_gen_render(const char *tpl, char *out, size_t outsz)
{
    const char *p = tpl;
    while (*p && outsz > 1)
    {
        if (strncmp(p, "{USER}", 6) == 0)
        {
            size_t ulen = strlen(ins_user);
            size_t cp = (ulen < outsz - 1) ? ulen : (outsz - 1);
            memcpy(out, ins_user, cp);
            out += cp;
            outsz -= cp;
            p += 6;
        }
        else
        {
            *out++ = *p++;
            outsz--;
        }
    }
    *out = '\0';
}

void INS_ShowGenerated(void)
{
    char buf[INS_PRESET_LEN + INS_USER_NAME_MAX + 8];
    ins_gen_render(ins_gen_templates[esp_random() % INS_GEN_TEMPLATE_N],
                   buf, sizeof(buf));
    INS_ShowIns(buf);
}

/* 破译参数读写(WEB 配置用) */
void INS_GetParams(uint16_t *def, uint16_t *gb, uint16_t *dl, uint16_t *rv)
{
    *def = INS_SCR_DEFAULT;
    *gb = INS_SCR_GARBLE;
    *dl = INS_SCR_DELAY_MS;
    *rv = INS_REVEAL_DELAY_MS;
}

void INS_SetParams(uint16_t def, uint16_t gb, uint16_t dl, uint16_t rv)
{
    if (dl < 5) dl = 5;
    if (rv < 10) rv = 10;
    INS_SCR_DEFAULT = def;
    INS_SCR_GARBLE = gb;
    INS_SCR_DELAY_MS = dl;
    INS_REVEAL_DELAY_MS = rv;
    nvs_handle_t h;
    if (nvs_open("ins2", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u16(h, "def", def);
        nvs_set_u16(h, "gb", gb);
        nvs_set_u16(h, "dl", dl);
        nvs_set_u16(h, "rv", rv);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 破译字号读写(0=16 1=24 2=32px; 设置菜单+WEB, 存NVS "ins2"/"fnt") */
uint8_t INS_Font(void) { return ins_font; }

void INS_SetFont(uint8_t f)
{
    if (f > 2) f = 2;
    ins_font = f;
    nvs_handle_t h;
    if (nvs_open("ins2", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u8(h, "fnt", ins_font);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 从 NVS 读指令库; 无则用内置默认 */
static void ins_presets_load(void)
{
    nvs_handle_t h;
    size_t sz = 0;
    char *buf = NULL;
    ins_preset_count = 0;

    if (nvs_open("ins", NVS_READONLY, &h) == ESP_OK)
    {
        if (nvs_get_blob(h, "presets", NULL, &sz) == ESP_OK && sz > 0 && sz <= 8192)
        {
            buf = malloc(sz + 1);   /* +1 放 NUL, 防末行无换行时越界读堆垃圾 */
            if (buf && nvs_get_blob(h, "presets", buf, &sz) == ESP_OK)
            {
                buf[sz] = '\0';     /* 保证以 NUL 结尾, 循环才可安全收尾 */
                char *p = buf, *nl;
                while (*p && ins_preset_count < INS_PRESET_MAX)
                {
                    nl = strchr(p, '\n');
                    if (nl) *nl = '\0';
                    strncpy(ins_presets[ins_preset_count], p, INS_PRESET_LEN - 1);
                    ins_presets[ins_preset_count][INS_PRESET_LEN - 1] = '\0';
                    if (ins_presets[ins_preset_count][0]) ins_preset_count++;
                    if (!nl) break;
                    p = nl + 1;
                }
            }
        }
        if (buf) free(buf);
        nvs_close(h);
    }
    if (ins_preset_count == 0)               /* 无 NVS 数据 -> 内置默认 */
    {
        uint8_t n = (INS_DEFAULT_COUNT < INS_PRESET_MAX) ? (uint8_t)INS_DEFAULT_COUNT : (uint8_t)INS_PRESET_MAX;
        for (ins_preset_count = 0; ins_preset_count < n; ins_preset_count++)
        {
            strncpy(ins_presets[ins_preset_count], ins_defaults[ins_preset_count], INS_PRESET_LEN - 1);
            ins_presets[ins_preset_count][INS_PRESET_LEN - 1] = '\0';
        }
    }
}

/* ================= 指令库对外接口(WEB 配置用) ================= */

/* 返回运行期指令库指针与条数(WEB 用); 二维数组转指针数组, 避免把字符串头当指针 */
const char *const *INS_Presets(uint8_t *count)
{
    static const char *ptrs[INS_PRESET_MAX];
    uint8_t i;
    *count = ins_preset_count;
    for (i = 0; i < ins_preset_count; i++)
    {
        ptrs[i] = ins_presets[i];
    }
    return ptrs;
}

/* 用 '\n' 分隔文本重建指令库并持久化(WEB 保存) */
void INS_PresetsFromText(const char *text)
{
    const char *p = text, *nl;
    uint8_t n = 0;
    while (*p && n < INS_PRESET_MAX)
    {
        size_t len;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > INS_PRESET_LEN - 1) len = INS_PRESET_LEN - 1;
        if (len > 0)
        {
            memcpy(ins_presets[n], p, len);
            ins_presets[n][len] = '\0';
            n++;
        }
        if (!nl) break;
        p = nl + 1;
    }
    ins_preset_count = n;

    nvs_handle_t h;
    if (nvs_open("ins", NVS_READWRITE, &h) == ESP_OK)
    {
        size_t total = 1, o = 0;
        uint8_t i;
        for (i = 0; i < ins_preset_count; i++) total += strlen(ins_presets[i]) + 1;
        char *buf = malloc(total);
        if (buf)
        {
            for (i = 0; i < ins_preset_count; i++)
            {
                size_t l = strlen(ins_presets[i]);
                memcpy(buf + o, ins_presets[i], l); o += l;
                buf[o++] = '\n';
            }
            nvs_set_blob(h, "presets", buf, o);
            nvs_commit(h);
            free(buf);
        }
        nvs_close(h);
    }
}

void INS_SetBeep(uint8_t on)
{
    ins_beep_enabled = on ? 1 : 0;
    if (!ins_beep_enabled && ins_beep_on)
    {
        ins_buzzer_off();             /* 正在响则立即停 */
        ins_beep_on = 0;
    }
}

void INS_Show(const char *text)
{
    if (strncmp(text, "{TODO}", 6) == 0)   /* 待办标记: 同时存入待办列表(去重), 正文去掉标记显示 */
    {
        TODO_Add(text + 6);
        text += 6;
    }
    ins_scr_parse(text);
    ins_reveal_idx = 0;
    ins_scr_frames = 0;
    ins_scr_phase = 0;
    ins_xoff = 0;
    ins_scr_last = ins_now_ms();
    ins_reveal_last = ins_now_ms();
    ins_scr_on = 1;

    /* 解码期间不蜂鸣(语音进行音已由扬声器播, 避免重复); 仅完成时响 1~2 下 */
    ins_beep_remain = 0;
    ins_beep_on = 0;
    ins_beep_next = ins_now_ms() + 100 + esp_random() % 300;

    SOUND_PlayLoop(snd_progress, snd_progress_frames);   /* 乱码翻译进行音循环播放 */

    ins_scr_render();
}

/* 指令显示: 已有"致X:"收件人原样; 无则自动加"致{当前使用者}:"(先处理{TODO}标记) */
void INS_ShowIns(const char *text)
{
    char buf[INS_PRESET_LEN + INS_USER_NAME_MAX + 4];
    const char *user = ins_user;
    if (strncmp(text, "{TODO}", 6) == 0)   /* 待办标记: 先存待办, 正文再去掉标记 */
    {
        TODO_Add(text + 6);
        text += 6;
    }
    if (strncmp(text, "致", 3) == 0)       /* 已有收件人 -> 原样 */
    {
        INS_Show(text);
        return;
    }
    if (user[0])
    {
        snprintf(buf, sizeof(buf), "致%s:%s", user, text);
        INS_Show(buf);
        return;
    }
    INS_Show(text);
}

void INS_ShowByIndex(uint8_t idx)
{
    if (ins_preset_count == 0)
    {
        return;
    }
    if (idx >= ins_preset_count)
    {
        idx = 0;
    }
    INS_ShowIns(ins_presets[idx]);   /* 神谕指令: 无"致X:"自动加致当前使用者 */
}

/* 特殊代行者「浮士德/里恩」的专属指令池; 其他使用者返回 NULL(只走默认库) */
static const char *const *ins_user_pool(uint8_t *count)
{
    *count = 0;
    if (strcmp(ins_user, "浮士德") == 0)
    {
        *count = INS_USER_FAUST_N;
        return ins_user_faust;
    }
    if (strcmp(ins_user, "里恩") == 0)
    {
        *count = INS_USER_RIEN_N;
        return ins_user_rien;
    }
    return NULL;
}

void INS_ShowRandom(void)
{
    uint8_t uc = 0;
    const char *const *upool = ins_user_pool(&uc);
    if (upool && uc > 0)                          /* 浮士德/里恩特殊: 有且仅有专属指令 */
    {
        INS_ShowIns(upool[esp_random() % uc]);
        return;
    }
    if (ins_preset_count == 0 || (esp_random() % 3) == 0)   /* 库空或 1/3 概率: 模板现场生成 */
    {
        INS_ShowGenerated();
        return;
    }
    INS_ShowByIndex(esp_random() % ins_preset_count);
}

void INS_Tick(void)
{
    if (!ins_scr_on)
    {
        return;
    }
    ins_scr_step();
}

/* 蜂鸣推进(解码随机哔/完成提示/倒计时提示), 由 UI 主任务每循环调用 */
void INS_BeepTick(void)
{
    ins_beep_tick();
}

/* 独立蜂鸣 n 下(倒计时结束等), 走同一套 beep_tick 调度 */
void INS_BeepTimes(uint8_t n)
{
    ins_beep_remain = n;
    ins_beep_next = ins_now_ms() + 30;
    ins_beep_on = 0;
}

uint8_t INS_Finished(void)
{
    return (ins_scr_on && ins_scr_phase == 2);
}

void INS_Exit(void)
{
    ins_scr_on = 0;
    if (ins_beep_on)
    {
        ins_buzzer_off();
        ins_beep_on = 0;
    }
    SOUND_Stop();          /* 停破译音效(进行音/结束音) */
    UI_RenderScreen();
}
