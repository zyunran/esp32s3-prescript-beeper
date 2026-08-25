/* GACHA 组件: 抽卡界面(主菜单"观测"直接进入, 子菜单: 十连/拼点/单抽/积分/图鉴/退出)
 * 非阻塞状态机(由 RTOS 主任务驱动):
 *   - GC_MENU:   十连/拼点/单抽/积分/图鉴/退出 子菜单(复用 UI 通用子菜单)
 *   - GC_ANIM:   竖线从左到右扫过 10 个无色方框并逐个染成该抽稀有度色(时间戳驱动)
 *   - GC_VOICE:  金人格抽取语音打字机显示(时间戳驱动 + 按键翻页/加速)
 *   - GC_RESULT: 滚动列表, 上下键一次滑动一位, 确认回子菜单
 * 稀有度: 人格按官方灯级 ★1灰/★2红/★3金, EGO 统一金; 概率千分比可调
 * 绘制使用 UI 组件帧缓冲接口(UI_ScrClear/UI_ScrRect/UI_ScrGlyph/UI_ScrBlit)。
 */
#include "GACHA.h"
#include "LCD.h"   /* 仅用 LCD_WIDTH/HEIGHT 屏幕几何宏(显式声明驱动依赖) */
#include "UI.h"
#include "INSTRUCTION.h"
#include "SOUND.h"
#include "snd_effects.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stddef.h>   /* offsetof(_Static_assert 用) */
#include <string.h>
#include <stdio.h>

#define GACHA_TEN_N      10     /* 十连次数 */
#define GACHA_POINTS_PULL 10    /* 积分抽: 10积分抽一次 */
#define GACHA_MENU_CNT   6      /* 子菜单项数: 十连/拼点/单抽/积分/图鉴/退出 */

/* 事件码与 main.c 的 EVT_UP/EVT_OK/EVT_DOWN 一致 */
#define GC_EVT_UP        1
#define GC_EVT_OK        2
#define GC_EVT_DOWN      3

typedef enum { GC_MENU, GC_ANIM, GC_VOICE, GC_RESULT, GC_COIN, GC_SCORE, GC_CODEX } gacha_phase_t;

static gacha_phase_t gc_phase = GC_MENU;
static uint8_t gc_busy = 0;
static uint8_t gc_menu_cur = 0;   /* 子菜单光标记忆(子流程返回后保持选中项) */

static uint32_t gacha_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint16_t gacha_color(uint8_t rar)
{
    switch (rar)
    {
        case GACHA_RAR_GRAY: return GACHA_COLOR_GRAY;
        case GACHA_RAR_RED:  return GACHA_COLOR_RED;
        default:             return GACHA_COLOR_GOLD;   /* 金人格 & EGO */
    }
}

/* ================= 抽卡核心 ================= */
static uint8_t gacha_roll_rarity(void)
{
    uint16_t r = esp_random() % 1000;
    if (r < GACHA_PCT_EGO)  return GACHA_RAR_EGO;
    r -= GACHA_PCT_EGO;
    if (r < GACHA_PCT_GOLD) return GACHA_RAR_GOLD;
    r -= GACHA_PCT_GOLD;
    if (r < GACHA_PCT_RED)  return GACHA_RAR_RED;
    return GACHA_RAR_GRAY;
}

static const gacha_card_t *gacha_pick(uint8_t rar)
{
    uint16_t cnt = gacha_pool_count[rar];
    if (cnt == 0)
    {
        return NULL;
    }
    return &gacha_cards[gacha_pool_start[rar] + esp_random() % cnt];
}

/* 积分抽/金池: 从统一人格表(coin_skills, 全★3)抽(函数体定义在数据之后) */
static uint16_t gc_last_gold = 0;   /* 最近抽中的金人格在 coin_skills 的下标 */
static const gacha_card_t *gacha_pick_gold(void);

/* ================= 文字工具 ================= */
/* 安全取 UTF-8 字符字节数(1/2/3); 截断/非法返回1(防 s+=3 跳过 '\0' 越界死循环).
 * 罪人名字含 U+00B7"·"(2字节), 旧逻辑一律按 3 字节会错位越界. */
static uint8_t gacha_utf8_len(const char *s)
{
    uint8_t b = (uint8_t)s[0];
    if (b == 0 || b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0 && s[1] != '\0' && (s[1] & 0xC0) == 0x80) return 2;
    if ((b & 0xF0) == 0xE0 && s[1] != '\0' && s[2] != '\0' &&
        (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) return 3;
    return 1;
}

static int16_t gacha_text_w(const char *s)
{
    int16_t w = 0;
    while (*s)
    {
        uint8_t len = gacha_utf8_len(s);
        w += (len > 1) ? 16 : 8;
        s += len;
    }
    return w;
}

static void gacha_text(uint16_t x, int16_t y, const char *s, uint16_t fc, uint16_t bc)
{
    uint16_t xo = 0;
    while (*s)
    {
        char ch[4] = {0};
        uint8_t n = gacha_utf8_len(s);
        memcpy(ch, s, n);
        xo += UI_ScrGlyph(x + xo, y, ch, fc, bc);
        s += n;
    }
}

static void gacha_text_c(int16_t y, const char *s, uint16_t fc, uint16_t bc)
{
    gacha_text((LCD_WIDTH - gacha_text_w(s)) / 2, y, s, fc, bc);
}

/* ================= 十连 ================= */

/* 空框: 白边框 + 黑底(十连中未被扫到的框) */
static void gacha_empty_box(int16_t x, int16_t y)
{
    UI_ScrRect(x, y, GACHA_BOX_SIZE, 1, GACHA_COLOR_BOX);
    UI_ScrRect(x, y + GACHA_BOX_SIZE - 1, GACHA_BOX_SIZE, 1, GACHA_COLOR_BOX);
    UI_ScrRect(x, y, 1, GACHA_BOX_SIZE, GACHA_COLOR_BOX);
    UI_ScrRect(x + GACHA_BOX_SIZE - 1, y, 1, GACHA_BOX_SIZE, GACHA_COLOR_BOX);
}

/* ---- 十连动画状态(GC_ANIM) ---- */
static const gacha_card_t *gc_res[GACHA_TEN_N];
static uint8_t gc_rar[GACHA_TEN_N];
static int16_t gc_pidx[GACHA_TEN_N];    /* 各抽中卡在 coin_skills 的下标(金=下标, 非金=-1) */
static int16_t gc_box_y, gc_bx0;       /* 方框区几何 */
static int16_t gc_lx, gc_line_end;     /* 扫描线当前位置/终点 */
static uint32_t gc_anim_last;          /* 上帧时刻 */
static uint32_t gc_hold_end;           /* 扫描完成后停留结束时刻 */
static uint8_t gc_anim_snd[GACHA_TEN_N]; /* 各框是否已播过扫过音(每个框只响一次) */

static void gacha_anim_render(void)
{
    uint8_t i;
    UI_ScrClear(UI_COLOR_BG);
    for (i = 0; i < GACHA_TEN_N; i++)
    {
        int16_t bx = gc_bx0 + i * GACHA_BOX_PITCH;
        if (gc_lx >= bx + GACHA_BOX_SIZE / 2)   /* 竖线扫过该框 -> 染上该抽稀有度色 */
        {
            UI_ScrRect(bx, gc_box_y, GACHA_BOX_SIZE, GACHA_BOX_SIZE, gacha_color(gc_rar[i]));
        }
        else
        {
            gacha_empty_box(bx, gc_box_y);
        }
    }
    UI_ScrRect(gc_lx, 0, 1, LCD_HEIGHT, GACHA_COLOR_LINE);   /* 青色细扫描线 */
    UI_ScrBlit();
}

/* 前置声明(定义在下文) */
static void coin_score_ensure(void);
static void coin_owned_load(void);
static void gacha_enter_menu(void);

static void gacha_start_ten(void);   /* 定义在下文(数据区之后) */

/* 语音阶段(定义在下文, 先声明) */
static void gacha_start_voices(void);
static void gacha_voice_init_card(uint8_t gi);

static void gacha_anim_tick(void)
{
    uint32_t now = gacha_now_ms();
    if (now - gc_anim_last < GACHA_FRAME_MS)
    {
        return;
    }
    gc_anim_last = now;

    if (gc_lx <= gc_line_end)
    {
        gc_lx += GACHA_LINE_STEP;
        if (gc_lx > gc_line_end)
        {
            gc_hold_end = now + GACHA_HOLD_MS;   /* 扫描结束, 开始停留 */
            return;
        }
        {
            uint8_t i;
            for (i = 0; i < GACHA_TEN_N; i++)
            {
                int16_t bx = gc_bx0 + i * GACHA_BOX_PITCH;
                int16_t center = bx + GACHA_BOX_SIZE / 2;
                if (!gc_anim_snd[i] && gc_lx >= center)
                {
                    gc_anim_snd[i] = 1;
                    if (gc_rar[i] == GACHA_RAR_RED)
                        SOUND_Play(snd_ten_red, snd_ten_red_frames);
                    else if (gc_rar[i] == GACHA_RAR_GOLD || gc_rar[i] == GACHA_RAR_EGO)
                        SOUND_Play(snd_ten_gold, snd_ten_gold_frames);
                    else
                        SOUND_Play(snd_ten_gray, snd_ten_gray_frames);
                }
            }
        }
        gacha_anim_render();
    }
    else if ((int32_t)(now - gc_hold_end) >= 0)   /* 差比较: 防 49.7 天回绕误判停摆 */
    {
        gacha_start_voices();                    /* 停留结束, 出结果 */
    }
}

/* ================= 拼点(GC_COIN) =================
 * 硬币拼点小游戏(仿边狱巴士): 双方各持技能(基础点+N枚硬币, 每枚威力±V),
 * 每轮重新掷全部剩余硬币(每枚50%正面), 总点数=基础点+正面数×威力,
 * 大者胜本轮并摧毁对方1号硬币, 平局重掷, 一方硬币耗尽则败, 99轮强制平局.
 * 数据: 12 罪人 × 各点数上限最高 3 人格(取自 指令大全_Prescripts.md 附录五全3★大招表;
 *   上限值 = 基础点 + 硬币数×威力, 负威力按全反面=基础点计).
 * 交互: 选罪人(角色, 子菜单) -> 选该罪人最强人格(含"随机") -> 确认掷一轮(翻转动画后停稳)
 *   -> 再确认结算并掷下一轮, 一方硬币耗尽显示胜负. */

/* 硬币正面图(16×16 RGB565, 大端字节序, 定义于 gacha_coin_img.c) */
extern const uint8_t gacha_coin_img[];
#define COIN_IMG_W     16
#define COIN_IMG_H     16

/* 翻转动画参数 */
#define COIN_FLIP_MS        700           /* 掷币翻转总时长 ms */
#define COIN_FLIP_FRAME_MS  70            /* 翻转帧间隔 ms(全幅交替) */

/* ---- 拼点/金人格统一数据(罪人×全部3★; 含语音, 融合抽卡/拼点/语音) ---- */
typedef struct {
    const char *sinner;  /* 罪人 */
    const char *name;    /* 人格名(选择菜单显示, 完整) */
    const char *voice;   /* 抽取语音(★3; NULL=无) */
    const char *skill;   /* 大招名(战斗显示, 超长自动拆行) */
    uint8_t  base;       /* 基础点 */
    uint8_t  coins;      /* 硬币数 */
    int8_t   power;      /* 硬币威力(负=掷正面反而减点) */
} coin_skill_t;

/* gacha_pick_gold 把 coin_skill_t* 当 gacha_card_t* 用(读 sinner/name):
 * 编译期把关两者前两字段布局一致, 防未来加字段时静默错位(C2) */
_Static_assert(offsetof(coin_skill_t, sinner) == offsetof(gacha_card_t, sinner) &&
               offsetof(coin_skill_t, name)   == offsetof(gacha_card_t, name),
               "coin_skill_t 前两字段布局必须与 gacha_card_t 一致");

/* 罪人名(角色, 12 人) */
static const char *coin_sinners[] = {
    "李箱", "浮士德", "堂吉诃德", "良秀", "默尔索", "鸿璐",
    "希斯克利夫", "以实玛利", "罗佳", "辛克莱", "奥提斯", "格里高尔",
};
#define COIN_SINNER_N   (sizeof(coin_sinners) / sizeof(coin_sinners[0]))
#define COIN_MAX_PER    16           /* 每罪人最多拼点人格数(含余量) */

static const uint8_t coin_sinner_off[COIN_SINNER_N + 1] = { 0, 10, 21, 30, 40, 50, 58, 69, 79, 90, 99, 109, 120 };

static const coin_skill_t coin_skills[] = {
    { "李箱", "蜘蛛巢 食指 父辈", "与其自己去选择人生,不如被人生所选择……*哔哔声*呼。但愿女儿……亦得尝此般人生之乐。", "Furioso-Replica", 3, 9, 3 },
    { "李箱", "绽放E.G.O:: 山茶花", "欢迎。我的内心……是否合你心意?", "血染芬芳", 6, 3, 4 },
    { "李箱", "N公司E.G.O:: 凶弹", "他们将会指点着这以友人之血点缀的褴褛而嘲弄吧。愚不可及。极致的悲剧,唯有自己体会方能真正知其痛苦。", "调整目标射击", 4, 2, 7 },
    { "李箱", "W公司 3级 清扫人员", "我不愿在早班通勤时被打扰……怎么了?", "次元裂隙", 5, 3, 4 },
    { "李箱", "黑兽 - 午 魁首", "君主所期望的鸿园,与小姐所期望的鸿园最终有所交集。哪怕这条道路沾染着鲜血……我也愿代替小姐您,去实现那尚未绽放的梦想。因此……还请小姐……切勿原谅您这愚钝的护卫。", "破阵先锋", 5, 3, 4 },
    { "李箱", "LCE E.G.O:: 次元撕裂者", "啊……感激不尽。我会好好享用的。……前几日我虽因羞涩而婉拒了部门聚餐……但或许能允许我厚着脸皮……参与其中么?", "空间撕裂", 5, 3, 4 },
    { "李箱", "脑叶公司E.G.O:: 庄严哀悼", "我了解你对死亡的看法。暂且放下内心的冲动,再多交谈一会儿吧。", "葬仪师李箱，以上", 4, 4, 3 },
    { "李箱", "六协会 南部3科", "一如既往地,不得不身处一个极度喧闹的空间……哈啊,同僚们为什么就不能把我一个人留在房间里呢?", "剑之所向", 4, 4, 3 },
    { "李箱", "环指 点彩派 学徒", "您能为我带来全新的灵感吗?否则……难以令我提起多少兴致。", "血滴赋彩", 3, 4, 3 },
    { "李箱", "剑契组 杀手", "我熟知以华丽且奇特的方式斩开心脏的方法。", "异面钻刺", 8, 3, 2 },
    { "浮士德", "脑叶公司E.G.O:: 悔恨", "T-01-54,开始对该收容异想体进行工作。", "被解放的暴戾", 3, 3, 5 },
    { "浮士德", "Seven协会 南部4科", "欢迎。在此处受理点单。", "特征剖析", 5, 3, 4 },
    { "浮士德", "剑契组 杀手", "出战前夜定是寂静无声。正巧……也是一轮盈月。", "红梅飘散", 5, 2, 6 },
    { "浮士德", "LCE E.G.O:: 红艳煞", "个体的战斗威胁程度略高。但考虑到其在特定场合中的自毁特性……红焰蛾最终判定,WAW-5。", "渴火", 5, 3, 4 },
    { "浮士德", "黑兽 - 卯 魁首", "此回答便已足够,主公。", "目不能追，耳未可及。", 5, 3, 4 },
    { "浮士德", "食指 苦行者: 【纸条】", "您可能对指令之意抱有好奇心。不过即使有疑问也必须执行指令。指令是绝对的,因此只需执行。", "我将遵照指令将你处决。", 5, 3, 4 },
    { "浮士德", "黎明事务所 收尾人", "黑色的针……非典型的纹样。果然,正如浮士德所料,这次事件的幕后黑手……是那个人。", "迸射", 5, 3, 4 },
    { "浮士德", "多裂纹事务所 代表", "虽然这个计划存在诸多缺陷,但浮士德会让其变得天衣无缝。", "40Y-3充能", 4, 4, 3 },
    { "浮士德", "し协会 东部3科", NULL, "弓闪", 4, 4, 3 },
    { "浮士德", "蜘蛛巢 环指 子辈", "醒了吗,法西娅?为了介绍新朋友,浮士德在等着法西娅呢。如何?身上的肉片相当不错吧?", "受压肉体", 4, 4, 3 },
    { "浮士德", "执柄者", "要与我一起吗……?进行这净化丑恶的大业。", "处刑", 6, 3, 2 },
    { "堂吉诃德", "拉·曼却领 总督", "家族管理是不会出差错的。……毕竟照顾你所抛下的东西是我的责任。", "桑丘变体硬血6式-鞭", 5, 4, 5 },
    { "堂吉诃德", "脑叶公司E.G.O:: 以爱与憎之名", "请看!守护爱与和平之人的故事正被记述于此……哼哼,那不正是吾等魔法少女吗?哦哦……!原来那就是所谓的收尾人吗?", "恶人…在哪儿…？", 20, 3, -6 },
    { "堂吉诃德", "Cinq协会 南部5科科长", "好!就是汝打算同吾决斗吗?", "向您致敬！", 6, 2, 6 },
    { "堂吉诃德", "食指 代行者 绽放E.G.O:: 代行", "目光交汇之人。目光不曾交汇之人。72分钟。击杀。找到了。不必逃窜。因指、指令正指向汝。", "立、立刻执行指令…", 6, 2, 6 },
    { "堂吉诃德", "中指 幼妹", "吾为堂吉诃德!乃中指的幼妹,亦是幼兄可靠的手下!多多关照!", "正义的报复", 4, 4, 3 },
    { "堂吉诃德", "T公司 3级 征收人员", "啊~失礼了。吾乃T公司3级员工,被委派前来进行时间征收工作。对吾来访的原因,想必诸位心中已有定论了吧?", "那位，请止步！", 4, 4, 3 },
    { "堂吉诃德", "Cinq协会 东部3科", "吾正觉得餐食有些寡淡呢~原来是准备了此等乱斗作为佐料,真是出乎吾之意料!", "请您赐教", 4, 4, 3 },
    { "堂吉诃德", "黑兽 - 未", "汝等……当真觉得这天气暑热难耐吗……?吾可是是是是……马上要冻死在这儿了……哈啾!", "破碎角", 4, 4, 3 },
    { "堂吉诃德", "W公司 3级 清扫人员", "各位乘客大家好!!!吾,已然,到来了!!!", "空间撕裂", 1, 5, 2 },
    { "良秀", "N公司E.G.O:: 轻蔑，敬畏", "索性站着死去吧。毕竟你那丑恶视线的鲜度……每分每秒都在下降。", "轻蔑下所倾落的敬畏之视线", 14, 1, 14 },
    { "良秀", "蜘蛛巢之刃", "若如此能将父辈们尽数焚灭、斩断……我会欣然去成为,那将一切试图去遗忘的丑恶之物投入其中,深深沉入、不再浮起的奈落。来吧。所有的我都将在彼处等候着斩断所有的你们。", "空间斩", 10, 1, 12 },
    { "良秀", "脑叶公司E.G.O:: 余香·孤独", "……那个童话的结局就是这样吗?哈。还不如不听完呢。", "无尽的故事", 12, 1, 7 },
    { "良秀", "埃德加家族 首席管家", "迅·正。就凭你也能自称为管家?退下,我来给你示范。迅速,而且正确地……", "迎客之道第2式 折·脖", 4, 2, 7 },
    { "良秀", "脑叶公司E.G.O:: 赤瞳·忏悔", "终于愿意正眼看我了啊。对,我并非你的猎物。", "碎颅", 5, 3, 4 },
    { "良秀", "黑兽 - 卯", "现在还没到规定的时间呢……让我抽完这支高·烟也无妨吧。", "以镌刻诅咒之剑刺穿吧", 5, 3, 4 },
    { "良秀", "黑云会 若众", "什么都挡不住我的刀刃……就连影子我也能斩断。", "乱云", 8, 2, 4 },
    { "良秀", "鸿园的 流浪武者", "未能保护好应当守护之人的我,究竟该斩些什么好呢。你知道答案吗。……是吗,我倒没抱那么大期待。", "勠力同心", 4, 4, 3 },
    { "良秀", "良·派 厨师长", "来啦?要吃个派吗?", "烹杀万物", 3, 4, 3 },
    { "良秀", "W公司 3级 清扫人员", "荒谬。竟然让我去当·保。", "次·魔·空·裂", 3, 4, 2 },
    { "默尔索", "剑契组 头领", "不准备行动吗。嗯。我们正处于不得不落子的局面。", "肉斩", 20, 1, -8 },
    { "默尔索", "拇指 东部 指挥官IIII", "我建议你考虑再三后再和我搭话。我今天拔下的舌头已经够多了。", "超绝猛虎杀击乱斩", 3, 5, 3 },
    { "默尔索", "Dieci协会 南部4科科长", "嗯,这次的晚自习没有人缺勤。重新积累挥发掉的知识固然可取,但也不要忘记学习新的知识。", "燃尽知识", 5, 3, 4 },
    { "默尔索", "Cinq协会 西部3科", "本次采访到此结束。详情请向西部Cinq协会宣传部门确认。", "向您致敬", 5, 3, 4 },
    { "默尔索", "环指 野兽派 学徒", "向着比起功能更优先表现素材原貌的方向。让其表面的粘合染色方式更加突出尸体痉挛。是。我会按照反馈,补全缺陷之处。讲解员大人。", "野兽派-展品呈现", 5, 3, 4 },
    { "默尔索", "拉·曼却领 王子", "在这战败的王国中没有王子的一席之地。……因此他只能亲自登上王座,为王国指引方向。", "令血凝固，缠绕于你我身周。", 4, 4, 3 },
    { "默尔索", "脑叶公司E.G.O:: 黄蜂【变调】", "确认工作开始顺序……完毕。开始进行对个体T-04-50的第38次洞察工作。", "临床试验-粉尘灼热", 4, 4, 3 },
    { "默尔索", "R公司 第四集团军 犀牛队", "准备完毕。我来打头阵。", "犀角突击", 6, 3, 3 },
    { "默尔索", "N公司 大锤", "执柄者之锤在此。", "异端抹杀", 8, 2, 3 },
    { "默尔索", "W公司 2级 清扫人员", "请告诉我目的地吧,我将为您提供指引。", "能源汇流", 5, 4, 2 },
    { "鸿璐", "豆豆帮 帮主", "谁做主把我叫到这里来的?觉得自己承担得起后果吗?", "碎尸万段", 5, 1, 25 },
    { "鸿璐", "句点事务所 代表", "好了,停止射击。确保前方的视野后……再去追踪敌人。", "联合：标记目标", 7, 4, 4 },
    { "鸿璐", "蜘蛛巢 环指 父辈", "让我们一起剖析,你身体中残存的生命脉动吧?", "我们深爱着血与肉", 3, 5, 4 },
    { "鸿璐", "鸿园的君主", "从鸿园的最高处望去……大观园是这样的么?呼呼,那么……我也不必再掩饰了。", "全体黑兽，回应于我", 10, 1, 12 },
    { "鸿璐", "R公司 第四集团军 驯鹿队", "哈……这次是驯鹿最先上场吗?能否请您转告兔子和犀牛,他们要是再不快点赶来,就没有可撕咬的鲜草了哦?", "心神凝聚", 8, 1, 10 },
    { "鸿璐", "K公司 3级 摘除人员", "嗯……我只要把眼前这些人都摘除,就好了吧?", "摘除目标", 8, 2, 4 },
    { "鸿璐", "20区 圣愚", "这里就是杀人现场吧?嗯~这么业余的手法,看来他们的时间非常紧迫啊?", "犯人就是你！", 3, 4, 3 },
    { "鸿璐", "Dieci协会 南部4科", "嗯~原来是这么写的啊~", "循环的知识", 5, 3, 3 },
    { "希斯克利夫", "句点事务所 收尾人", "装填一枚子弹。型号是……逻辑工作室制高速粉碎弹。好,狙击准备完成。", "逻辑工作室制 高速粉碎弹", 15, 1, 18 },
    { "希斯克利夫", "狂猎", "我又回来了。去见凯瑟琳……和宅邸里那些要被我撕裂的家伙们。", "悲叹、哀恸、破灭吧", 31, 2, -13 },
    { "希斯克利夫", "中指 幼兄", "嗬哟!哈啊啊……没劲。啥,疼吗?那就别做会惹上仇怨的事啊……嗯?", "全员，处刑！！", 16, 1, 10 },
    { "希斯克利夫", "脑叶公司E.G.O:: 狐雨", "……滚一边去。别故意在我眼前晃悠。", "开！", 18, 3, -7 },
    { "希斯克利夫", "Öufi协会 南部3科", NULL, "宣告执行", 5, 3, 4 },
    { "希斯克利夫", "黑云会 若众", "帮派们只是聚在一起开这种无聊的会议……到底什么时候才是决战啊?", "雷鸣斩", 5, 3, 4 },
    { "希斯克利夫", "黑兽 - 酉 魁首", "咕……终于收到命令了!全体斗鸡,亮出利爪!今晚都给我打个痛快!直到沙地上的食饵一粒不剩……!", "血天下鸡舞乱刀", 5, 4, 3 },
    { "希斯克利夫", "蜘蛛巢 拇指 子辈", "……她给了曾经粗俗又低贱的我住处和食物。甚至教了我一辈子连做梦都不敢想的……厉害剑术。我懂。我都懂……可是我……难道要这样过一辈子吗?", "猎人", 5, 3, 4 },
    { "希斯克利夫", "裴廓德号 鱼叉手", "绳子。一定紧紧抓住。……如果不想沉下去。", "切断绳结", 4, 4, 3 },
    { "希斯克利夫", "R公司 第四集团军 兔子队", "嘻嘻哈哈……蹦蹦跳跳……兔子跃来,粉碎一切!", "快速压制", 3, 5, 2 },
    { "希斯克利夫", "W公司 4级 清扫人员 - CCA", NULL, "空间撕裂-CCA过载", 3, 4, 2 },
    { "以实玛利", "R公司 第四集团军 驯鹿队", "呼,走形式的部分就省略掉吧。我该干什么?", "精神鞭挞", 2, 4, 6 },
    { "以实玛利", "家主候选人", NULL, "赤春", 8, 1, 14 },
    { "以实玛利", "臼齿修船厂 收尾人", "哈啊,请不要靠近。我还有好多螃蟹要捕获。还有……你可能会溅到螃蟹的内脏。", "激进判断", 3, 3, 5 },
    { "以实玛利", "Zwei协会 西部3科", "闭上嘴排好队。我可不希望晨会在训斥中度过。", "保护", 5, 3, 4 },
    { "以实玛利", "黑云会 副会长", "有事找我的话请过会再来吧。我清洁刀刃时不想被人打扰。", "墨染", 5, 3, 4 },
    { "以实玛利", "定事务所 代表", "桃李不言下自成蹊……人们都这么说。哈,我们这的花倒是下自成血。", "樱闪", 5, 3, 4 },
    { "以实玛利", "LCD 现场推理小队", "集体发生的扭曲都有着共同的……共同的……啧……确实是在手册上见过的内容来着……共同的……啊……忘记了。", "清潾火", 5, 3, 4 },
    { "以实玛利", "裴廓德号 船长", "欢迎登船,新来的。那副结实的身子,跟着你带来了吧!", "执着鱼叉", 4, 4, 3 },
    { "以实玛利", "蜘蛛巢 中指 子辈", "哈啊,老妈!不是说了进房间的时候要敲门吗!不不,不是说不想让你进来……咱只是在翻《月刊收尾人》啦。……不忙的话,老妈也来一起看看下次杀谁吧?", "紫罗兰破坏猛击", 4, 4, 3 },
    { "以实玛利", "六协会 南部4科", "您是在叫我吗?嗷,烫……好的,需要我做什么吗?", "里门顶肘", 3, 4, 3 },
    { "罗佳", "黑云会 若众", "我来砍东西了。不论活人,流水,浮云,还是人心。", "晴空斩", 7, 1, 18 },
    { "罗佳", "脑叶公司E.G.O:: 泪锋之剑", "我剩下的,只有自己是第一个负责你的人这点空洞的自尊了……到头来你却选择了……那个人?", "褪色的信条", 20, 3, -5 },
    { "罗佳", "蜘蛛巢 拇指 父辈", "……我已经等得够久了。本来以为终于能摆脱这冻死人的鬼地方了。结果就因为那个臭小鬼,券券就变成废纸了?哈……哈哈……说梦话也该有个限度吧。", "解兔", 5, 5, 3 },
    { "罗佳", "拉·曼却领 公主", "欢迎来到拉·曼却领的嘉年华。……要与我一同,参加这永无止尽的游行吗?", "堂吉诃德派硬血奥义 落幕", 4, 3, 5 },
    { "罗佳", "玫瑰扳手工坊 代表", "你好你好~这里是玫瑰扳手工坊代表,罗佳哦~", "大干一场吧", 5, 3, 4 },
    { "罗佳", "Девять协会 北部3科", NULL, "波鲁！加把劲！", 5, 3, 4 },
    { "罗佳", "黑兽 - 巳", "被交代的任务我全都完成了……也已经尽忠尽职了……为什么……不是我?", "绝命巳乱", 5, 3, 4 },
    { "罗佳", "环指 野兽派 讲解员", "各位嘴上的评价是粗糙而野蛮,为什么却不挪开视线呢?就像是着了迷一样。", "令你皮肤下搏动着的那赤红色彩更加狂野", 5, 4, 3 },
    { "罗佳", "Dieci协会 南部4科", "嘘……!快来点这个。别声张,要细嚼慢咽哦。不用谢啦,呼呼。", "苦痛的启蒙", 4, 4, 3 },
    { "罗佳", "六协会 南部4科科长", "购物果然是世界上最棒的消遣~好啦!你这次想去吃点啥?", "一点突破", 4, 4, 3 },
    { "罗佳", "R公司 第四集团军 驯鹿队", NULL, "精神鞭挞-“啊哈哈…看吧，我们都是圣诞的烟花！”", 4, 4, 3 },
    { "辛克莱", "准执柄者", "全部,焚烧殆尽吧……连同我那令人作呕的人生,也一并烧却。", "自我毁灭的净化", 30, 3, -12 },
    { "辛克莱", "黎明事务所 收尾人", "偶尔我的手也会颤抖,希望不会被当成胆小鬼。", "爆炎一击", 13, 1, 15 },
    { "辛克莱", "剑契组 杀手", "我……该去砍什么?", "骨断", 8, 1, 18 },
    { "辛克莱", "蜘蛛巢 小指 子辈", "小生仍然是不足以继承星名之人。为了终将来临的那日,小生在辅佐主人的同时也不懈于磨砺自身。", "落星一杀", 8, 1, 14 },
    { "辛克莱", "Cinq协会 南部4科科长", "啊、啊!我很抱歉!我、我有点太过分了……?!", "反攻", 5, 3, 4 },
    { "辛克莱", "Девять协会 北部3科", NULL, "波鲁德尼察…拜托您了！", 5, 3, 4 },
    { "辛克莱", "中指 幼弟", "既然今天的处理对象都已被划去……差不多,该到期待已久的抓理发券小偷时间了。去和大哥说一声。", "铭刻于心", 5, 3, 4 },
    { "辛克莱", "拇指 东部 士兵II", "我在倾听命令。只是……不胜惶恐,为避免与上级眼神接触,将视线垂下而已。", "推力集中", 5, 3, 4 },
    { "辛克莱", "黑兽 - 酉", "主公已经下令。终于……是时候刺穿并拔除群聚于鸿园的害虫了。", "血炎乱舞", 4, 4, 3 },
    { "奥提斯", "脑叶公司E.G.O:: 魔弹", "不要踏入我的火线。除非你想成为这寄宿着恶魔的子弹的靶子。", "魔弹射击", 15, 1, 4 },
    { "奥提斯", "蜘蛛巢 中指 父辈", "闺女!还记得老妈跟你说过的吧?要是有什么让你不顺心的!随时告诉我!", "剖开你的肚子", 4, 5, 3 },
    { "奥提斯", "LCA 瓦吉特 先锋三队 队长", "电磁拦截器故障。紧急屏蔽合金……故障。步枪压力调节装置……哈。这个也是故障。", "先锋了结", 6, 3, 4 },
    { "奥提斯", "黑兽 - 卯", "兔子没有名字。我不过是,那虚无缥缈之影中的野兽而已。", "刻咒杀剑", 5, 3, 4 },
    { "奥提斯", "T公司 3级 强制征收人员", "辩护时间为31秒。以T公司中央钟楼为基准。不允许超时。", "T3型征收铁锤最大化展开", 5, 3, 4 },
    { "奥提斯", "臼齿事务所 收尾人", "啊……什么?我脑袋晕乎乎的,你说慢点呗。", "当机立断", 4, 4, 3 },
    { "奥提斯", "呼啸山庄 首席管家", "鞋上的泥都清理干净了吗?事先声明,在这栋宅邸里,没有我的许可,就算家具也不许碰。", "遵夫人之命", 4, 4, 3 },
    { "奥提斯", "W公司 3级 清扫组长", "开始列车清扫任务。把所有乘客安置到原位。", "次元撕裂", 4, 4, 3 },
    { "奥提斯", "拉·曼却领 理发师", "来得正好。现在我会亲自给你们展示什么才叫真正的打扮……敬请期待。", "给你做条新裙子！", 3, 4, 3 },
    { "奥提斯", "Seven协会 南部6科科长", "嗯,我是奥提斯。你来拜访我的理由是?", "要害勘破", 6, 3, 2 },
    { "格里高尔", "黑兽 - 巳", "这几乎就是人类的胳膊嘛。我还以为动了手术之后,双臂都会变成蛇呢。", "绝巳臂刺", 8, 1, 14 },
    { "格里高尔", "LCE E.G.O:: AEDD", "好吧,首先欢迎你来到我们研究部。收下这杯吧,我今天打赌输了,所以不得不请所有人每人一杯咖啡。", "最高输出-超高压电流放电", 5, 3, 5 },
    { "格里高尔", "双钩海盗团 大副", "呼……对,我就是双钩海盗团的大副,格里高尔。要不要给你签个名?", "决裂定局", 5, 2, 6 },
    { "格里高尔", "拉·曼却领 神父", "即使我替父亲宽恕他们……他也不会赦免我吧。", "无法洗清的罪", 5, 3, 4 },
    { "格里高尔", "炎拳事务所 幸存者", "那是……我们炎拳事务所的制服。", "炎拳", 5, 3, 4 },
    { "格里高尔", "脑叶公司E.G.O:: 目灯", "与世间普遍认知不同,所谓的下一次机会并不会轻易到访。所以我打算在这次就做些什么。否则迟来的后悔留给我的,就只会剩下丑陋的感情了。", "给予被灯所眩惑之人以救赎", 5, 3, 4 },
    { "格里高尔", "黎明事务所 代表", "哦……是双和茶吗?今天的茶特别香浓,我很喜欢。这次的蛋黄也完美地浮在了正中心,对吧?", "黎明将至", 5, 4, 3 },
    { "格里高尔", "埃德加家族 继承人", "你也是来猎杀那混蛋的吗?还是说你只是想……来见证这份没落。", "噩梦狩猎", 4, 4, 3 },
    { "格里高尔", "夜锥组 队长", "等到这场雨下完,就该永远离开这条后巷了。呼……我还想继续活下去。", "处决", 4, 4, 3 },
    { "格里高尔", "Zwei协会 南部4科", "啊!哎哟,这位老板……能借个火吗?", "守护者", 5, 3, 3 },
    { "格里高尔", "G公司 科长代理", "报告!我是接到入队命令的格里高尔。很荣幸能够和各位并肩作战!", "开膛破腹", 4, 4, 2 },
};
#define COIN_SKILL_N (sizeof(coin_skills) / sizeof(coin_skills[0]))

/* 点数上限值(展示用; 负威力按全反面=基础点计, 取 max(基础点, 基础点+硬币数×威力)) */
static int16_t coin_skill_max(uint8_t idx)
{
    const coin_skill_t *s = &coin_skills[idx];
    return (int16_t)s->base + (s->power > 0 ? (int16_t)s->coins * s->power : 0);
}

/* 积分抽/金池: 从统一人格表(coin_skills, 全★3)抽; 前2字段 sinner/name 与 gacha_card_t 一致可转 */
static const gacha_card_t *gacha_pick_gold(void)
{
    if (COIN_SKILL_N == 0)
    {
        return NULL;
    }
    gc_last_gold = (uint16_t)(esp_random() % COIN_SKILL_N);
    return (const gacha_card_t *)&coin_skills[gc_last_gold];
}

/* 选择菜单项缓冲: 罪人项(12+退出) / 人格项(3+随机, 文字=完整名+上限值) */
static const char *coin_sinner_items[COIN_SINNER_N + 1];
static char coin_sel_buf[COIN_MAX_PER + 1][48];
static const char *coin_sel_items[COIN_MAX_PER + 1];
static uint8_t avail_sinners[COIN_SINNER_N];   /* 已抽中≥1人格的罪人(索引) */
static uint8_t avail_sinner_n;
static uint8_t avail_skills[COIN_MAX_PER];     /* 所选罪人已抽中的人格(coin_skills索引) */
static uint8_t avail_skill_n;
static uint8_t gc_codex_back = 0;              /* 图鉴详情页退出后回图鉴列表 */

/* ---- 拼点状态 ---- */
#define cbm_now_ms  gacha_now_ms
typedef enum { COIN_SINNER, COIN_SEL, COIN_ROLL, COIN_RES } coin_sub_t;
static coin_sub_t gc_coin_sub;
static uint8_t  cbm_me, cbm_op;            /* 技能索引(coin_skills[] 内 0..COIN_SKILL_N-1) */
static uint8_t  cbm_me_c, cbm_op_c;        /* 剩余硬币数 */
static uint8_t  cbm_me_f[9], cbm_op_f[9];  /* 本轮各硬币正反(1=正面) */
static uint8_t  cbm_me_h, cbm_op_h;        /* 本轮正面数 */
static int16_t  cbm_me_p, cbm_op_p;        /* 本轮点数 */
static uint8_t  cbm_round;
static uint8_t  cbm_done;                  /* 0=进行 1=我胜 2=我负 3=平(99轮) */
static uint8_t  cbm_last_res;              /* 上一轮结果: 0=无 1=我胜 2=我负 3=平 */
static uint32_t cbm_flip_until;            /* 翻转动画结束时刻(0=已停稳) */
static uint32_t cbm_flip_last;             /* 翻转帧时刻 */

/* ---- 拼点积分/图鉴跨任务互斥(清单 M2) ----
 * 网页图鉴 GACHA_Coin* 在 httpd 任务读 coin_owned/score, 抽卡在 ui_task 写:
 * 用递归互斥量串行化(内部 ensure/save 互相嵌套, 故用 Recursive). GACHA_Init 在 app_main 创建. */
static SemaphoreHandle_t gacha_mux = NULL;
void GACHA_Init(void) { gacha_mux = xSemaphoreCreateRecursiveMutex(); }
static void gacha_lock(void)   { if (gacha_mux) xSemaphoreTakeRecursive(gacha_mux, portMAX_DELAY); }
static void gacha_unlock(void) { if (gacha_mux) xSemaphoreGiveRecursive(gacha_mux); }

/* ---- 拼点积分(NVS "coin" 持久化) ---- */
static int32_t coin_score_total = 0;       /* 累计总积分(终身, 只增不减) */
static int32_t coin_score_cur = 0;         /* 当前积分(可消费, 积分抽扣除) */
static int32_t coin_score_max = 0;         /* 历史最大单场伤害 */
static int32_t coin_score_streak = 0;      /* 当前连胜(赢一局+1, 输一局清零) */
static int32_t coin_score_streak_max = 0;  /* 历史最高连胜 */
static int32_t coin_dmg_last = 0;          /* 本场伤害(结果屏显示) */
static uint8_t coin_score_loaded = 0;

static void coin_score_load(void)
{
    nvs_handle_t h;
    if (nvs_open("coin", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_i32(h, "total", &coin_score_total);
        nvs_get_i32(h, "cur", &coin_score_cur);
        nvs_get_i32(h, "max", &coin_score_max);
        nvs_get_i32(h, "strk", &coin_score_streak);
        nvs_get_i32(h, "smax", &coin_score_streak_max);
        nvs_close(h);
    }
}

/* 锁内只拍快照, NVS 落盘在锁外做: 防 httpd 侧(图鉴读取)跨任务阻塞在闪存写入上.
 * 注意: 调用方若仍持有 gacha_lock(递归互斥), 本函数的"锁外落盘"不会生效 ——
 * 抽卡/拼点路径已全部把落盘安排在 gacha_unlock() 之后, 勿再挪回锁内. */
static void coin_score_save(void)
{
    int32_t snap[5];
    gacha_lock();
    snap[0] = coin_score_total;
    snap[1] = coin_score_cur;
    snap[2] = coin_score_max;
    snap[3] = coin_score_streak;
    snap[4] = coin_score_streak_max;
    gacha_unlock();
    nvs_handle_t h;
    if (nvs_open("coin", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_i32(h, "total", snap[0]);
        nvs_set_i32(h, "cur", snap[1]);
        nvs_set_i32(h, "max", snap[2]);
        nvs_set_i32(h, "strk", snap[3]);
        nvs_set_i32(h, "smax", snap[4]);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void coin_score_ensure(void)
{
    gacha_lock();
    if (!coin_score_loaded)
    {
        coin_score_loaded = 1;
        coin_score_load();
        coin_owned_load();
    }
    gacha_unlock();
}

/* ---- 已抽中拼点人格(NVS "coin"/owned) ---- */
static uint8_t coin_owned[COIN_SKILL_N];     /* 1=该拼点人格已抽中 */

static void coin_owned_save(void)
{
    uint8_t snap[COIN_SKILL_N];
    gacha_lock();
    memcpy(snap, coin_owned, sizeof(snap));
    gacha_unlock();
    nvs_handle_t h;
    if (nvs_open("coin", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_blob(h, "owned", snap, sizeof(snap));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void coin_owned_load(void)
{
    nvs_handle_t h;
    size_t sz = sizeof(coin_owned);
    memset(coin_owned, 0, sizeof(coin_owned));
    if (nvs_open("coin", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_blob(h, "owned", coin_owned, &sz);
        nvs_close(h);
    }
}

/* 十连: 抽卡并直接标记已抽金人格 */
static void gacha_start_ten(void)
{
    uint8_t i, owned_chg = 0;
    gacha_lock();
    coin_score_ensure();
    for (i = 0; i < GACHA_TEN_N; i++)
    {
        gc_rar[i] = gacha_roll_rarity();
        if (gc_rar[i] == GACHA_RAR_GOLD)
        {
            gc_res[i] = gacha_pick_gold();          /* 金: 从统一人格表 */
            gc_pidx[i] = (int16_t)gc_last_gold;
            if (!coin_owned[gc_last_gold]) { coin_owned[gc_last_gold] = 1; owned_chg = 1; }
        }
        else
        {
            gc_res[i] = gacha_pick(gc_rar[i]);
            gc_pidx[i] = -1;
        }
    }
    gacha_unlock();
    if (owned_chg) coin_owned_save();   /* NVS 落盘在锁外: httpd 图鉴读取不被闪存提交阻塞 */
    gc_box_y  = (LCD_HEIGHT - GACHA_BOX_SIZE) / 2;
    gc_bx0    = (LCD_WIDTH - GACHA_TEN_N * GACHA_BOX_PITCH) / 2;
    gc_lx     = -2;                     /* 屏外左侧进入 */
    gc_line_end = LCD_WIDTH + 2;        /* 完全移出屏外右侧 */
    gc_anim_last = gacha_now_ms();
    gc_phase = GC_ANIM;
    memset(gc_anim_snd, 0, sizeof(gc_anim_snd));
    gacha_anim_render();
}

/* 我方胜利: 剩余硬币重掷(正面+=威力/反面+=0), 总伤害=基础×(1+轮数×3%), 计入积分 */
static void coin_score_battle(void)
{
    int32_t base;
    gacha_lock();
    coin_score_ensure();
    /* 伤害 = 胜利回合实际点数(不再重新随机掷币, 避免 20 点打赢只算 1 点) */
    base = cbm_me_p;
    coin_dmg_last = base * (100 + (int32_t)cbm_round * 3) / 100;
    if (coin_dmg_last < 1) coin_dmg_last = 1;   /* 胜利至少1点, 杜绝"胜伤0" */
    coin_score_total += coin_dmg_last;   /* 累计终身, 只增不减 */
    coin_score_cur += coin_dmg_last;     /* 当前可消费 */
    if (coin_dmg_last > coin_score_max) coin_score_max = coin_dmg_last;
    coin_score_streak++;                 /* 连胜+1 */
    if (coin_score_streak > coin_score_streak_max) coin_score_streak_max = coin_score_streak;
    gacha_unlock();
    coin_score_save();   /* NVS 落盘在锁外(内部锁内快照): 防 httpd 图鉴读取被闪存提交阻塞 */
}

/* 局部水平居中画文字(中心线 cx) */
static void gacha_text_cx(int16_t cx, int16_t y, const char *s, uint16_t fc, uint16_t bc)
{
    gacha_text(cx - gacha_text_w(s) / 2, y, s, fc, bc);
}

/* 前置声明(定义在下文, cbm_roll 先调用) */
static void gacha_coin_render(void);

/* 一枚硬币: 正面=彩色图, 反面=全灰图(灰度); 图内纯黑=透明与UI背景融合 */
static void cbm_coin_draw(int16_t x, int16_t y, uint8_t head)
{
    UI_ScrImage((uint16_t)x, y, COIN_IMG_W, COIN_IMG_H, gacha_coin_img, head ? 0 : 1);
}

/* 掷本轮硬币(每枚50%正面), 计算点数, 进入翻转动画 */
static void cbm_roll(void)
{
    uint8_t i;
    const coin_skill_t *s;
    s = &coin_skills[cbm_me];
    cbm_me_h = 0;
    for (i = 0; i < cbm_me_c; i++)
    {
        cbm_me_f[i] = (uint8_t)(esp_random() & 1);
        cbm_me_h += cbm_me_f[i];
    }
    cbm_me_p = (int16_t)s->base + (int16_t)cbm_me_h * s->power;
    if (cbm_me_p < 1) cbm_me_p = 1;      /* 负威力人格多正面时攻击为负/0 -> 下限1 */
    s = &coin_skills[cbm_op];
    cbm_op_h = 0;
    for (i = 0; i < cbm_op_c; i++)
    {
        cbm_op_f[i] = (uint8_t)(esp_random() & 1);
        cbm_op_h += cbm_op_f[i];
    }
    cbm_op_p = (int16_t)s->base + (int16_t)cbm_op_h * s->power;
    if (cbm_op_p < 1) cbm_op_p = 1;
    cbm_round++;
    cbm_flip_until = cbm_now_ms() + COIN_FLIP_MS;
    cbm_flip_last = cbm_now_ms();
    SOUND_Play(snd_coin, snd_coin_frames);   /* 硬币投掷: 叮叮声 */
    gacha_coin_render();
}

/* 翻转动画推进: 窗口内逐帧重绘(硬币全幅交替), 到时停稳定格真实正反 */
static void cbm_flip_tick(void)
{
    uint32_t now = cbm_now_ms();
    if (cbm_flip_until == 0)
    {
        return;                          /* 已停稳, 等待确认 */
    }
    if ((int32_t)(now - cbm_flip_until) >= 0)   /* 差比较: 防 49.7 天回绕 */
    {
        cbm_flip_until = 0;
        gacha_coin_render();             /* 定格 */
        return;
    }
    if (now - cbm_flip_last >= COIN_FLIP_FRAME_MS)
    {
        cbm_flip_last = now;
        gacha_coin_render();
    }
}

/* 结算本轮(确认键): 比大小, 摧毁输方1号硬币, 判胜负; 未结束则掷下一轮 */
static void cbm_resolve(void)
{
    if (cbm_flip_until != 0)
    {
        return;                          /* 翻转中忽略按键 */
    }
    SOUND_Play(snd_clash, snd_clash_frames);   /* 对拼: 金属碰撞声 */
    gacha_lock();
    if (cbm_me_p > cbm_op_p)      { cbm_last_res = 1; if (cbm_op_c > 0) cbm_op_c--; }
    else if (cbm_me_p < cbm_op_p) { cbm_last_res = 2; if (cbm_me_c > 0) cbm_me_c--; }
    else { cbm_last_res = 3; }                           /* 平局重掷 */
    if (cbm_me_c == 0)
    {
        uint8_t had = (coin_score_streak != 0);
        cbm_done = 2;
        if (had) coin_score_streak = 0;   /* 输: 连胜清零 */
        gc_coin_sub = COIN_RES;
        gacha_unlock();
        if (had) coin_score_save();       /* NVS 落盘在锁外(内部锁内快照): 防 httpd 图鉴读取被闪存提交阻塞 */
        gacha_coin_render();
        return;
    }
    if (cbm_op_c == 0)
    {
        cbm_done = 1;
        gc_coin_sub = COIN_RES;
        gacha_unlock();
        coin_score_battle();              /* 加分+落盘: 自身加锁, NVS 落盘锁外 */
        gacha_coin_render();
        return;
    }
    if (cbm_round >= 99) { cbm_done = 3; gc_coin_sub = COIN_RES; gacha_coin_render(); gacha_unlock(); return; }
    cbm_roll();                          /* 下一轮 */
    gacha_unlock();
}

/* 大招名拆行缓冲(战斗显示完整名) */
static char coin_nbuf[2][32];

/* 大招名自动拆行: 超过 maxw 像素拆两行(优先空格处, 否则按字符); 返回行数 1/2 */
static uint8_t coin_split(const char *s, int16_t maxw, const char **l1, const char **l2)
{
    const char *p = s, *splitp = NULL;
    int16_t w = 0;
    uint8_t pos = 0;

    while (*p && pos < (uint8_t)(sizeof(coin_nbuf[0]) - 4))
    {
        uint8_t len = gacha_utf8_len(p);     /* 兼容 2/3 字节 UTF-8(Ö/西里尔/·等), 之前一律3字节会错位 */
        int16_t cw = (*p & 0x80) ? 16 : 8;
        if (pos > 0 && w + cw > maxw)
        {
            break;                       /* 超宽: 切在此处 */
        }
        if (*p == ' ') splitp = p + 1;   /* 记录空格后为候选切点 */
        memcpy(coin_nbuf[0] + pos, p, len);
        pos += len; w += cw;
        p += len;
    }

    if (*p && splitp)                    /* 超宽且有空格: 优先在空格后换行 */
    {
        p = splitp;
        pos = (uint8_t)(splitp - s);
        memcpy(coin_nbuf[0], s, pos);
        while (pos > 0 && coin_nbuf[0][pos - 1] == ' ') pos--;   /* 去行1尾部空格 */
    }
    coin_nbuf[0][pos] = '\0';
    *l1 = coin_nbuf[0];

    if (*p)
    {
        size_t rest = strlen(p);
        if (rest > sizeof(coin_nbuf[1]) - 1) rest = sizeof(coin_nbuf[1]) - 1;
        /* 按完整 UTF-8 字符回退: 防第二行末尾被切出半个汉字(战斗界面乱码) */
        size_t keep = 0, i = 0;
        while (i < rest)
        {
            size_t len = 1;
            unsigned char c = (unsigned char)p[i];
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            if (i + len > rest) break;
            i += len;
            keep = i;
        }
        memcpy(coin_nbuf[1], p, keep);
        coin_nbuf[1][keep] = '\0';
        *l2 = coin_nbuf[1];
        return 2;
    }
    *l2 = NULL;
    return 1;
}

static void gacha_coin_render(void)
{
    uint8_t i;
    char buf[24];
    int16_t pitch;
    const char *l1, *l2;
    UI_ScrClear(UI_COLOR_BG);
    if (gc_coin_sub == COIN_SINNER || gc_coin_sub == COIN_SEL)
    {
        return;                        /* 选择子菜单已由 UI 渲染 */
    }
    /* 双方完整大招名(唯一标识, 超宽自动拆两行) */
    coin_split(coin_skills[cbm_me].skill, 136, &l1, &l2);
    gacha_text_cx(70, 2, l1, UI_COLOR_TIME, UI_COLOR_BG);
    if (l2) gacha_text_cx(70, 18, l2, UI_COLOR_TIME, UI_COLOR_BG);
    coin_split(coin_skills[cbm_op].skill, 136, &l1, &l2);
    gacha_text_cx(214, 2, l1, UI_COLOR_TIME, UI_COLOR_BG);
    if (l2) gacha_text_cx(214, 18, l2, UI_COLOR_TIME, UI_COLOR_BG);
    /* 硬币排(居中): 翻转中全幅交替, 停稳显真实正反; 多枚收窄间距防出屏 */
    pitch = (cbm_me_c > 4) ? 14 : 20;
    for (i = 0; i < cbm_me_c; i++)
    {
        uint8_t head = cbm_me_f[i];
        if (cbm_flip_until) head ^= (uint8_t)((cbm_now_ms() / COIN_FLIP_FRAME_MS) & 1);
        cbm_coin_draw((int16_t)(70 - (int16_t)((cbm_me_c - 1) * pitch) / 2 + i * pitch), 36, head);
    }
    pitch = (cbm_op_c > 4) ? 14 : 20;
    for (i = 0; i < cbm_op_c; i++)
    {
        uint8_t head = cbm_op_f[i];
        if (cbm_flip_until) head ^= (uint8_t)((cbm_now_ms() / COIN_FLIP_FRAME_MS) & 1);
        cbm_coin_draw((int16_t)(214 - (int16_t)((cbm_op_c - 1) * pitch) / 2 + i * pitch), 36, head);
    }
    /* 底部: 点X(左右) + 轮次/结果(中) */
    snprintf(buf, sizeof(buf), "点%d", (int)cbm_me_p);
    gacha_text_cx(70, 56, buf, UI_COLOR_TIME, UI_COLOR_BG);
    snprintf(buf, sizeof(buf), "点%d", (int)cbm_op_p);
    gacha_text_cx(214, 56, buf, UI_COLOR_TIME, UI_COLOR_BG);
    if (gc_coin_sub == COIN_RES)
    {
        if (cbm_done == 1)      /* 我方胜: 显示本场伤害 */
        {
            snprintf(buf, sizeof(buf), "胜 伤%d", (int)coin_dmg_last);
            gacha_text_cx(LCD_WIDTH / 2, 56, buf, UI_COLOR_FRAME, UI_COLOR_BG);
        }
        else
        {
            const char *r = "平";
            uint16_t rc = UI_COLOR_DATE;
            if (cbm_done == 2) { r = "负"; rc = GACHA_COLOR_RED; }
            gacha_text_cx(LCD_WIDTH / 2, 56, r, rc, UI_COLOR_BG);
        }
    }
    else
    {
        snprintf(buf, sizeof(buf), "第%u轮", (unsigned)cbm_round);
        if (cbm_last_res == 1) strcat(buf, " 上胜");
        else if (cbm_last_res == 2) strcat(buf, " 上负");
        else if (cbm_last_res == 3) strcat(buf, " 上平");
        gacha_text_cx(LCD_WIDTH / 2, 56, buf, UI_COLOR_DATE, UI_COLOR_BG);
    }
    UI_ScrBlit();
}

/* 初始化战斗: 双方按技能硬币数开局, 掷第1轮 */
static void cbm_battle_start(void)
{
    cbm_me_c = coin_skills[cbm_me].coins;
    cbm_op_c = coin_skills[cbm_op].coins;
    cbm_round = 0;
    cbm_done = 0;
    cbm_last_res = 0;
    cbm_flip_until = 0;
    gc_coin_sub = COIN_ROLL;             /* 进入对战, cbm_roll 掷第1轮 */
    cbm_roll();
}

/* 显示罪人(角色)选择子菜单: 只列已抽中≥1人格的罪人 + 退出(块左对齐居中) */
static void gacha_show_sinners(void)
{
    uint8_t i, n = 0;
    coin_score_ensure();
    for (i = 0; i < COIN_SINNER_N; i++)
    {
        uint8_t k, has = 0;
        for (k = coin_sinner_off[i]; k < coin_sinner_off[i + 1]; k++)
        {
            if (coin_owned[k]) { has = 1; break; }
        }
        if (has) avail_sinners[n++] = i;
    }
    avail_sinner_n = n;
    if (n == 0)
    {
        INS_Show("先抽卡解锁人格\n再拼点");
        gc_phase = GC_SCORE;            /* 乱码显示, 任意键回观测菜单 */
        return;
    }
    for (i = 0; i < n; i++)
    {
        coin_sinner_items[i] = coin_sinners[avail_sinners[i]];
    }
    coin_sinner_items[n] = "退出";
    UI_SubMenuSetCenterDx(32);                 /* 右移两个字符(32px) */
    UI_SubMenuInitItemsC(coin_sinner_items, n + 1, 2);
    gc_coin_sub = COIN_SINNER;
}

/* 显示所选罪人的已抽中人格(完整名+上限值) + 随机 */
static void gacha_show_skills(uint8_t sinner)
{
    uint8_t k, n = 0;
    for (k = coin_sinner_off[sinner]; k < coin_sinner_off[sinner + 1]; k++)
    {
        if (!coin_owned[k]) continue;
        if (n >= (uint8_t)(COIN_MAX_PER - 2)) break;   /* 预留"随机/退出"两槽, 防越界 */
        avail_skills[n] = k;
        snprintf(coin_sel_buf[n], sizeof(coin_sel_buf[0]), "%s %d",
                 coin_skills[k].name, (int)coin_skill_max(k));
        coin_sel_items[n] = coin_sel_buf[n];
        n++;
    }
    avail_skill_n = n;
    coin_sel_items[n] = "随机";
    coin_sel_items[n + 1] = "退出";   /* 退出回上一级(罪人选择) */
    UI_SubMenuInitItemsC(coin_sel_items, n + 2, 1);
    gc_coin_sub = COIN_SEL;
}

/* 进入拼点: 先选罪人(角色); 无已抽人格则提示先抽卡 */
static void gacha_start_coin(void)
{
    gc_phase = GC_COIN;
    gacha_show_sinners();
}

/* 拼点推进: 掷币翻转动画 */
static void gacha_coin_tick(void)
{
    if (gc_coin_sub == COIN_ROLL)
    {
        cbm_flip_tick();
    }
}

/* ================= 抽取语音(金人格 ★3, 数据已并入 coin_skills[].voice) ================= */

/* 语音按像素宽度自动断行(每行最多16汉字), 返回行数 */
static uint8_t gacha_wrap_voice(const char *voice, char lines[][52], uint8_t max_lines)
{
    uint8_t nlines = 0, blen = 0;
    int16_t w = 0;
    char buf[52];
    while (*voice && nlines < max_lines)
    {
        char ch[4] = {0};
        uint8_t len = gacha_utf8_len(voice);
        uint8_t cw = (len > 1) ? 16 : 8;
        memcpy(ch, voice, len);
        if (w + cw > 256 && blen > 0)          /* 换行 */
        {
            buf[blen] = '\0';
            memcpy(lines[nlines], buf, blen + 1);   /* 含终止符, 无截断警告 */
            nlines++;
            w = 0; blen = 0;
            if (nlines >= max_lines) break;
        }
        memcpy(buf + blen, ch, len); blen += len;
        w += cw;
        voice += len;
    }
    if (blen > 0 && nlines < max_lines)
    {
        buf[blen] = '\0';
        memcpy(lines[nlines], buf, blen + 1);   /* 含终止符, 无截断警告 */
        nlines++;
    }
    return nlines;
}

/* 行字符数(UTF-8) */
static uint16_t gacha_line_chars(const char *line)
{
    uint16_t n = 0;
    while (*line)
    {
        line += gacha_utf8_len(line);
        n++;
    }
    return n;
}

/* 打字机绘制当前页: 前 shown 个字符逐个显示并保留, 其余留白; 颜色可配(fc) */
static void gacha_draw_typing(const char lines[][52], uint8_t nlines, uint8_t page,
                              uint16_t shown, uint16_t fc, uint16_t bc)
{
    uint16_t remain = shown;      /* 还剩 shown 个字符要显示(跨行累计) */
    uint8_t i;
    for (i = 0; i < 3; i++)
    {
        uint8_t li = page * 3 + i;
        const char *line;
        uint16_t x = 14;
        if (li >= nlines) break;
        line = lines[li];
        while (*line && remain > 0)
        {
            char ch[4] = {0};
            uint8_t len = gacha_utf8_len(line);
            uint8_t cw = (len > 1) ? 16 : 8;
            memcpy(ch, line, len);
            gacha_text(x, 20 + i * 20, ch, fc, bc);
            x += cw;
            remain--;
            line += len;
        }
    }
}

/* ---- 语音阶段状态(GC_VOICE) ---- */
static char    gc_v_lines[8][52];    /* 当前金人格语音断行 */
static uint8_t gc_v_nlines;
static char    gc_v_title[64];       /* "罪人·名字" */
static uint8_t gc_v_page, gc_v_pages;
static uint8_t gc_v_done;            /* 1=语音已全部显示, 现显示人格名 */
static uint16_t gc_v_shown, gc_v_page_chars;
static uint32_t gc_v_last;           /* 上帧打字时刻 */
static uint8_t  gc_v_gold_idx;       /* 当前处理的金卡片在 gc_res 中的下标 */
static uint8_t  gc_single = 0;       /* 1=积分单抽(语音播完直接回菜单, 不进结果列表) */

static void gacha_voice_render(void)
{
    uint8_t i;
    UI_ScrClear(UI_COLOR_BG);
    if (gc_v_done)
    {
        gacha_text_c((LCD_HEIGHT - 16) / 2, gc_v_title, GACHA_COLOR_GOLD, UI_COLOR_BG);
    }
    else
    {
        gc_v_page_chars = 0;
        for (i = 0; i < 3; i++)
        {
            uint8_t li = gc_v_page * 3 + i;
            if (li >= gc_v_nlines) break;
            gc_v_page_chars += gacha_line_chars(gc_v_lines[li]);
        }
        gacha_draw_typing(gc_v_lines, gc_v_nlines, gc_v_page, gc_v_shown, GACHA_VOICE_COLOR, UI_COLOR_BG);
    }
    UI_ScrBlit();
}

static void gacha_voice_init_card(uint8_t gi)
{
    const gacha_card_t *c = gc_res[gi];
    const char *voice = (gc_pidx[gi] >= 0) ? coin_skills[gc_pidx[gi]].voice : NULL;
    snprintf(gc_v_title, sizeof(gc_v_title), "%s·%s", c->sinner, c->name);
    if (voice)
    {
        gc_v_nlines = gacha_wrap_voice(voice, gc_v_lines, 8);
        gc_v_pages = (gc_v_nlines + 2) / 3;
        gc_v_done = 0;
    }
    else
    {
        gc_v_nlines = 0;
        gc_v_pages = 0;
        gc_v_done = 1;              /* 无语音: 直接显示人格名 */
    }
    gc_v_page = 0;
    gc_v_shown = 0;
    gc_v_page_chars = 0;
    gc_v_last = gacha_now_ms();
    gacha_voice_render();
}

/* 结果列表(定义在下文, 先声明) */
static void gacha_start_result(void);

/* 从 start 起找下一个抽到且有语音的金人格; 找到则初始化并返回1, 无则返回0 */
static uint8_t gacha_voice_find(uint8_t start)
{
    uint8_t gi;
    for (gi = start; gi < GACHA_TEN_N; gi++)
    {
        const char *v;
        if (gc_rar[gi] != GACHA_RAR_GOLD) continue;
        v = (gc_pidx[gi] >= 0) ? coin_skills[gc_pidx[gi]].voice : NULL;
        if (!v) continue;
        gc_v_gold_idx = gi;
        gacha_voice_init_card(gi);
        return 1;
    }
    return 0;
}

/* 当前人格已播完, 找下一个金人格(跳过当前); 单抽则直接回菜单 */
static void gacha_voice_next(void)
{
    if (gc_single)
    {
        gc_single = 0;
        gacha_enter_menu();
        return;
    }
    if (!gacha_voice_find((uint8_t)(gc_v_gold_idx + 1)))
    {
        gacha_start_result();
    }
}

static void gacha_start_voices(void)
{
    gc_v_gold_idx = 0;
    gc_phase = GC_VOICE;
    if (!gacha_voice_find(0))
    {
        gacha_start_result();
    }
}

/* 打字机推进: 到帧间隔才进一个字符 */
static void gacha_voice_tick(void)
{
    uint32_t now = gacha_now_ms();
    if (gc_v_done || gc_v_shown >= gc_v_page_chars)
    {
        return;                                  /* 等按键 */
    }
    if (now - gc_v_last < GACHA_TYPE_MS)
    {
        return;
    }
    gc_v_last = now;
    gc_v_shown += GACHA_TYPE_STEP;
    if (gc_v_shown > gc_v_page_chars) gc_v_shown = gc_v_page_chars;
    gacha_voice_render();
}

/* 语音阶段按键: 打字中加速本页; 打满翻页; 末页->人格名; 名字显示->下一个/结果 */
static void gacha_voice_key(void)
{
    if (gc_v_done)
    {
        gacha_voice_next();
        return;
    }
    if (gc_v_shown < gc_v_page_chars)
    {
        gc_v_shown = gc_v_page_chars;
        gacha_voice_render();
        return;
    }
    if (gc_v_page < gc_v_pages - 1)
    {
        gc_v_page++;
        gc_v_shown = 0;
        gc_v_last = gacha_now_ms();
        gacha_voice_render();
        return;
    }
    gc_v_done = 1;
    gacha_voice_render();
}

/* ================= 结果列表(GC_RESULT) ================= */
static int16_t gc_roll, gc_target;
static int16_t gc_max_roll;
static uint32_t gc_roll_last;

static void gacha_result_render(void)
{
    char buf[64];        /* 序号+罪人·名字最长~50字节, 防截断 */
    int16_t y_top = (LCD_HEIGHT - GACHA_RESULT_ROWS * GACHA_ROW_H) / 2;
    uint8_t i;
    UI_ScrClear(UI_COLOR_BG);
    for (i = 0; i < GACHA_TEN_N; i++)
    {
        int16_t y = y_top + i * GACHA_ROW_H - gc_roll;
        if (y < 0 || y + 16 > LCD_HEIGHT)
        {
            continue;
        }
        if (gc_res[i])
        {
            snprintf(buf, sizeof(buf), "%d %s·%s", i + 1, gc_res[i]->sinner, gc_res[i]->name);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%d -", i + 1);
        }
        gacha_text(2, y, buf, gacha_color(gc_rar[i]), UI_COLOR_BG);   /* 左对齐, 超宽行不挤到屏幕外 */
    }
    UI_ScrBlit();
}

static void gacha_start_result(void)
{
    gc_roll = 0;
    gc_target = 0;
    gc_max_roll = (GACHA_TEN_N - GACHA_RESULT_ROWS) * GACHA_ROW_H;
    gc_roll_last = gacha_now_ms();
    gc_phase = GC_RESULT;
    gacha_result_render();
}

/* 滚动缓动: 向目标滑近一帧 */
static void gacha_result_tick(void)
{
    uint32_t now = gacha_now_ms();
    if (now - gc_roll_last < GACHA_ROLL_MS)
    {
        return;
    }
    gc_roll_last = now;
    if (gc_roll < gc_target)
    {
        gc_roll += GACHA_ROLL_STEP;
        if (gc_roll > gc_target) gc_roll = gc_target;
        gacha_result_render();
    }
    else if (gc_roll > gc_target)
    {
        gc_roll -= GACHA_ROLL_STEP;
        if (gc_roll < gc_target) gc_roll = gc_target;
        gacha_result_render();
    }
}


/* ================= 子界面菜单(十连/拼点/单抽/积分/图鉴/退出) ================= */
static const char *gacha_menu_items[GACHA_MENU_CNT] = { "十连", "拼点", "单抽", "积分", "图鉴", "退出" };
#define GC_MENU_COIN   1              /* "拼点"项索引 */
#define GC_MENU_PULL   2              /* "积分抽"项索引 */
#define GC_MENU_SCORE  3              /* "积分"项索引 */
#define GC_MENU_CODEX  4              /* "图鉴"项索引 */
#define GC_MENU_EXIT   5              /* "退出"项索引 */

/* 积分抽: 10当前积分直接抽一张★3, 扣当前积分并标记已抽; 走普通出金的语音打字机 */
static void gacha_points_pull(void)
{
    uint8_t owned_new = 0;   /* 本次新抽中人格(锁外落盘) */
    gacha_lock();
    coin_score_ensure();
    if (coin_score_cur < GACHA_POINTS_PULL)
    {
        INS_Show("当前积分不足\n单抽需10积分");
        gc_phase = GC_SCORE;
        gacha_unlock();
        return;
    }
    coin_score_cur -= GACHA_POINTS_PULL;
    gc_res[0] = gacha_pick_gold();                  /* 统一人格表抽★3 */
    gc_pidx[0] = (int16_t)gc_last_gold;
    gc_rar[0] = GACHA_RAR_GOLD;
    if (!coin_owned[gc_last_gold])
    {
        coin_owned[gc_last_gold] = 1;
        owned_new = 1;
    }
    gacha_unlock();
    coin_score_save();          /* NVS 落盘在锁外(内部锁内快照): 防 httpd 图鉴读取被闪存提交阻塞 */
    if (owned_new) coin_owned_save();
    gc_single = 1;
    gc_v_gold_idx = 0;
    gc_phase = GC_VOICE;
    gacha_voice_init_card(0);
}

/* 显示积分页(乱码破译格式: 当前/累计积分/最大伤害/连胜), 任意键返回子菜单 */
static void gacha_score_show(void)
{
    char buf[64];
    coin_score_ensure();
    snprintf(buf, sizeof(buf), "当前积分 %d 累计 %d\n最大伤害 %d\n连胜 %d 最高 %d",
             (int)coin_score_cur, (int)coin_score_total, (int)coin_score_max,
             (int)coin_score_streak, (int)coin_score_streak_max);
    INS_Show(buf);              /* 复用乱码破译 */
    gc_phase = GC_SCORE;
    gc_codex_back = 0;
}

/* 图鉴: 12 罪人分组, 显示已收集/总数; 选中罪人可看收集详情 */
static char codex_buf[COIN_SINNER_N + 1][32];
static const char *codex_items[COIN_SINNER_N + 1];

static void gacha_show_codex(void)
{
    uint8_t i;
    coin_score_ensure();
    for (i = 0; i < COIN_SINNER_N; i++)
    {
        uint16_t own = 0, k, total = GACHA_CoinSinnerCount(i);
        for (k = coin_sinner_off[i]; k < coin_sinner_off[i + 1]; k++)
        {
            if (coin_owned[k]) own++;
        }
        snprintf(codex_buf[i], sizeof(codex_buf[0]), "%s %u/%u",
                 coin_sinners[i], (unsigned)own, (unsigned)total);
        codex_items[i] = codex_buf[i];
    }
    snprintf(codex_buf[COIN_SINNER_N], sizeof(codex_buf[COIN_SINNER_N]), "退出");
    codex_items[COIN_SINNER_N] = codex_buf[COIN_SINNER_N];
    UI_SubMenuSetCenterDx(24);
    UI_SubMenuInitItemsC(codex_items, COIN_SINNER_N + 1, 2);
    gc_phase = GC_CODEX;
    gc_codex_back = 0;
}

static void gacha_enter_menu(void)
{
    UI_SubMenuInitItems(gacha_menu_items, GACHA_MENU_CNT);
    UI_SubMenuSetCur(gc_menu_cur);   /* 恢复上次选中项(单抽/拼点等返回后不掉回十连) */
    gc_phase = GC_MENU;
    gc_codex_back = 0;
}

/* ================= 对外接口(非阻塞) ================= */
void GACHA_Enter(void)
{
    gacha_enter_menu();
    gc_busy = 1;
}

/* 强制退出(OK 长按): 回主界面. 不在此重绘: 由 main.c 随后 ui_pop() 统一重绘一次. */
void GACHA_ForceExit(void)
{
    gc_busy = 0;
    gc_menu_cur = 0;   /* 完整退出: 下次进入从十连开始 */
}

void GACHA_OnEvent(uint8_t evt)
{
    if (!gc_busy)
    {
        return;
    }
    switch (gc_phase)
    {
        case GC_MENU:
            if (evt == GC_EVT_UP)        UI_SubMenuScroll(1);
            else if (evt == GC_EVT_DOWN) UI_SubMenuScroll(-1);
            else if (evt == GC_EVT_OK)
            {
                gc_menu_cur = UI_SubMenuCur();   /* 记忆当前项: 子流程返回后光标保持原位 */
                if (gc_menu_cur == GC_MENU_EXIT)   /* 选"退出" -> 回主界面(下次从头开始).
                                                    * 不在此重绘: 主循环 !Busy() 时由 ui_pop 统一重绘一次, 避免双重重绘 */
                {
                    gc_menu_cur = 0;
                    gc_busy = 0;
                }
                else if (gc_menu_cur == GC_MENU_COIN)   /* 选"拼点" -> 开始拼点 */
                {
                    gacha_start_coin();
                }
                else if (gc_menu_cur == GC_MENU_PULL)   /* 选"积分抽" -> 10积分单抽 */
                {
                    gacha_points_pull();
                }
                else if (gc_menu_cur == GC_MENU_SCORE)  /* 选"积分" -> 显示积分页 */
                {
                    gacha_score_show();
                }
                else if (gc_menu_cur == GC_MENU_CODEX)  /* 选"图鉴" -> 按罪人显示收集率 */
                {
                    gacha_show_codex();
                }
                else                                    /* 选"十连" -> 开始抽取 */
                {
                    gacha_start_ten();
                }
            }
            break;

        case GC_SCORE:                                /* 积分页(乱码): 任意键返回子菜单 */
            INS_Exit();
            if (gc_codex_back) gacha_show_codex();    /* 图鉴详情: 返回图鉴列表 */
            else gacha_enter_menu();
            break;

        case GC_CODEX:                                /* 图鉴列表 */
            if (evt == GC_EVT_UP) UI_SubMenuScroll(1);
            else if (evt == GC_EVT_DOWN) UI_SubMenuScroll(-1);
            else if (evt == GC_EVT_OK)
            {
                uint8_t sel = UI_SubMenuCur();
                if (sel >= COIN_SINNER_N)             /* 退出 -> 回观测主菜单 */
                {
                    gacha_enter_menu();
                }
                else
                {
                    char buf[64];
                    uint16_t own = 0, k, total = GACHA_CoinSinnerCount(sel);
                    for (k = coin_sinner_off[sel]; k < coin_sinner_off[sel + 1]; k++)
                    {
                        if (coin_owned[k]) own++;
                    }
                    snprintf(buf, sizeof(buf), "%s\n已收集 %u/%u\n任意键返回图鉴",
                             coin_sinners[sel], (unsigned)own, (unsigned)total);
                    INS_Show(buf);
                    gc_phase = GC_SCORE;
                    gc_codex_back = 1;
                }
            }
            break;

        case GC_COIN:
            if (gc_coin_sub == COIN_SINNER)              /* 选罪人(角色, 只显示已抽中的) */
            {
                if (evt == GC_EVT_UP) UI_SubMenuScroll(1);
                else if (evt == GC_EVT_DOWN) UI_SubMenuScroll(-1);
                else if (evt == GC_EVT_OK)
                {
                    if (UI_SubMenuCur() >= avail_sinner_n) gacha_enter_menu();   /* "退出" */
                    else gacha_show_skills(avail_sinners[UI_SubMenuCur()]);
                }
            }
            else if (gc_coin_sub == COIN_SEL)            /* 选该罪人已抽中的人格 */
            {
                if (evt == GC_EVT_UP) UI_SubMenuScroll(1);
                else if (evt == GC_EVT_DOWN) UI_SubMenuScroll(-1);
                else if (evt == GC_EVT_OK)
                {
                    uint8_t sel = UI_SubMenuCur();
                    if (sel >= avail_skill_n + 1) { gacha_show_sinners(); return; }  /* "退出": 回罪人选择 */
                    if (avail_skill_n == 0) return;   /* 防御: 该罪人无已抽人格(正常流程不可达) */
                    if (sel >= avail_skill_n) sel = (uint8_t)(esp_random() % avail_skill_n);  /* "随机" */
                    cbm_me = avail_skills[sel];
                    cbm_op = (uint8_t)(esp_random() % COIN_SKILL_N);
                    if (cbm_op == cbm_me) cbm_op = (uint8_t)((cbm_op + 1) % COIN_SKILL_N);
                    cbm_battle_start();
                }
            }
            else if (gc_coin_sub == COIN_ROLL)           /* 手动按键拼点一轮 */
            {
                if (evt == GC_EVT_OK) cbm_resolve();
            }
            else if (gc_coin_sub == COIN_RES)            /* 结果: OK 返回子菜单 */
            {
                if (evt == GC_EVT_OK) gacha_enter_menu();
            }
            break;

        case GC_ANIM:
            break;                          /* 扫描动画期间按键忽略 */

        case GC_VOICE:
            gacha_voice_key();
            break;

        case GC_RESULT:
            if (evt == GC_EVT_UP)
            {
                gc_target -= GACHA_ROW_H;               /* 一次移动一位 */
                if (gc_target < 0) gc_target = 0;
            }
            else if (evt == GC_EVT_DOWN)
            {
                gc_target += GACHA_ROW_H;
                if (gc_target > gc_max_roll) gc_target = gc_max_roll;
            }
            else if (evt == GC_EVT_OK)
            {
                gacha_enter_menu();                     /* 确认返回子菜单 */
            }
            break;
    }
}

void GACHA_Tick(void)
{
    if (!gc_busy)
    {
        return;
    }
    switch (gc_phase)
    {
        case GC_MENU:   break;                          /* 静止, 无需推进 */
        case GC_SCORE:  INS_Tick();          break;      /* 积分页乱码推进 */
        case GC_ANIM:   gacha_anim_tick();  break;
        case GC_VOICE:  gacha_voice_tick(); break;
        case GC_RESULT: gacha_result_tick(); break;
        case GC_COIN:   gacha_coin_tick();  break;
        case GC_CODEX:  break;                          /* 图鉴列表静止 */
    }
}

uint8_t GACHA_Busy(void)
{
    return gc_busy;
}

/* ================= 图鉴/拼点人格表对外接口(网页图鉴用) ================= */
uint16_t GACHA_CoinTotal(void) { return COIN_SKILL_N; }

uint16_t GACHA_CoinOwnedCount(void)
{
    uint16_t i, n = 0;
    gacha_lock();
    coin_score_ensure();
    for (i = 0; i < COIN_SKILL_N; i++)
    {
        if (coin_owned[i]) n++;
    }
    gacha_unlock();
    return n;
}

uint8_t GACHA_CoinOwned(uint16_t idx)
{
    uint8_t r;
    gacha_lock();
    coin_score_ensure();
    if (idx >= COIN_SKILL_N) r = 0;
    else r = coin_owned[idx];
    gacha_unlock();
    return r;
}

const char *GACHA_CoinName(uint16_t idx)
{
    if (idx >= COIN_SKILL_N) return "";
    return coin_skills[idx].name;
}

uint8_t GACHA_CoinSinnerN(void) { return COIN_SINNER_N; }

const char *GACHA_CoinSinnerName(uint8_t i)
{
    if (i >= COIN_SINNER_N) return "";
    return coin_sinners[i];
}

uint16_t GACHA_CoinSinnerOff(uint8_t i)
{
    if (i >= COIN_SINNER_N) return 0;
    return coin_sinner_off[i];
}

uint16_t GACHA_CoinSinnerCount(uint8_t i)
{
    if (i >= COIN_SINNER_N) return 0;
    return coin_sinner_off[i + 1] - coin_sinner_off[i];
}
