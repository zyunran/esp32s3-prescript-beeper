/* RTOS 多任务架构:
 *  - input_task: GPIO 沿检测读按键 -> 事件队列(高优先级, 独立于界面)
 *  - ui_task:    收事件按界面状态机驱动, 并推进各功能(破译/抽卡/计时/神谕推送)
 *  - cloud_task: OneNET MQTT 会话(CLOUD 组件, 内部建), 属性/事件上报 + display_cmd 下发
 *  - 待机: 息屏后 ui_task 进浅睡眠(50ms tick 查按键/闹钟, STANDBY_TICK_US);
 *    路径1 完全按需联网: 唤醒不再自动重连 WiFi, 联网仅由 联网->连接网络 手动开启(会话内校时/天气);
 *    「远程在线」开启时 CLOUD_KeepAlive() 阻止待机断网, 云端持续在线
 * 功能已拆分为组件: UI(菜单/配置+LOOM彩蛋) / INSTRUCTION(破译+蜂鸣+答案) / GACHA(抽卡) / NET(联网天气)
 *   / AUDIO(蜂鸣+音频) / SETTING(设置+NVS+神谕) / TIMER(倒计时/番茄钟/闹钟) / POWER(电源+电量)
 *   / CLOUD(OneNET MQTT) / WEB(配置页) / OTA(双分区升级) / MPU6050(摇动) / DS1302(RTC) / COMMON(公共头)
 * 主菜单标题与子菜单项文字集中配置于 ui_menu_cfg(ui.c), 改那里即可改文字
 * (主菜单序: 神谕/TTL协议/待办/联网/观测/询问/使用者/设置; 联网含 云端开关与 OTA)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "LCD.h"
#include "KEY.h"
#include "UI.h"
#include "INSTRUCTION.h"
#include "GACHA.h"
#include "NET.h"
#include "SOUND.h"
#include "snd_effects.h"
#include "SETTING.h"
#include "TIMER.h"
#include "ALARM.h"
#include "TODO.h"
#include "BATTERY.h"
#include "ORACLE.h"
#include "ANSWER.h"
#include "LOOM.h"
#include "POMODORO.h"
#include "BUZZER.h"
#include "POWER.h"
#include "WEB.h"
#include "CLOUD.h"
#include "ota_drv.h"
#include "MPU6050.h"
#include "DS1302.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_random.h"
#include "evt.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

/* ================= 按键事件(事件码定义在 COMMON/evt.h, 全工程唯一来源) =================
 * 注: 长按 OK=返回上一级; 连发参数改这里即可调整手感 */
/* 长按参数(改这里即可调整手感):
 *   LONG_PRESS_MS  = 判定"长按"的按住时长(超此即长按)
 *   REPEAT_PRESS_MS = 长按连发间隔(上下键按住后每隔此时间滚动一项) */
#define LONG_PRESS_MS   600
#define REPEAT_PRESS_MS 150

/* 开启联网后, 超过此时长仍没连上则判定"未连上"并反馈 */
#define NET_CONNECT_RESULT_MS  12000

/* ================= 界面状态机 ================= */
typedef enum { ST_MAIN, ST_SUB, ST_INS, ST_GACHA, ST_TIMER, ST_ALARM, ST_INFO, ST_TODO, ST_MPU, ST_ASK, ST_LOOM, ST_POMO } ui_state_t;

static QueueHandle_t key_q;
static TaskHandle_t input_task_h = NULL;   /* 按键轮询任务句柄(待机挂起/唤醒恢复, 省电) */
static ui_state_t ui_state = ST_MAIN;
static uint8_t sub_kind;   /* 当前所在子菜单对应的主菜单项索引 */
static char daily_last[8] = {0};   /* 每日签: 上次签到的日期 "MM-DD" */
static uint8_t net_conn_pending = 0;    /* 联网开关 开启后等结果: 连上→"已连接" / 超时→"未连上" */
static uint32_t net_conn_deadline = 0;  /* 结果判定超时时刻(esp_timer ms) */
static uint8_t reset_pending = 0;     /* 初始化确认中: 再按OK清除NVS重启 */
static uint8_t set_info_active = 0;   /* 1=正在"系统信息"翻页页(上下键翻页, 其他键返回) */
static uint8_t ota_info_active = 0;   /* 1=正在"版本更新(OTA)"信息页 */
static uint32_t ota_ui_last = 0;      /* OTA 信息页重绘节流 */

/* ================= 息屏时钟(AOD) ================= */
static uint8_t  aod_active = 0;       /* 1=息屏时钟模式 */
static uint8_t  aod_mode = 0;         /* 0=普通时钟 1=神谕64库信息 */
static char     aod_oracle[INS_PRESET_LEN] = {0};
static uint32_t aod_last_draw = 0;    /* AOD 重绘节流 */
static uint8_t  aod_saved_font = 0xFF; /* 进入神谕64模式前保存的破译字号(0xFF=未保存) */

/* ================= 菜单返回栈 =================
 * 进入子页面/信息页/计时等之前压入当前屏幕, 返回时弹栈还原, 消除各处硬编码的"回哪"关系.
 * 栈帧只存恢复所需的最小信息: 页面状态 / 主菜单子菜单索引 / 子菜单光标 */
#define UI_STACK_MAX  6
typedef struct {
    ui_state_t state;
    uint8_t sub_kind;
    uint8_t cur;
} ui_frame_t;

static ui_frame_t ui_stack[UI_STACK_MAX];
static uint8_t ui_stack_n = 0;

/* ================= 织机彩蛋(LOOM, 已并入 UI 组件) 胶水 =================
 * 「纺织时间/纺织记忆」逻辑在 LOOM(已并入 UI 组件); 主菜单入口已移除,
 * 改为主界面 Konami 手势解锁(摇:上上下下左右左右 / 键:下下上上+OK·LNG·OK·LNG, ST_MAIN 喂 LOOM_Konami).
 * 此处仅保留: 确认动画(MADE IN HEAVEN)定格流程 与 加速时间的显示节流. */
static uint8_t egg_confirm = 0;       /* 彩蛋确认乱码显示中(播完自动回主界面) */
static uint32_t egg_hold_t = 0;       /* 确认定格计时 */
static uint32_t egg_wx_last = 0;      /* 加速态天气随机刷新节流 ms */
static uint32_t egg_draw_last = 0;    /* 加速态时钟整屏重绘节流 ms */

/* 进入"使用者"子菜单并高亮当前使用者名称(实时反映, 如当前为但丁).
 * 列表快照/高亮在 UI 组件锁内完成(UI_UserMenuEnter): 防 httpd 的 UI_UserAdd 与本处迭代并发 */
static void user_submenu_enter(void)
{
    UI_UserMenuEnter(INS_UserName());
    ui_state = ST_SUB;
}

/* 子菜单项索引使用 UI.h 枚举(与 ui_menu_cfg 配置顺序一致, 见 ui.c) */
static void ui_enter_submenu(uint8_t kind, uint8_t cur)
{
    const ui_menu_cfg_t *cfg = &ui_menu_cfg[kind];
    sub_kind = kind;
    if (cfg->fn == UI_FN_SETTING)
    {
        SET_SubmenuEnter();
        UI_SubMenuSetCur(cur);
    }
    else if (cfg->fn == UI_FN_USER)
    {
        user_submenu_enter();   /* 高亮当前使用者 */
    }
    else if (cfg->items)
    {
        UI_SubMenuInitItems(cfg->items, cfg->item_count);
        if (cur < cfg->item_count) UI_SubMenuSetCur(cur);
        if (cfg->fn == UI_FN_NET)   /* 联网/云端/配网开关项: 进入子菜单即把标签刷新为 开/关 实时状态 */
        {
            char nbuf[16];
            snprintf(nbuf, sizeof(nbuf), "联网:%s", NET_SessionOn() ? "开" : "关");
            UI_SubMenuSetItem(UI_NET_CONNECT, nbuf);
            snprintf(nbuf, sizeof(nbuf), "云端:%s", CLOUD_GetOn() ? "开" : "关");
            UI_SubMenuSetItem(UI_NET_CLOUD, nbuf);
            snprintf(nbuf, sizeof(nbuf), "配网:%s", NET_ApOn() ? "开" : "关");
            UI_SubMenuSetItem(UI_NET_AP, nbuf);
        }
    }
    else
    {
        UI_SubMenuInit(cfg->title, cfg->sub_count);
        if (cur <= cfg->sub_count) UI_SubMenuSetCur(cur);
    }
    ui_state = ST_SUB;
}

static void ui_clear_stack(void)
{
    ui_stack_n = 0;
}

static void ui_push(ui_state_t next)
{
    if (ui_stack_n < UI_STACK_MAX)
    {
        ui_stack[ui_stack_n].state = ui_state;
        ui_stack[ui_stack_n].sub_kind = sub_kind;
        ui_stack[ui_stack_n].cur = UI_SubMenuCur();
        ui_stack_n++;
    }
    ui_state = next;
}

static void ui_pop(void)
{
    ui_frame_t f;
    if (ui_stack_n == 0)
    {
        ui_state = ST_MAIN;
        UI_RenderScreen();
        return;
    }
    f = ui_stack[--ui_stack_n];
    if (f.state == ST_SUB)
    {
        ui_enter_submenu(f.sub_kind, f.cur);
    }
    else if (f.state == ST_LOOM)
    {
        sub_kind = f.sub_kind;
        ui_state = ST_LOOM;
        LOOM_Enter();                    /* 平衡页整屏刷新覆盖了织机菜单: 重绘 */
        if (f.cur <= 3) UI_SubMenuSetCur(f.cur);   /* 恢复织机菜单光标(如停在"平衡") */
    }
    else if (f.state == ST_TODO)
    {
        TODO_Enter();
        UI_SubMenuSetCur(f.cur);
        ui_state = ST_TODO;
    }
    else
    {
        sub_kind = f.sub_kind;
        ui_state = f.state;
        if (f.state == ST_MAIN) UI_RenderScreen();
        /* 计时/闹钟/抽卡/平衡等组件内部状态仍在, 直接恢复 */
    }
}

static void ui_to_main(void)
{
    ui_clear_stack();
    ui_state = ST_MAIN;
    UI_RenderScreen();
}

static void ui_enter_main_submenu(uint8_t sel, uint8_t cur)
{
    ui_push(ST_SUB);
    ui_enter_submenu(sel, cur);
}

/* 显示当前 IP(乱码破译, 任意键返回); 供 联网->显示IP 用(手机开配置页需要 IP) */
static void show_ip_screen(void)
{
    char buf[40];
    char ip[16];
    NET_IpStrCopy(ip, sizeof(ip));               /* 拷贝版: 避免与 /api/status 共用静态缓冲时被覆写 */
    snprintf(buf, sizeof(buf), "IP:%s", ip[0] ? ip : "未联网");
    INS_Show(buf);
    ui_push(ST_INS);
}


/* ================= 息屏时钟(AOD) 绘制/交互 ================= */
static void aod_clock_draw(void)
{
    char clk[12];
    char ch[2] = {0, 0};
    uint16_t x, total;
    int16_t y;
    const char *p;

    NET_TimeStrCopy(clk, sizeof(clk));
    UI_ScrClear(UI_COLOR_BG);
    total = (uint16_t)strlen(clk) * 32;          /* 64px 下 ASCII 宽 32px */
    x = (LCD_WIDTH > total) ? (uint16_t)((LCD_WIDTH - total) / 2) : 0;
    y = (int16_t)((LCD_HEIGHT - 64) / 2);

    for (p = clk; *p; p++)
    {
        ch[0] = *p;
        x += UI_ScrGlyphF(x, y, ch, 64, INS_SCR_GARBLE, UI_COLOR_BG);
    }
    UI_ScrBlit();
}

static void aod_oracle_pick(void)
{
    static char buf[INS_PRESET_MAX][INS_PRESET_LEN];
    uint8_t n = 0;
    if (INS_PresetsEx(3, buf, INS_PRESET_MAX, &n) && n > 0)
    {
        strncpy(aod_oracle, buf[esp_random() % n], sizeof(aod_oracle) - 1);
        aod_oracle[sizeof(aod_oracle) - 1] = '\0';
    }
    else
    {
        strcpy(aod_oracle, "_CLEAR.__");
    }
}

static void aod_enter_64(void)
{
    if (aod_saved_font == 0xFF)
    {
        aod_saved_font = INS_Font();
        if (aod_saved_font != 3) INS_SetFont(3);
    }
}

static void aod_leave_64(void)
{
    if (aod_saved_font != 0xFF)
    {
        INS_SetFont(aod_saved_font);
        aod_saved_font = 0xFF;
    }
}

static void aod_show_current(uint8_t glitch, uint8_t replay)
{
    if (aod_mode == 0)
    {
        if (glitch)
        {
            char clk[16];
            NET_TimeStrCopy(clk, sizeof(clk));
            aod_enter_64();
            INS_Show(clk);
        }
        else
        {
            aod_leave_64();
            aod_last_draw = (uint32_t)(esp_timer_get_time() / 1000);
            aod_clock_draw();
        }
    }
    else
    {
        aod_enter_64();
        if (!replay || aod_oracle[0] == '\0') aod_oracle_pick();
        INS_Show(aod_oracle);
    }
}

static void aod_exit(void)
{
    aod_leave_64();
    aod_active = 0;
    if (aod_mode == 1) INS_Exit();   /* 内部已 UI_RenderScreen 回主界面 */
    else UI_RenderScreen();
}

static void aod_handle_key(uint8_t evt, uint32_t now)
{
    if (evt == EVT_UP || evt == EVT_DOWN)
    {
        aod_mode = !aod_mode;
        aod_show_current(1, 0);      /* 左右/上下切换也要乱码过渡 */
    }
    else if (evt == EVT_OK)
    {
        aod_show_current(1, 1);      /* 当前页面重播乱码 */
    }
    else if (evt == EVT_LONG_OK)
    {
        aod_exit();
    }
    PWR_Activity(now);
}

/* ================= 输入任务: 按键沿检测 -> 事件队列 =================
 * 非阻塞轮询 GPIO(20ms 天然消抖).
 *  - 上/下键: 按下沿立即发一次(滚动一项); 按住≥LONG_PRESS_MS 后进入连发,
 *    每 REPEAT_PRESS_MS 重复发一次(快速连续滚动).
 *  - OK键: 按住≥LONG_PRESS_MS 发 EVT_LONG_OK(返回上一级), 短按释放发 EVT_OK.
 * 沿检测: p_x 记录上一轮电平(1=按下); 按下沿=cur&&!prev, 按住中=cur&&prev, 释放沿=!cur&&prev. */
static void input_task(void *arg)
{
    uint8_t p_up = 0, p_ok = 0, p_dn = 0;   /* 上一轮电平(1=按下) */
    uint32_t ok_press_t = 0;                /* OK 按下时刻(0=未按/已发长按) */
    uint32_t up_hold = 0, dn_hold = 0;      /* 上下键按住计时起点 */
    uint8_t  up_rep = 0, dn_rep = 0;        /* 1=已进入连发 */
    for (;;)
    {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t up = (gpio_get_level(UI_KEY_UP)   == 0);
        uint8_t ok = (gpio_get_level(UI_KEY_OK)   == 0);
        uint8_t dn = (gpio_get_level(UI_KEY_DOWN) == 0);

        /* OK: 按下记时刻; 按住超阈值立即发长按(只一次); 短按释放发短按 */
        if (ok && !p_ok)                        ok_press_t = now;        /* 按下沿 */
        else if (ok && p_ok)                                              /* 按住中 */
        {
            if (ok_press_t && now - ok_press_t >= LONG_PRESS_MS)
            {
                uint8_t e = EVT_LONG_OK;
                xQueueSend(key_q, &e, 0);
                ok_press_t = 0;
            }
        }
        else if (!ok && p_ok)                                             /* 释放沿 */
        {
            if (ok_press_t)
            {
                uint8_t e = EVT_OK;
                xQueueSend(key_q, &e, 0);
                ok_press_t = 0;
            }
        }

        /* 上键: 按下沿立即滚, 长按连发 */
        if (up && !p_up)                                                  /* 按下沿 */
        {
            uint8_t e = EVT_UP;
            xQueueSend(key_q, &e, 0);
            up_hold = now;
            up_rep = 0;
        }
        else if (up && p_up)                                              /* 按住中 */
        {
            if (now - up_hold >= LONG_PRESS_MS)
            {
                if (!up_rep) { up_rep = 1; up_hold = now; }
                else if (now - up_hold >= REPEAT_PRESS_MS)
                {
                    up_hold = now;
                    uint8_t e = EVT_UP;
                    xQueueSend(key_q, &e, 0);
                }
            }
        }
        else if (!up && p_up) { up_hold = 0; up_rep = 0; }                /* 释放沿 */

        /* 下键: 同 */
        if (dn && !p_dn)                                                  /* 按下沿 */
        {
            uint8_t e = EVT_DOWN;
            xQueueSend(key_q, &e, 0);
            dn_hold = now;
            dn_rep = 0;
        }
        else if (dn && p_dn)                                              /* 按住中 */
        {
            if (now - dn_hold >= LONG_PRESS_MS)
            {
                if (!dn_rep) { dn_rep = 1; dn_hold = now; }
                else if (now - dn_hold >= REPEAT_PRESS_MS)
                {
                    dn_hold = now;
                    uint8_t e = EVT_DOWN;
                    xQueueSend(key_q, &e, 0);
                }
            }
        }
        else if (!dn && p_dn) { dn_hold = 0; dn_rep = 0; }                /* 释放沿 */

        p_up = up; p_ok = ok; p_dn = dn;
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

/* ================= 事件处理(按当前界面状态) ================= */
/* 按键音: 根据设置播放蜂鸣/短音频(0=关 1=蜂鸣 2=音频 3=双) */
static void play_key_sound(uint8_t evt)
{
    uint8_t mode = SET_KeySound();
    const int16_t *pcm = NULL;
    uint32_t frames = 0;
    if (mode == 0) return;   /* 关 */
    if (evt == EVT_UP || evt == EVT_DOWN)
    {
        pcm = snd_key_up;
        frames = snd_key_up_frames;
    }
    else if (evt == EVT_OK)
    {
        pcm = snd_key_ok;
        frames = snd_key_ok_frames;
    }
    else if (evt == EVT_LONG_OK)
    {
        pcm = snd_key_back;
        frames = snd_key_back_frames;
    }
    if (pcm) SOUND_Play(pcm, frames);   /* 音频(扬声器) */
}

static void on_event(uint8_t evt)
{
    if (evt != EVT_NONE) play_key_sound(evt);
    switch (ui_state)
    {
        case ST_MAIN:
        {
            /* 织机隐藏入口: 摇 上上下下左右左右 / 键 下下上上+OK·LNG·OK·LNG.
             * 前四步完全放行(照常滚动有反馈, 失败零副作用); 完成即进织机.
             * 后四步的 确认/长按OK 被"拦截区"占用: 拦下不开子菜单, 否则序列中断(实测踩坑). */
            uint8_t kres = (evt != EVT_NONE) ? LOOM_Konami(evt) : LOOM_KON_PASS;
            if (kres == LOOM_KON_DONE)
            {
                ui_push(ST_LOOM);
                LOOM_Enter();
                break;
            }
            if (LOOM_KonamiArmed() && (evt == EVT_OK || evt == EVT_LONG_OK)) break;
            if (evt == EVT_UP)        UI_Scroll(1);
            else if (evt == EVT_DOWN) UI_Scroll(-1);
            else if (evt == EVT_OK)
            {
                uint8_t sel = UI_GetSelect();
                const ui_menu_cfg_t *cfg = &ui_menu_cfg[sel];
                switch (cfg->fn)
                {
                    case UI_FN_INS:                  /* 神谕 -> 指令破译 */
                    INS_BeepNext(1);   /* 指令: 结尾三连急促蜂鸣 */
                    INS_ShowRandom();
                    ui_push(ST_INS);
                        break;
                    case UI_FN_ASK:                  /* 询问 -> 答案之书(分类子菜单) */
                        ANS_Enter();
                        ui_push(ST_ASK);
                        break;
                    case UI_FN_GACHA:                /* 观测 -> 直接进抽卡"十连/退出"菜单 */
                        GACHA_Enter();
                        ui_push(ST_GACHA);
                        break;
                    case UI_FN_SETTING:              /* 设置 -> 设置子菜单(项由组件生成) */
                    case UI_FN_NET:                  /* 联网 -> 自定义子菜单 */
                    case UI_FN_TTL:                  /* TTL协议 -> 闹钟/计时/番茄钟/退出 */
                    case UI_FN_USER:                 /* 使用者 -> 名称列表, 高亮当前使用者 */
                    case UI_FN_SUBMENU:              /* 其余 -> 通用子菜单 {title}01..NN */
                        ui_enter_main_submenu(sel, 0);
                        break;
                    case UI_FN_TODO:                 /* 待办 -> 待办列表 */
                        TODO_Enter();
                        ui_push(ST_TODO);
                        break;
                    default:
                        break;
                }
            }
            break;
        }

        case ST_SUB:
            if (evt == EVT_UP)        UI_SubMenuScroll(1);
            else if (evt == EVT_DOWN) UI_SubMenuScroll(-1);
            else if (evt == EVT_LONG_OK)            /* OK 长按: 返回主界面 */
            {
                ui_pop();
            }
            else if (evt == EVT_OK)
            {
                uint8_t sel = UI_SubMenuCur();
                const ui_menu_cfg_t *cfg = &ui_menu_cfg[sub_kind];
                /* 退出项索引: 设置(组件)在末项; 自定义项在末项; 通用子菜单在 sub_count 项 */
                uint16_t exit_idx;
                if (cfg->fn == UI_FN_SETTING)    /* 设置项为空时给不可能命中值, 防 SET_SubmenuCount()-1 下溢 */
                    exit_idx = (SET_SubmenuCount() > 0) ? (uint16_t)(SET_SubmenuCount() - 1) : 0xFFFF;
                else if (cfg->fn == UI_FN_SUBMENU) exit_idx = cfg->sub_count;
                else                             exit_idx = cfg->item_count - 1;
                if (sel == exit_idx)   /* 选"退出" -> 回主界面 */
                {
                    ui_pop();
                }
                else if (cfg->fn == UI_FN_NET && sel == UI_NET_CONNECT)  /* 联网开关: 开/关联网会话 */
                {
                    if (NET_SessionOn())   /* 会话进行中(射频开): 按 = 关闭 */
                    {
                        net_conn_pending = 0;               /* 取消"等结果"挂起 */
                        NET_SessionEnd();
                        INS_Show("已关闭联网");               /* 乱码显示, 任意键返回 */
                        ui_push(ST_INS);
                    }
                    else if (!NET_GetSsid()[0])   /* 从未配过 WiFi(纯 AP 配网模式): 引导配网 */
                    {
                        UI_FullScreen("未配置WiFi", "先开 配网 再连手机设WiFi");
                        ui_push(ST_INFO);                    /* 任意键回联网子菜单 */
                    }
                    else   /* 开启: 立即反馈"正在连接", 结果(已连接/未连上)由 ui_task 挂起回调在同一屏换内容 */
                    {
                        NET_Connect();
                        net_conn_pending = 1;               /* 等结果: 连上→已连接 / 超时→未连上 */
                        net_conn_deadline = (uint32_t)(esp_timer_get_time() / 1000) + NET_CONNECT_RESULT_MS;
                        INS_Show("正在连接…");
                        ui_push(ST_INS);                    /* 任意键可先退回; 结果会自动换在这屏 */
                    }
                }
                else if (cfg->fn == UI_FN_NET && sel == UI_NET_AP)  /* 联网-配网: 开/关配网热点(返回子菜单时标签按 NET_ApOn 重刷) */
                {
                    uint8_t on = NET_ApToggle();
                    if (on)
                    {
                        char m[68];   /* "热点已开 <SSID>/<8位随机密码>": SSID≤32B+密码8B+中文前后缀, 留足余量 */
                        snprintf(m, sizeof(m), "热点已开 %s/%s", NET_GetApSsid(), NET_GetApPass());
                        UI_FullScreen("配网", m);
                    }
                    else
                    {
                        UI_FullScreen("配网", "热点已关");
                    }
                    ui_push(ST_INFO);                   /* 任意键回联网子菜单 */
                }
                else if (cfg->fn == UI_FN_NET && sel == UI_NET_WEATHER)  /* 联网-查看天气 -> 天气逐字破译 */
                {
                    static char wbuf[96];
                    uint8_t wc = NET_WeatherCount(), i;
                    const char *d;
                    size_t wp = 0;   /* 写指针: 带余量, 防 3×31B 天气行超 96B 缓冲 */
                    wbuf[0] = '\0';
                    for (i = 0; i < wc && i < 3; i++)
                    {
                        d = NET_WeatherDayStr(i);
                        if (!d) continue;
                        if (wp >= sizeof(wbuf) - 1) break;
                        {
                            /* 用"实际写入量"推进: snprintf 返回值是"应写长度", 被截断时直接拿来累加会让 wp 虚增(下轮写越界危险) */
                            int r = snprintf(&wbuf[wp], sizeof(wbuf) - wp, "%s%s",
                                             (wp ? "\n" : ""), d);
                            if (r < 0) break;
                            if ((size_t)r >= sizeof(wbuf) - wp)
                            {
                                wp = sizeof(wbuf) - 1;   /* 截断: 写指针归尾并收工 */
                                break;
                            }
                            wp += (size_t)r;
                        }
                    }
                    if (wp == 0)
                    {
                        strcpy(wbuf, "暂无天气数据");
                        wp = strlen(wbuf);
                    }
                    else if (NET_WeatherAge() > 3600)   /* 数据超过1小时: 旧数据附更新时间戳 */
                    {
                        const char *u = NET_WeatherUpdatedStr();
                        if (u && wp < sizeof(wbuf) - 1)
                        {
                            wp += (size_t)snprintf(&wbuf[wp], sizeof(wbuf) - wp, "\n%s", u);
                        }
                    }
                    INS_Show(wbuf);   /* 复用乱码破译: 逐字显示, 与指令一致 */
                    ui_push(ST_INS);
                }
                else if (cfg->fn == UI_FN_NET && sel == UI_NET_IP)  /* 联网-显示IP: 手机开配置页用 */
                {
                    show_ip_screen();   /* "IP:xxx" 或 "未联网"(乱码显示, 任意键返回) */
                }
                else if (cfg->fn == UI_FN_NET && sel == UI_NET_CLOUD)  /* 联网-连接云端: 开/关 OneNET 远程在线 */
                {
                    uint8_t on = CLOUD_GetOn() ? 0 : 1;
                    cloud_cfg_t c;
                    char m[64];
                    CLOUD_SetOn(on);   /* 仅翻转开关(与网页保存同锁), 云任务自动重连/收掉会话 */
                    CLOUD_GetConfig(&c);
                    snprintf(m, sizeof(m), "云端已%s%s", on ? "开启" : "关闭",
                             (on && !(c.pid[0] && c.name[0] && c.key[0])) ? "\n未配三元组(网页填写)" : "");
                    INS_Show(m);       /* 乱码显示, 任意键返回联网子菜单(标签返回时按 CLOUD_GetOn 重刷) */
                    ui_push(ST_INS);
                }
                else if (cfg->fn == UI_FN_NET && sel == UI_NET_UPDATE)  /* 联网-版本更新(OTA): 需网络, v1.15 自设置移入 */
                {
                    if (!ota_drv_configured())
                    {
                        UI_FullScreen("版本更新", "未配置OTA地址");
                        ota_info_active = 0;
                        ui_push(ST_INFO);
                    }
                    else if (ota_drv_busy())
                    {
                        UI_FullScreen("版本更新", "升级正在进行...");
                        ota_info_active = 1;
                        ui_push(ST_INFO);
                    }
                    else
                    {
                        esp_err_t ota_err = ota_drv_start();
                        UI_FullScreen("版本更新",
                                      (ota_err == ESP_OK) ? "开始检查更新..." : "启动升级失败");
                        ota_info_active = (ota_err == ESP_OK);
                        ui_push(ST_INFO);
                    }
                }
                else if (cfg->fn == UI_FN_SETTING)   /* 设置-各项: 循环/切换(组件处理) */
                {
                    if (sel == SET_IDX_INFO)   /* 系统信息(可上下翻页: 系统/签收/战绩) */
                    {
                        set_info_active = 1;
                        SET_ShowInfo();
                        ui_push(ST_INFO);
                    }
                    /* 平衡 自 v1.16 移至 织机->平衡 */
                    else if (sel == SET_IDX_RESET)   /* 初始化: 确认后清NVS重启 */
                    {
                        reset_pending = 1;
                        INS_Show("确认初始化?\n再按OK清除全部并重启");
                        ui_push(ST_INFO);
                    }
                    /* 版本更新(OTA) 自 v1.15 移至 联网->版本更新 */
                    else
                    {
                        SET_SubmenuSelect(sel);
                    }
                }
                else if (cfg->fn == UI_FN_TTL)       /* TTL协议: 锚定时间(闹钟)/跨越时间(倒计时) */
                {
                    if (sel == UI_TTL_FUTURE)       /* 跨越时间 -> 倒计时 */
                    {
                        ui_push(ST_TIMER);
                        TIM_Enter();
                    }
                    else if (sel == UI_TTL_PAST)    /* 锚定时间 -> 闹钟 */
                    {
                        ui_push(ST_ALARM);
                        ALM_Enter();
                    }
                    else if (sel == UI_TTL_POMO)    /* 番茄钟 */
                    {
                        ui_push(ST_POMO);
                        POM_Enter();
                    }
                }
                else if (cfg->fn == UI_FN_USER && cfg->item_count > 0 && sel + 1 < cfg->item_count)  /* 使用者: 选名字(sel+1 防 item_count-1 在 0 时下溢) */
                {
                    INS_SetUserName(cfg->items[sel]);
                    UI_SetUserTitle(cfg->items[sel]);   /* 主菜单标题实时显示当前名(如但丁) */
                    {
                        char ubuf[48];   /* "当前使用者\n"(16B) + 名字(≤23B) */
                        snprintf(ubuf, sizeof(ubuf), "当前使用者\n%s", cfg->items[sel]);
                        INS_Show(ubuf);
                    }
                    ui_push(ST_INFO);
                }
                /* 其余子项: 占位, 后续填充 */
            }
            break;

        case ST_TIMER:
            if (evt == EVT_LONG_OK) { TIM_Exit(); ui_pop(); }   /* OK长按退出回TTL子菜单 */
            else
            {
                /* TTL协议倒计时: 按键交给 TIMER 组件, 完成/退出回 TTL 子菜单 */
                tim_ret_t r = TIM_Key(evt == EVT_UP, evt == EVT_OK, evt == EVT_DOWN);
                if (r == TIM_EXIT) ui_pop();
            }
            break;

        case ST_ALARM:
            ALM_Key(evt == EVT_UP, evt == EVT_OK, evt == EVT_DOWN, evt == EVT_LONG_OK);
            break;

        case ST_TODO:                       /* 待办列表: OK重显示/长按OK=PASS/退出 */
        {
            uint8_t r;
            if (evt == EVT_LONG_OK) r = TODO_Key(0, 0, 0, 1);   /* 长按OK = 标记PASS/恢复 */
            else r = TODO_Key(evt == EVT_UP, evt == EVT_OK, evt == EVT_DOWN, 0);
            if (r == TODO_KEY_SHOW)         /* 选待办: 重新乱码破译显示, 任意键回待办 */
            {
                const char *t = TODO_CurText();
                if (t)
                {
                    ui_push(ST_INS);                    /* 栈内记住待办列表与光标 */
                    INS_ShowIns(t);
                }
            }
            else if (r == TODO_KEY_EXIT)    /* 选"退出": 回主界面 */
            {
                ui_pop();
            }
            break;
        }

        case ST_INFO:                       /* 乱码信息/开启配网/初始化确认页/系统信息翻页 */
            if (ota_info_active)            /* 版本更新页: 任意键返回设置(升级在后台继续) */
            {
                ota_info_active = 0;
                INS_Exit();
                ui_pop();
                break;
            }
            if (reset_pending)              /* 初始化: 物理OK清NVS重启(左摇的OK不算, 防误擦除), 其他键取消 */
            {
                reset_pending = 0;
                if (evt == EVT_OK && !MPU_EvtWasShake())
                {
                    nvs_flash_erase();
                    esp_restart();
                    break;
                }
                INS_Exit();
                ui_pop();
                break;
            }
            if (set_info_active && (evt == EVT_UP || evt == EVT_DOWN))   /* 系统信息: 上下键翻页 */
            {
                SET_InfoNav(evt);
                break;
            }
            if (evt == EVT_OK && !MPU_EvtWasShake() && INS_Decoding())   /* 乱码信息未完成: 物理确认=直接显示原文(左摇不算) */
            {
                INS_FinishNow();
                break;
            }
            set_info_active = 0;
            INS_Exit();
            ui_pop();
            break;

        case ST_MPU:                        /* 平衡: 实时姿态+摇动反馈; 仅物理按键返回设置(摇动的确认/退出被过滤, 保住左右摇演示) */
            if ((evt == EVT_OK || evt == EVT_LONG_OK) && !MPU_EvtWasShake())
            {
                ui_pop();
            }
            break;

        case ST_GACHA:
            if (evt == EVT_LONG_OK) { GACHA_ForceExit(); ui_pop(); }   /* OK 长按: 强制退出回主界面 */
            else GACHA_OnEvent(evt);                     /* 抽卡内部状态机分发 */
            break;

        case ST_LOOM:                         /* 织机彩蛋: 子菜单导航, 动作码决定导航 */
        {
            uint8_t lr = LOOM_Key(evt == EVT_UP, evt == EVT_OK, evt == EVT_DOWN, evt == EVT_LONG_OK);
            if (lr == LOOM_KEY_EXIT) { ui_pop(); }
            else if (lr == LOOM_KEY_TIME)      /* 纺织时间切换: 回主界面并播确认动画 */
            {
                ui_to_main();
                INS_Show(LOOM_TimeOn() ? "MADE IN\nHEAVEN" : "时间恢复");
                egg_confirm = 1;
                egg_hold_t = 0;
                ui_push(ST_INS);
            }
            else if (lr == LOOM_KEY_MEMORY)    /* 白框滤镜已切换: 清栈回主界面立即反映 */
            {
                ui_to_main();
            }
            else if (lr == LOOM_KEY_BAL)       /* 织机->平衡: 进 MPU 姿态页, 页内返回回织机菜单 */
            {
                ui_push(ST_MPU);
            }
            break;
        }

        case ST_POMO:                          /* 番茄钟: 同倒计时模式, 长按OK退出 */
            if (evt == EVT_LONG_OK) { POM_Exit(); ui_pop(); }
            else
            {
                pom_ret_t pr = POM_Key(evt == EVT_UP, evt == EVT_OK, evt == EVT_DOWN);
                if (pr == POM_EXIT) ui_pop();
            }
            break;

        case ST_ASK:
            ANS_OnEvent(evt);                            /* 询问内部处理(含长按OK返回) */
            break;

        case ST_INS:
            if (evt == EVT_OK && !MPU_EvtWasShake() && INS_Decoding())
            {
                INS_FinishNow();     /* 物理确认: 跳过动画直接显示原文(左摇产生的 OK 不算); 显示完后再按键退出 */
                break;
            }
            INS_Exit();                  /* 已显示完(或非确认键): 任意键返回 */
            egg_confirm = 0;             /* 手动按键退出: 取消彩蛋确认自动回主界面 */
            egg_hold_t = 0;
            ui_pop();
            break;
    }
}

/* ================= 电源宿主回调(POWER 组件 pwr_host_t 实现) ================= */
static void stby_net_stop(void)
{
    net_conn_pending = 0;   /* 会话被待机强断: 不再等联网结果(防唤醒后误弹"未连上") */
    NET_WifiStop();         /* 待机关 WiFi(最省电); 唤醒不自动重连(按需联网) */
}
static uint8_t stby_btn_pressed(void)   /* 任一按键按下(低电平) */
{
    return (gpio_get_level(UI_KEY_UP) == 0) ||
           (gpio_get_level(UI_KEY_OK) == 0) ||
           (gpio_get_level(UI_KEY_DOWN) == 0);
}
static uint8_t stby_alarm_due(void)     { return ALM_Check(); }   /* 到点即唤醒(ALM 内已标记当日触发) */
static void stby_sensor_suspend(void)   { MPU_Suspend(); }
static void stby_sensor_resume(void)    { MPU_Resume(); }
static void stby_alarm_wake(void)
{
    CLOUD_NotifyEvent(CLOUD_EVT_ALARM, NULL);   /* 待机中触发的闹钟补云端事件(未启用自动丢弃): 此路径的 ALM_Check 已被待机 tick 消费, ui_task 分支不会再走 */
    ui_to_main();      /* 与亮屏路径一致: 先回主界面再显示闹钟指令 */
    ALM_Show();        /* 闹钟专属指令乱码破译 */
    ui_push(ST_INS);
}
static pwr_host_t s_pwr_host;   /* app_main 建任务后填充并交 PWR_Init */

/* ================= UI 主任务: 收事件 + 推进各功能 + 息屏 + 神谕推送 ================= */
static void ui_task(void *arg)
{
    uint32_t oracle_last = 0;   /* 神谕检查节流 */
    uint32_t bat_last = 0;      /* 电量读取节流 */
    static char ui_user_last[INS_USER_NAME_MAX];   /* 主界面已显示的使用者名(变化才重绘) */
    PWR_Wake((uint32_t)(esp_timer_get_time() / 1000));
    strncpy(ui_user_last, INS_UserName(), sizeof(ui_user_last) - 1);
    ui_user_last[sizeof(ui_user_last) - 1] = '\0';

    for (;;)
    {
        uint8_t evt = EVT_NONE;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        /* 网页下发的指令 -> 乱码破译显示 */
        {
            static char wcmd[96];
            if (WEB_TakeCmd(wcmd, sizeof(wcmd)))
            {
                if (strcasecmp(wcmd, "made in heaven") == 0)   /* 彩蛋指令: 切换时间加速 */
                {
                    LOOM_TimeToggle();
                    ui_to_main();                /* 彩蛋确认播完自动回主界面看时间加速 */
                    INS_Show(LOOM_TimeOn() ? "MADE IN\nHEAVEN" : "时间恢复");
                    egg_confirm = 1;
                    ui_push(ST_INS);
                }
                else
                {
                    if (ui_state != ST_INS && ui_state != ST_INFO)
                    {
                        if (ui_state != ST_MAIN && ui_state != ST_SUB) ui_to_main();   /* 计时/闹钟/抽卡/询问/平衡页先回主界面: 防退出后画面与状态错位 */
                        ui_push(ST_INS);   /* 返回时恢复原页面 */
                    }
                    INS_ShowIns(wcmd);   /* 网页下发: 无"致X:"自动加致当前使用者 */
                }
                PWR_Wake(now);
            }
        }

        /* 云端下发的指令(OneNET display_cmd) -> 与网页指令同一乱码破译路径(含彩蛋同待遇) */
        {
            static char ccmd[CLOUD_CMD_MAX];
            if (CLOUD_TakeCmd(ccmd, sizeof(ccmd)))
            {
                if (strcasecmp(ccmd, "made in heaven") == 0)
                {
                    LOOM_TimeToggle();
                    ui_to_main();                /* 彩蛋确认播完自动回主界面看时间加速 */
                    INS_Show(LOOM_TimeOn() ? "MADE IN\nHEAVEN" : "时间恢复");
                    egg_confirm = 1;
                    ui_push(ST_INS);
                }
                else
                {
                    if (ui_state != ST_INS && ui_state != ST_INFO)
                    {
                        if (ui_state != ST_MAIN && ui_state != ST_SUB) ui_to_main();   /* 计时/闹钟/抽卡/询问/平衡页先回主界面: 防退出后画面与状态错位 */
                        ui_push(ST_INS);
                    }
                    INS_ShowIns(ccmd);
                }
                PWR_Wake(now);
            }
        }

        /* 随机神谕推送 + 闹钟触发 + 每日签: 每秒检查, 到时主界面空闲则推送并唤醒屏幕 */
        if (now - oracle_last >= 1000)
        {
            oracle_last = now;
            /* 每日签: 开机/换日后, 时间有效且主界面空闲, 显示一次"今日指令" */
            if (ui_state == ST_MAIN && NET_TimeOk())
            {
                char dd[8];
                NET_DateStrCopy(dd, sizeof(dd));   /* 拷贝版: 防止与网页状态页并发读到静态缓冲 */
                if (strcmp(dd, "--") != 0 && strcmp(dd, daily_last) != 0)
                {
                    strncpy(daily_last, dd, sizeof(daily_last) - 1);
                    daily_last[sizeof(daily_last) - 1] = '\0';
                    ORACLE_DsignInc();   /* 每日签计数(已收拢至 ORACLE 组件) */
                    CLOUD_NotifyEvent(CLOUD_EVT_DAILY, NULL);   /* 云端事件: 每日神谕已推送 */
                    INS_BeepNext(1);   /* 每日签: 结尾三连急促蜂鸣 */
                    INS_ShowRandom();
                    ui_push(ST_INS);
                    PWR_Wake(now);
                }
            }
            /* 联网开关 开启后等结果: 连上→"已连接" / 超时→"未连上"(正在连接屏就地换内容, 不残留) */
            if (net_conn_pending)
            {
                if (NET_WifiOk())
                {
                    net_conn_pending = 0;
                    if (ui_state != ST_INS)
                    {
                        if (ui_state != ST_MAIN && ui_state != ST_SUB && ui_state != ST_INFO) ui_to_main();
                        ui_push(ST_INS);   /* 已退回就重开一屏, 否则就地换 */
                    }
                    INS_Show("已连接");
                    PWR_Wake(now);
                }
                else if ((int32_t)(now - net_conn_deadline) >= 0)   /* 差比较: 防 uptime 49.7d 回绕误判超时 */
                {
                    net_conn_pending = 0;
                    if (ui_state != ST_INS)
                    {
                        if (ui_state != ST_MAIN && ui_state != ST_SUB && ui_state != ST_INFO) ui_to_main();
                        ui_push(ST_INS);
                    }
                    INS_Show("未连上");
                    PWR_Wake(now);
                }
            }
            if (ORACLE_Due())
            {
                if (aod_active) aod_active = 0;
                ORACLE_Delivered();
                if (ui_state == ST_MAIN)
                {
                    INS_BeepNext(1);   /* 神谕定时推送: 结尾三连急促蜂鸣 */
                    INS_ShowRandom();
                    ui_push(ST_INS);
                    PWR_Wake(now);
                }
            }
            if (ALM_Check())       /* 有闹钟到点(已在 ALM 内标记当日触发): 任意界面都打断显示 */
            {
                if (aod_active) aod_active = 0;
                CLOUD_NotifyEvent(CLOUD_EVT_ALARM, NULL);   /* 云端事件: 闹钟到点(累计计数在 CLOUD 内) */
                ui_to_main();      /* 与待机闹钟唤醒一致: 先回主界面再显示闹钟指令 */
                ALM_Show();        /* 闹钟专属指令乱码破译 */
                ui_push(ST_INS);
                PWR_Wake(now);
            }
            if (TODO_RemindDue())  /* 有待办提醒到点: 亮屏+蜂鸣+乱码显示待办 */
            {
                if (aod_active) aod_active = 0;
                CLOUD_NotifyEvent(CLOUD_EVT_TODO, TODO_RemindText());   /* 云端事件: 待办提醒(msg=待办文本) */
                ui_to_main();
                INS_BeepNext(1);   /* 待办提醒: 结尾三连急促蜂鸣(与破译完成对齐, 替代旧的独立三响) */
                INS_Show(TODO_RemindText());
                ui_push(ST_INS);
                PWR_Wake(now);
            }
        }

        /* 屏亮 20ms 快速响应按键; 屏灭 200ms 慢轮询省电(走秒用 esp_timer 不依赖此轮询, 倒计时到点误差≤200ms) */
        if (xQueueReceive(key_q, &evt, (PWR_ScreenOn() ? 20u : 200u) / portTICK_PERIOD_MS))
        {
            PWR_Activity(now);   /* 任意按键均刷新活动时刻 */
            if (aod_active)       /* 息屏时钟: OK/上下/长按OK 按 AOD 规则, 不透传给菜单 */
            {
                aod_handle_key(evt, now);
                continue;
            }
            if (!PWR_ScreenOn())     /* 屏幕休眠: 第一键仅唤醒(按键本身不透传) */
            {
                PWR_Wake(now);
            }
            else
            {
                on_event(evt);
            }
        }

        /* 倒计时: 无论亮屏与否都推进(render=亮屏才重绘, 熄屏省电);
         * 到点蜂鸣并唤醒屏幕显示"你已到达X分钟后的未来!" */
        if (ui_state == ST_TIMER)
        {
            tim_ret_t r = TIM_Tick(PWR_ScreenOn());
            if (r == TIM_DONE)
            {
                BUZZER_Beep(2);
                PWR_Wake(now);   /* 倒计时结束自动亮屏+记活动 */
            }
            if (r == TIM_EXIT) ui_pop();      /* 完成/退出回 TTL 子菜单 */
        }
/* 番茄钟: 与倒计时一致, 无论亮屏与否都推进; 阶段到点蜂鸣+亮屏 */
        if (ui_state == ST_POMO)
        {
            pom_ret_t pr = POM_Tick(PWR_ScreenOn());
            if (pr == POM_DONE)
            {
                BUZZER_Beep(2);
                PWR_Wake(now);   /* 工作/休息阶段切换: 亮屏提示 */
            }
            if (pr == POM_EXIT) ui_pop();     /* 防御: 退出回 TTL 子菜单 */
        }

        if (PWR_ScreenOn())
        {
            if (aod_active)
            {
                /* 息屏时钟: 只更新 AOD 画面, 不走普通主界面刷新 */
                if (aod_mode == 0)
                {
                    if (INS_Decoding())   /* 时钟乱码破译/逐字显示中 */
                    {
                        INS_Tick();
                    }
                    else
                    {
                        if (aod_saved_font != 0xFF)
                        {
                            INS_SetFont(aod_saved_font);
                            aod_saved_font = 0xFF;
                        }
                        if (now - aod_last_draw >= 1000)
                        {
                            aod_last_draw = now;
                            aod_clock_draw();
                        }
                    }
                }
                else
                {
                    INS_Tick();
                }
            }
            else
            {
                /* 网页保存配置/待办, 或使用者名变化: 当前界面即时刷新
                 * (绘制统一在本任务做, 避免 httpd 任务与 UI 并发;
                 *  INS_UserName 返回共享快照缓冲, 一次性拷到局部串, 防比较/记录/显示读到不同版本) */
                {
                    char user_now[INS_USER_NAME_MAX];
                    strncpy(user_now, INS_UserName(), sizeof(user_now) - 1);
                    user_now[sizeof(user_now) - 1] = '\0';
                    if (WEB_ConfigDirty() || strcmp(user_now, ui_user_last) != 0)
                    {
                        if (ui_state == ST_MAIN)
                        {
                            WEB_ConfigDirtyClear();
                            strncpy(ui_user_last, user_now, sizeof(ui_user_last) - 1);
                            ui_user_last[sizeof(ui_user_last) - 1] = '\0';
                            UI_SetUserTitle(user_now);
                            UI_RenderScreen();
                        }
                        else if (ui_state == ST_TODO)
                        {
                            WEB_ConfigDirtyClear();
                            TODO_Enter();   /* 网页改了待办: 刷新列表(光标重置) */
                        }
                        else if (ui_state == ST_ALARM)
                        {
                            WEB_ConfigDirtyClear();
                            ALM_WebChanged();   /* 网页改了闹钟: 列表就地刷新 */
                        }
                        else if (ui_state == ST_SUB)
                        {
                            WEB_ConfigDirtyClear();
                            if (ui_menu_cfg[sub_kind].fn == UI_FN_SETTING)
                            {
                                SET_SubmenuEnter();   /* 设置项文字含当前值: 网页改动后刷新标签 */
                                UI_SubMenuSetCur(UI_SubMenuCur());
                            }
                            else
                            {
                                UI_SubMenuSetCur(UI_SubMenuCur());   /* 网页改了主题/颜色: 就地重绘当前子菜单 */
                            }
                        }
                    }
                }
                if (ui_state == ST_MAIN)
                {
                    if (LOOM_TimeOn())
                    {
                        /* 彩蛋「纺织时间」(LOOM 组件): 现实1秒=显示1小时, 显示节流 ~5次/秒防刷屏打满 */
                        char d[8], t[12], w[8];
                        LOOM_TimeGet(d, sizeof(d), t, sizeof(t), w, sizeof(w));
                        if (now - egg_draw_last >= 200)
                        {
                            egg_draw_last = now;
                            UI_TimeSet(d, t, w);
                        }
                        if (now - egg_wx_last >= 1200)   /* 天气每1.2秒跳一次(随机词+随机温湿) */
                        {
                            egg_wx_last = now;
                            UI_WeatherSet(NET_WeatherMadStr());
                        }
                    }
                    else
                    {
                        char date[8], clk[12], week[8], wx[32];
                        NET_DateStrCopy(date, sizeof(date));
                        NET_TimeStrCopy(clk, sizeof(clk));
                        NET_WeekStrCopy(week, sizeof(week));
                        UI_TimeSet(date, clk, week);               /* 主页面左侧日期+星期+时间刷新(变化才重绘) */
                        UI_WeatherSet(NET_WeatherStrCopy(wx, sizeof(wx)) ? wx : NULL);   /* 无/超期=NULL->清除 */
                    }
                    UI_WifiSet(NET_WifiOk());                      /* 左上角网络图标(绿=已连/灰=未连) */
                    if (now - bat_last >= 2000)                    /* 每2秒读一次电量(数值变化才重绘) */
                    {
                        bat_last = now;
                        UI_BatterySet(BAT_GetPct());               /* 左上角电量图标 */
                    }
                }
                else if (ui_state == ST_GACHA)
                {
                    GACHA_Tick();                      /* 抽卡动画/语音/滚动推进(非阻塞) */
                    if (!GACHA_Busy())                 /* 抽卡"退出"已重绘主界面 -> 回主态 */
                    {
                        ui_pop();
                    }
                }
                else if (ui_state == ST_ASK)
                {
                    INS_Tick();                        /* 答案破译动画推进(非阻塞) */
                    if (!ANS_Busy())                   /* 询问"退出"已重绘主界面 -> 回主态 */
                    {
                        ui_pop();
                    }
                }
                else if (ui_state == ST_ALARM)
                {
                    ALM_Tick();                      /* 闹钟子界面推进(非阻塞) */
                    if (!ALM_Busy()) ui_pop();  /* 二级菜单"退出" -> 回 TTL 子菜单 */
                }
                else if (ui_state == ST_INS || ui_state == ST_INFO)
                {
                    if (ui_state == ST_INFO && ota_info_active)
                    {
                        if (now - ota_ui_last >= 200)   /* 节流重绘, 避免每 20ms 全屏刷屏 */
                        {
                            char ota_st[64];
                            ota_ui_last = now;
                            ota_drv_status(ota_st, sizeof(ota_st));
                            UI_FullScreen("版本更新", ota_st);
                        }
                    }
                    else
                    {
                        INS_Tick();                    /* 破译动画推进(非阻塞; 系统信息/开启配网提示也走乱码) */
                    }
                    /* 彩蛋确认「MADE IN HEAVEN」: 破译完成后定格片刻, 自动回主界面看时间加速 */
                    if (egg_confirm && ui_state == ST_INS)
                    {
                        if (INS_Finished())
                        {
                            if (egg_hold_t == 0) egg_hold_t = now;
                            else if (now - egg_hold_t >= 900)
                            {
                                egg_confirm = 0;
                                egg_hold_t = 0;
                                INS_Exit();            /* 内部已 UI_RenderScreen 回主界面 */
                                ui_pop();
                                PWR_Activity(now);   /* 防回主界面后立即被判息屏 */
                            }
                        }
                        else egg_hold_t = 0;           /* 仍在破译, 重置定格计时 */
                    }
                }
                else if (ui_state == ST_MPU)
                {
                    MPU_BalanceTick();                 /* 平衡: 实时刷新 横滚/俯仰/航向 */
                }
    
                /* 息屏检查: 无按键超过设置时长则关背光.
                 * OTA 升级进行中不判息屏: 息屏->待机会经 stby_net_stop 关 WiFi, 下载被掐断(表现为"下载中断") */
                if (SET_TimeoutSec() > 0 && !ota_drv_busy() &&
                    (now - PWR_LastAct() >= (uint32_t)SET_TimeoutSec() * 1000))
                {
                    if (SET_AodClock())
                    {
                        aod_active = 1;
                        aod_mode = 0;
                        aod_show_current(0, 0);
                        PWR_Activity(now);
                    }
                    else
                    {
                        PWR_LcdOff();
                    }
                }
            }
        }
        BUZZER_Tick();   /* 恒推进蜂鸣: 息屏时也把已开始的哔走完, 防蜂鸣卡在响发烫 */

        /* 待机: 息屏后进浅睡眠(倒计时/番茄钟/OTA 下载/云端会话进行中不睡: 前两者需精确走秒,
         * OTA 待机会关 WiFi 掐断下载, 云端在线会断 MQTT 致频繁掉线; 按键唤醒后有 400ms 冷却) */
        if (!PWR_ScreenOn() && ui_state != ST_TIMER && ui_state != ST_POMO &&
            !ota_drv_busy() && !CLOUD_KeepAlive() && PWR_StandbyAllowed(now))
        {
            PWR_StandbyEnter();   /* 息屏后浅睡眠; 扣屏等待窗/唤醒冷却由 POWER 判定 */
        }
    }
}

/* ================= 入口 ================= */
void app_main(void)
{
    /* 复位原因诊断(排查"烧录后过几秒又重启"): 每次开机打印上次复位类型 */
    {
        const char *rs = "unknown";
        switch (esp_reset_reason())
        {
            case ESP_RST_POWERON:   rs = "POWERON(上电)"; break;
            case ESP_RST_EXT:       rs = "EXT(外部复位/EN按键)"; break;
            case ESP_RST_SW:        rs = "SW(软件 esp_restart)"; break;
            case ESP_RST_PANIC:     rs = "PANIC(崩溃重启)"; break;
            case ESP_RST_INT_WDT:   rs = "INT_WDT(中断看门狗)"; break;
            case ESP_RST_TASK_WDT:  rs = "TASK_WDT(任务看门狗)"; break;
            case ESP_RST_WDT:       rs = "WDT(看门狗)"; break;
            case ESP_RST_DEEPSLEEP: rs = "DEEPSLEEP(深睡唤醒)"; break;
            case ESP_RST_BROWNOUT:  rs = "BROWNOUT(电压跌落)"; break;
            case ESP_RST_SDIO:      rs = "SDIO"; break;
            case ESP_RST_USB:       rs = "USB"; break;
            default:                rs = "UNKNOWN"; break;
        }
        printf("[RST] reason=%s\n", rs);
    }

    /* NVS 初始化(WiFi/NTP/设置 必需) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 上电先把功放关断(SD=GPIO13 拉低), 防初始化期间浮空引脚被功放放大 -> 喇叭刺声 */
    gpio_set_direction(GPIO_NUM_13, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_13, 0);

    KEY_Init();
    UI_UserInit();   /* 加载使用者列表(网页可添加, NVS持久化), 须在 UI_Init 前 */
    UI_Init();
    lcd_sleep_hold();   /* 浅睡眠保持 CS/RST/DC 输出高, 防唤醒白屏 */
    BUZZER_Init();   /* 有源蜂鸣器 GPIO15(高电平有效), 须在任何 Beep 前 */
    INS_Init();
    UI_SetUserTitle(INS_UserName());   /* 主菜单「使用者」标题 = 当前使用者(如李箱) */
    UI_RenderScreen();                 /* 重绘让标题立即生效 */
    DS1302_Init();   /* DS1302 GPIO 先就绪(防 SNTP 早到写回 RTC 时引脚未初始化) */
    NET_Init();      /* 初始化 WiFi/SNTP 基础(不自动联网, 由"联网"子菜单手动触发; 顺带设东八区 TZ) */
    /* 上电自动显示时间: DS1302 有有效时间则直接采用, 无需等联网校时(联网后 SNTP 再校准写回) */
    {
        struct tm t;
        if (DS1302_Read(&t))
        {
            struct timeval tv;
            tv.tv_sec = mktime(&t);   /* TZ 已设, 按东八区转 epoch */
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            NET_TimeAdopt();
            printf("[DS1302] adopt %04d-%02d-%02d %02d:%02d:%02d\n",
                   t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        }
        else
        {
            printf("[DS1302] no valid RTC time, wait SNTP\n");
        }
    }
    SOUND_Init(); /* 初始化 I2S 功放(MAX98357A, 无设备接上也无副作用) */
    SET_Init();   /* 加载设置并应用(蜂鸣开关/音量) */
    ALM_Init();   /* 闹钟初始化(NVS 加载) */
    TODO_Init();  /* 待办初始化(NVS 加载) */
    ANS_Init();   /* 答案之书初始化(NVS 加载) */
    LOOM_Init();  /* 织机彩蛋组件(Konami 解锁/纺织时间/纺织记忆) */
    POM_Init();   /* 番茄钟组件 */
    BAT_Init();   /* 电量 ADC 初始化(GPIO1 分压) */
    MPU_Init();   /* MPU6050 六轴初始化(软件I2C, 无传感器则后台重试) */
    GACHA_Init(); /* 创建抽卡/图鉴跨任务互斥量(须在 WEB_Init 启动 httpd 之前, 避免绘图请求撞上 mux=NULL) */
    CLOUD_Init();   /* 云端(OneNET MQTT): 加载三元组并建会话任务(未启用不连, 网页「云端」卡片配置); 须在 WEB_Init 前, 防 httpd 请求早于配置互斥量 */
    WEB_Init();   /* 启动配置页(联网后访问 http://<ip>/) */
    ota_drv_init();   /* 加载 OTA 配置(NVS "ota": url/sha256) */
    ota_drv_mark_valid();   /* 当前固件能跑到这里: 标记有效, 防止异常回滚 */
    UI_RenderScreen();   /* WEB_Init 已加载持久化主题色: 应用后重绘一次 */

    key_q = xQueueCreate(8, sizeof(uint8_t));
    MPU_Start(key_q);          /* 启动六轴采样任务(摇动->按键事件入队) */
    if (xTaskCreate(input_task, "input", 2048, NULL, 5, &input_task_h) != pdPASS ||
        xTaskCreate(ui_task,    "ui",    4096, NULL, 3, NULL) != pdPASS)
    {
        printf("[FATAL] task create failed, restart\n");   /* 无界面半活状态最难排查: 直接重启 */
        esp_restart();
    }

    /* 电源宿主回调注册(挂起谁/怎么查键/闹钟善后在 main 装配) */
    s_pwr_host.input_task      = input_task_h;
    s_pwr_host.on_enter        = stby_net_stop;
    s_pwr_host.btn_pressed     = stby_btn_pressed;
    s_pwr_host.alarm_due       = stby_alarm_due;
    s_pwr_host.sensor_suspend  = stby_sensor_suspend;
    s_pwr_host.sensor_resume   = stby_sensor_resume;
    s_pwr_host.on_alarm_wake   = stby_alarm_wake;
    PWR_Init(&s_pwr_host);

    vTaskDelete(NULL);   /* 主任务结束, 交由两个工作任务 */
}
