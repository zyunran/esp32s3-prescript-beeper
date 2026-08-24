/* RTOS 多任务架构:
 *  - input_task: GPIO 沿检测读按键 -> 事件队列(高优先级, 独立于界面)
 *  - ui_task:    收事件按界面状态机驱动, 并推进各功能(破译/抽卡/计时/神谕推送)
 *  - 待机: 息屏后 ui_task 进浅睡眠(50ms tick 查按键/闹钟, STANDBY_TICK_US);
 *    路径1 完全按需联网: 唤醒不再自动重连 WiFi, 联网仅由 联网->连接网络 手动开启(会话内校时/天气)
 * 功能已拆分为组件: UI(菜单/配置) / INSTRUCTION(破译+蜂鸣) / GACHA(抽卡) / NET(联网天气)
 *   / SOUND(音频) / SETTING(设置+NVS) / TIMER(计时/倒计时) / ORACLE(随机神谕推送)
 * 主菜单标题与子菜单项文字集中配置于 ui_menu_cfg(ui.c), 改那里即可改文字
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
#include "SETTING.h"
#include "TIMER.h"
#include "ALARM.h"
#include "TODO.h"
#include "BATTERY.h"
#include "ORACLE.h"
#include "ANSWER.h"
#include "WEB.h"
#include "MPU6050.h"
#include "DS1302.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

/* ================= 按键事件 =================
 * 注: 长按 OK=返回上一级; 连发参数改这里即可调整手感 */
#define EVT_NONE     0
#define EVT_UP       1
#define EVT_OK       2
#define EVT_DOWN     3
#define EVT_LONG_OK  4    /* OK 长按(返回上一级) */

/* 长按参数(改这里即可调整手感):
 *   LONG_PRESS_MS  = 判定"长按"的按住时长(超此即长按)
 *   REPEAT_PRESS_MS = 长按连发间隔(上下键按住后每隔此时间滚动一项) */
#define LONG_PRESS_MS   600
#define REPEAT_PRESS_MS 150

/* 路径1 联网会话空闲超时(ms): 连网后按键/网页(NET_Touch)都静默超此时长 -> 自动断(省电+缩暴露面). */
#define NET_SESSION_IDLE_MS  60000
/* 开启联网后, 超过此时长仍没连上则判定"未连上"并反馈 */
#define NET_CONNECT_RESULT_MS  12000

/* ================= 界面状态机 ================= */
typedef enum { ST_MAIN, ST_SUB, ST_INS, ST_GACHA, ST_TIMER, ST_ALARM, ST_INFO, ST_TODO, ST_MPU, ST_ASK } ui_state_t;

static QueueHandle_t key_q;
static TaskHandle_t input_task_h = NULL;   /* 按键轮询任务句柄(待机挂起/唤醒恢复, 省电) */
static ui_state_t ui_state = ST_MAIN;
static uint8_t sub_kind;   /* 当前所在子菜单对应的主菜单项索引 */
static char daily_last[8] = {0};   /* 每日签: 上次签到的日期 "MM-DD" */
static uint8_t net_conn_pending = 0;    /* 联网开关 开启后等结果: 连上→"已连接" / 超时→"未连上" */
static uint32_t net_conn_deadline = 0;  /* 结果判定超时时刻(esp_timer ms) */
static uint8_t reset_pending = 0;     /* 初始化确认中: 再按OK清除NVS重启 */
static uint8_t set_info_active = 0;   /* 1=正在"系统信息"翻页页(上下键翻页, 其他键返回) */

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

/* ================= 彩蛋「纺织时间」(made in heaven): 现实1秒=显示1小时 ================= */
static uint8_t mih_on = 0;            /* 1=时间加速中 */
static time_t mih_base = 0;           /* 开启时刻真实 epoch */
static int64_t mih_ms = 0;            /* 开启时刻 esp_timer ms(int64: 防 >49.7 天 uptime 时 uint32 回绕) */
static uint32_t mih_wx_last = 0;      /* 彩蛋天气刷新节流 ms */
static uint32_t mih_draw_last = 0;    /* 彩蛋时间整屏重绘节流 ms(加速时秒变化远快于刷新率, 限流防总线/CPU 打满) */
static uint8_t mih_confirm = 0;       /* 彩蛋确认乱码显示中(播完自动回主界面) */
static uint32_t mih_hold_t = 0;       /* 确认定格计时 */
static const char *const ui_week_name[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static void mih_toggle(void)
{
    mih_on = !mih_on;
    if (mih_on)
    {
        mih_base = time(NULL);
        mih_ms = (int64_t)(esp_timer_get_time() / 1000);
        mih_wx_last = (uint32_t)mih_ms;
        mih_draw_last = (uint32_t)mih_ms;
    }
}

/* ================= 彩蛋「纺织记忆」: 全系统白框滤镜 =================
 * 织机子菜单按一下: 整台 BB机 所有 UI(文字/数字/图标/菜单, 任意界面)全部变成空心白框,
 * 每个字符串按内容哈希随机稳定保留 1 个真字作提示, 其余白框(同串位置不变不乱跳).
 * 系统仍可正常上下移动/确认操作(盲操). 唯一退出: 再次进入织机→纺织记忆. */
static void egg_toggle(void)
{
    UI_BoxModeSet(UI_BoxModeGet() ? 0 : 1);   /* 切换白框滤镜 */
    /* 状态同步到主界面(与渲染出的主菜单一致): 否则白框下看着主菜单、实际仍在织机子菜单, 按键会脱节 */
    ui_stack_n = 0;
    ui_state = ST_MAIN;
    UI_RenderScreen();                        /* 立即全屏重绘(白框/恢复) */
}

/* 进入"使用者"子菜单并高亮当前使用者名称(实时反映, 如当前为但丁) */
static void user_submenu_enter(void)
{
    const ui_menu_cfg_t *cfg = &ui_menu_cfg[sub_kind];
    uint8_t u;
    UI_SubMenuInitItems(cfg->items, cfg->item_count);
    for (u = 0; u + 1 < cfg->item_count; u++)
    {
        if (strcmp(cfg->items[u], INS_UserName()) == 0)
        {
            UI_SubMenuSetCur(u);
            break;
        }
    }
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
        if (cfg->fn == UI_FN_NET)   /* 联网开关项: 进入子菜单即把标签刷新为 开/关 实时状态 */
        {
            char nbuf[16];
            snprintf(nbuf, sizeof(nbuf), "联网:%s", NET_SessionOn() ? "开" : "关");
            UI_SubMenuSetItem(UI_NET_CONNECT, nbuf);
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
static void on_event(uint8_t evt)
{
    switch (ui_state)
    {
        case ST_MAIN:
            if (evt == EVT_UP)        UI_Scroll(1);
            else if (evt == EVT_DOWN) UI_Scroll(-1);
            else if (evt == EVT_OK)
            {
                uint8_t sel = UI_GetSelect();
                const ui_menu_cfg_t *cfg = &ui_menu_cfg[sel];
                switch (cfg->fn)
                {
                    case UI_FN_INS:                  /* 神谕 -> 指令破译 */
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
                    case UI_FN_TTL:                  /* TTL协议 -> 跨越时间/锚定时间/退出 */
                    case UI_FN_LOOM:                 /* 织机 -> 纺织时间/退出(彩蛋) */
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
                        UI_FullScreen("未配置WiFi", "先 开启配网 再连手机设WiFi");
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
                else if (cfg->fn == UI_FN_NET && sel == UI_NET_AP)  /* 联网-开启配网: 开/关配网热点 */
                {
                    uint8_t on = NET_ApToggle();
                    if (on)
                    {
                        char m[68];   /* "热点已开 <SSID>/<8位随机密码>": SSID≤32B+密码8B+中文前后缀, 留足余量 */
                        snprintf(m, sizeof(m), "热点已开 %s/%s", NET_GetApSsid(), NET_GetApPass());
                        UI_FullScreen("开启配网", m);
                    }
                    else
                    {
                        UI_FullScreen("开启配网", "热点已关");
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
                else if (cfg->fn == UI_FN_SETTING)   /* 设置-各项: 循环/切换(组件处理) */
                {
                    if (sel == SET_IDX_INFO)   /* 系统信息(可上下翻页: 系统/签收/战绩) */
                    {
                        set_info_active = 1;
                        SET_ShowInfo();
                        ui_push(ST_INFO);
                    }
                    else if (sel == SET_IDX_BAL)   /* 平衡: MPU6050 六轴姿态实时页 */
                    {
                        ui_push(ST_MPU);
                    }
                    else if (sel == SET_IDX_RESET)   /* 初始化: 确认后清NVS重启 */
                    {
                        reset_pending = 1;
                        INS_Show("确认初始化?\n再按OK清除全部并重启");
                        ui_push(ST_INFO);
                    }
                    else
                    {
                        SET_SubmenuSelect(sel);
                    }
                }
                else if (cfg->fn == UI_FN_TTL)       /* TTL协议: 跨越时间(倒计时)/锚定时间(闹钟) */
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
                }
                else if (cfg->fn == UI_FN_LOOM && sel == UI_LOOM_SPIN)   /* 织机-纺织时间: 彩蛋开关 */
                {
                    mih_toggle();
                    ui_to_main();                    /* 彩蛋确认播完回主界面看时间加速 */
                    INS_Show(mih_on ? "MADE IN\nHEAVEN" : "时间恢复");
                    mih_confirm = 1;                 /* 乱码播完自动回主界面看时间加速 */
                    ui_push(ST_INS);
                }
                else if (cfg->fn == UI_FN_LOOM && sel == UI_LOOM_MEMORY)  /* 织机-纺织记忆: 全系统白框滤镜开关(唯一退出=再按一次) */
                {
                    egg_toggle();
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
            if (reset_pending)              /* 初始化: 再按OK清NVS重启, 其他键取消 */
            {
                reset_pending = 0;
                if (evt == EVT_OK)
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

        case ST_ASK:
            ANS_OnEvent(evt);                            /* 询问内部处理(含长按OK返回) */
            break;

        case ST_INS:
            INS_Exit();                  /* 破译中任意键返回 */
            mih_confirm = 0;             /* 手动按键退出: 取消彩蛋确认自动回主界面 */
            mih_hold_t = 0;
            ui_pop();
            break;
    }
}

/* ================= 待机: 息屏后浅睡眠(省电) =================
 * 息屏(背光灭)后进浅睡眠, CPU 停/RAM 保留/毫秒级唤醒, 系统时间由 RTC 维持不丢.
 * 注: 本板 GPIO 唤醒(EXT1/GPIO wakeup)触发硬件睡眠拒绝(ESP_ERR_SLEEP_REJECT, 实测),
 *     故用「定时器 tick 睡眠」: 每 50ms 睡一片, 醒来查按键/闹钟再睡.
 *     按键响应 ≤50ms; CPU 睡眠期占 >99%, 比全速运行省电数倍.
 * 待机关 WiFi(最省电); 路径1 唤醒后也不再自动重连(按需联网), 校时在联网会话内完成.
 * 白屏防护: 睡眠期间 LCD 的 CS/RST/DC 保持输出高(lcd_sleep_hold), 防屏复位/吃毛刺. */
#define STANDBY_TICK_US  50000ULL    /* 定时唤醒片长: 50ms(按键响应≤50ms, 与功耗折中; 原200ms唤醒偏慢) */

static uint32_t last_act;   /* 距上次操作时刻(ms), 提为全局供待机唤醒处理 */
static uint32_t standby_reenter_at = 0;   /* 按键唤醒后的待机冷却截止时刻(防按住时反复进退待机空转) */
static uint8_t  scr_on = 1; /* 屏幕背光状态(1=亮) */

/* 任一按键按下(低电平) */
static uint8_t standby_btn_pressed(void)
{
    return (gpio_get_level(UI_KEY_UP) == 0) ||
           (gpio_get_level(UI_KEY_OK) == 0) ||
           (gpio_get_level(UI_KEY_DOWN) == 0);
}

static void standby_enter(void)
{
    uint8_t alarm = 0;
    printf("[STBY] enter\n");
    NET_WifiStop();
    net_conn_pending = 0;   /* 会话被待机强断: 不再等联网结果(防唤醒后误弹"未连上") */
    /* 待机挂起按键轮询与六轴采样任务: 否则 20ms/30ms 轮询持续唤醒 CPU, 削弱浅睡眠省电;
     * 唤醒判断改由本任务直接读 GPIO(standby_btn_pressed), 挂起不影响响应 */
    if (input_task_h) vTaskSuspend(input_task_h);
    MPU_Suspend();
    esp_sleep_enable_timer_wakeup(STANDBY_TICK_US);
    for (;;)
    {
        if (standby_btn_pressed()) break;          /* 按键唤醒 */
        if (ALM_Check()) { alarm = 1; break; }     /* 闹钟到点(含本分钟) */
        if (esp_light_sleep_start() != ESP_OK)     /* 定时器片睡眠 */
        {
            vTaskDelay(20 / portTICK_PERIOD_MS);   /* 防御: 被拒时降速重试, 防忙转 */
        }
    }
    MPU_Resume();                                  /* 恢复六轴采样 */
    if (input_task_h) vTaskResume(input_task_h);   /* 恢复按键轮询: 唤醒键的按下沿在恢复后被检测入队(长按判定从按下时刻起算) */
    printf("[STBY] wake %s\n", alarm ? "alarm" : "btn");
    if (!alarm) standby_reenter_at = (uint32_t)(esp_timer_get_time() / 1000) + 400;   /* 按键唤醒: 冷却 400ms, 防键还按住时反复进出待机 */
    /* 路径1 完全按需: 唤醒不再自动重连 WiFi(省电+零暴露面). 需要联网时用户按 联网->连接网络 */
    if (alarm)
    {
        uint32_t nw = (uint32_t)(esp_timer_get_time() / 1000);
        /* 闹钟到点: 回主界面并显示(即使睡在子菜单也响) */
        ui_to_main();
        ALM_Show();
        ui_push(ST_INS);
        lcd_on();
        scr_on = 1;
        last_act = nw;
    }
    /* 按键唤醒: 不预亮屏, 按键事件由 input_task 入队, 主循环按"第一键仅唤醒"处理 */
}

/* ================= UI 主任务: 收事件 + 推进各功能 + 息屏 + 神谕推送 ================= */
static void ui_task(void *arg)
{
    uint32_t oracle_last = 0;   /* 神谕检查节流 */
    uint32_t bat_last = 0;      /* 电量读取节流 */
    static char ui_user_last[INS_USER_NAME_MAX];   /* 主界面已显示的使用者名(变化才重绘) */
    last_act = (uint32_t)(esp_timer_get_time() / 1000);
    scr_on = 1;
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
                    mih_toggle();
                    ui_to_main();                /* 彩蛋确认播完自动回主界面看时间加速 */
                    INS_Show(mih_on ? "MADE IN\nHEAVEN" : "时间恢复");
                    mih_confirm = 1;
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
                lcd_on();
                scr_on = 1;
                last_act = now;
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
                    /* 每日签计数(NVS "info"/"dsign", 与开机次数同命名空间; 系统信息页展示) */
                    {
                        nvs_handle_t h;
                        uint32_t ds = 0;
                        if (nvs_open("info", NVS_READWRITE, &h) == ESP_OK)
                        {
                            nvs_get_u32(h, "dsign", &ds);
                            ds++;
                            nvs_set_u32(h, "dsign", ds);
                            nvs_commit(h);
                            nvs_close(h);
                        }
                    }
                    INS_ShowRandom();
                    ui_push(ST_INS);
                    lcd_on();
                    scr_on = 1;
                    last_act = now;
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
                    lcd_on();
                    scr_on = 1;
                    last_act = now;
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
                    lcd_on();
                    scr_on = 1;
                    last_act = now;
                }
            }
            if (ORACLE_Due())
            {
                ORACLE_Delivered();
                if (ui_state == ST_MAIN)
                {
                    INS_ShowRandom();
                    ui_push(ST_INS);
                    lcd_on();
                    scr_on = 1;
                    last_act = now;
                }
            }
            if (ALM_Check())       /* 有闹钟到点(已在 ALM 内标记当日触发): 任意界面都打断显示 */
            {
                ui_to_main();      /* 与待机闹钟唤醒一致: 先回主界面再显示闹钟指令 */
                ALM_Show();        /* 闹钟专属指令乱码破译 */
                ui_push(ST_INS);
                lcd_on();
                scr_on = 1;
                last_act = now;
            }
        }

        /* 屏亮 20ms 快速响应按键; 屏灭 200ms 慢轮询省电(走秒用 esp_timer 不依赖此轮询, 倒计时到点误差≤200ms) */
        if (xQueueReceive(key_q, &evt, (scr_on ? 20u : 200u) / portTICK_PERIOD_MS))
        {
            last_act = now;
            if (!scr_on)               /* 屏幕休眠: 第一键仅唤醒 */
            {
                lcd_on();
                scr_on = 1;
            }
            else
            {
                on_event(evt);
            }
        }

        /* 按需联网会话(路径1): 会话进行中但按键/网页都静默超时 -> 自动断(省电+缩暴露面).
         * 以"射频开"为前置(连不上也算会话, 防密码错/无信号时射频空开); 按键与网页请求(NET_Touch)续期. */
        if (NET_SessionOn() && (now - last_act) >= NET_SESSION_IDLE_MS &&
            NET_SessionIdleMs() >= NET_SESSION_IDLE_MS)
        {
            net_conn_pending = 0;   /* 会话被自动结束: 取消联网结果挂起(防误弹"未连上") */
            NET_SessionEnd();
        }

        /* 倒计时: 无论亮屏与否都推进(render=亮屏才重绘, 熄屏省电);
         * 到点蜂鸣并唤醒屏幕显示"你已到达X分钟后的未来!" */
        if (ui_state == ST_TIMER)
        {
            tim_ret_t r = TIM_Tick(scr_on);
            if (r == TIM_DONE)
            {
                INS_BeepTimes(2);
                if (!scr_on) { lcd_on(); scr_on = 1; }   /* 倒计时结束自动亮屏 */
                last_act = now;
            }
            if (r == TIM_EXIT) ui_pop();      /* 完成/退出回 TTL 子菜单 */
        }

        if (scr_on)
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
                }
            }
            if (ui_state == ST_MAIN)
            {
                if (mih_on)
                {
                    /* 彩蛋「纺织时间」: 现实1秒=显示1小时(3600显示秒), 时每1实秒+1、分每~16.7实秒+1、
                     * 秒每~0.28实秒+1、日期每~6.67实时滚1天 —— 时/分/秒/日同源于一个连续时间, 同步不脱节 */
                    int64_t el_ms = (int64_t)(esp_timer_get_time() / 1000) - (int64_t)mih_ms;   /* int64: 防回绕大偏差 */
                    time_t dt = mih_base + (time_t)((uint64_t)el_ms * 3600 / 1000);  /* 连续显示秒 */
                    struct tm tmv;
                    char d[8], t[12];
                    localtime_r(&dt, &tmv);
                    snprintf(d, sizeof(d), "%02u-%02u", (uint8_t)(tmv.tm_mon + 1), (uint8_t)tmv.tm_mday);
                    snprintf(t, sizeof(t), "%02u:%02u:%02u", (uint8_t)tmv.tm_hour, (uint8_t)tmv.tm_min, (uint8_t)tmv.tm_sec);
                    if (now - mih_draw_last >= 200)  /* 加速时显示秒变化远快于刷新率: 限流到 ~5次/秒整屏重绘 */
                    {
                        mih_draw_last = now;
                        UI_TimeSet(d, t, ui_week_name[tmv.tm_wday]);   /* 彩蛋: 星期也随加速时间走 */
                    }
                    if (now - mih_wx_last >= 1200)   /* 天气每1.2秒跳一次 */
                    {
                        mih_wx_last = now;
                        UI_WeatherSet(NET_WeatherMadStr());   /* 天气词+温度+湿度全随机 */
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
                INS_Tick();                        /* 破译动画推进(非阻塞; 系统信息/开启配网提示也走乱码) */
                /* 彩蛋确认「MADE IN HEAVEN」: 破译完成后定格片刻, 自动回主界面看时间加速 */
                if (mih_confirm && ui_state == ST_INS)
                {
                    if (INS_Finished())
                    {
                        if (mih_hold_t == 0) mih_hold_t = now;
                        else if (now - mih_hold_t >= 900)
                        {
                            mih_confirm = 0;
                            mih_hold_t = 0;
                            INS_Exit();            /* 内部已 UI_RenderScreen 回主界面 */
                            ui_pop();
                            last_act = now;
                        }
                    }
                    else mih_hold_t = 0;           /* 仍在破译, 重置定格计时 */
                }
            }
            else if (ui_state == ST_MPU)
            {
                MPU_BalanceTick();                 /* 平衡: 实时刷新 横滚/俯仰/航向 */
            }

            /* 息屏检查: 无按键超过设置时长则关背光 */
            if (SET_TimeoutSec() > 0 && (now - last_act >= (uint32_t)SET_TimeoutSec() * 1000))
            {
                lcd_off();
                scr_on = 0;
            }
        }
        INS_BeepTick();   /* 恒推进蜂鸣: 息屏时也把已开始的哔走完, 防蜂鸣卡在响发烫 */

        /* 待机: 息屏后进浅睡眠(倒计时进行中不睡, 需精确走秒; 按键唤醒后有 400ms 冷却) */
        if (scr_on == 0 && ui_state != ST_TIMER && now >= standby_reenter_at)
        {
            standby_enter();
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
    BAT_Init();   /* 电量 ADC 初始化(GPIO1 分压) */
    MPU_Init();   /* MPU6050 六轴初始化(软件I2C, 无传感器则后台重试) */
    GACHA_Init(); /* 创建抽卡/图鉴跨任务互斥量(须在 WEB_Init 启动 httpd 之前, 避免绘图请求撞上 mux=NULL) */
    WEB_Init();   /* 启动配置页(联网后访问 http://<ip>/) */

    key_q = xQueueCreate(8, sizeof(uint8_t));
    MPU_Start(key_q);          /* 启动六轴采样任务(摇动->按键事件入队) */
    if (xTaskCreate(input_task, "input", 2048, NULL, 5, &input_task_h) != pdPASS ||
        xTaskCreate(ui_task,    "ui",    4096, NULL, 3, NULL) != pdPASS)
    {
        printf("[FATAL] task create failed, restart\n");   /* 无界面半活状态最难排查: 直接重启 */
        esp_restart();
    }

    vTaskDelete(NULL);   /* 主任务结束, 交由两个工作任务 */
}
