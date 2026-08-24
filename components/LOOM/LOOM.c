/* LOOM 组件: 「织机」彩蛋(独立组件, 自主菜单隐藏)
 * - 纺织时间: 现实1秒=显示1小时, 时/分/秒/日同源一个连续时间不脱节
 * - 纺织记忆: 全系统白框滤镜(UI_BoxMode), 唯一出口=再进一次本菜单关闭
 * - Konami 解锁序列在主界面由 main 喂入: 摇上x2,摇下x2,左,右,左,右(U/D事件与键反向, 见 konami_seq)
 *   (左=EVT_OK 右=EVT_LONG_OK, 摇动手势与物理按键等价) */
#include "LOOM.h"
#include "UI.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOOM_ITEM_N   3
static const char *const loom_items[LOOM_ITEM_N] = { "纺织时间", "纺织记忆", "退出" };

static uint8_t loom_busy = 0;

/* ---- 纺织时间状态(自 main.c 迁入) ---- */
static uint8_t mih_on = 0;            /* 1=时间加速中 */
static time_t mih_base = 0;           /* 开启时刻真实 epoch */
static int64_t mih_ms = 0;            /* 开启时刻 esp_timer ms(int64 防 uptime 回绕) */

/* ---- Konami 序列: 摇晃「上上 下下 左右左右」(按键等效 下下上上+OK/LNG) ----
 * 设计(v2): 前四步(上上下下)全部放行照常滚动 —— 有反馈、失败零副作用;
 * 后四步(左=OK 右=LONG_OK)必然对应"打开子菜单"动作, 由 main 查 LOOM_KonamiArmed()
 * 拦截下发, 序列才能存活到完成. 2秒无输入自动复位. */
static const uint8_t konami_seq[8] = { 3, 3, 1, 1, 2, 4, 2, 4 };   /* 物理序:摇上x2,摇下x2,左,右,左,右; 摇U/D事件与键相反故3,3,1,1(按键=下下上上) */
#define KONAMI_ARM_IDX 4              /* 从第5步起进入拦截区(首个OK) */
static uint8_t konami_idx = 0;
static uint32_t konami_last = 0;      /* 上次序列输入时刻 ms */
#define KONAMI_TIMEOUT_MS 2000u

void LOOM_Init(void)
{
    loom_busy = 0;
    konami_idx = 0;
}

uint8_t LOOM_Busy(void) { return loom_busy; }

void LOOM_Enter(void)
{
    loom_busy = 1;
    UI_SubMenuInitItems(loom_items, LOOM_ITEM_N);   /* 光标默认第0项(纺织时间) */
}

uint8_t LOOM_Konami(uint8_t evt)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t ret = LOOM_KON_PASS;

    if (konami_idx != 0 && now - konami_last > KONAMI_TIMEOUT_MS)
        konami_idx = 0;                                /* 半截序列超时作废 */
    if (evt >= 1 && evt <= 4)
    {
        if (evt == konami_seq[konami_idx])
        {
            konami_last = now;
            if (++konami_idx >= 8)
            {
                konami_idx = 0;
                ret = LOOM_KON_DONE;
            }
        }
        else
        {
            konami_idx = (evt == konami_seq[0]) ? 1 : 0;   /* 不匹配: 复位; 首元素吻合则从第2位续判 */
        }
    }
    return ret;
}

uint8_t LOOM_KonamiArmed(void)
{
    return (konami_idx >= KONAMI_ARM_IDX) ? 1 : 0;
}

uint8_t LOOM_TimeOn(void) { return mih_on; }

void LOOM_TimeToggle(void)
{
    mih_on = !mih_on;
    if (mih_on)
    {
        mih_base = time(NULL);
        mih_ms = (int64_t)(esp_timer_get_time() / 1000);
    }
}

void LOOM_TimeGet(char *d, size_t dn, char *t, size_t tn, char *w, size_t wn)
{
    static const char *const week_name[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    /* 现实1秒=显示1小时(3600显示秒): 时每1实秒+1、分每~16.7实秒+1、秒每~0.28实秒+1、日每~6.67实时滚1天 */
    int64_t el_ms = (int64_t)(esp_timer_get_time() / 1000) - mih_ms;   /* int64: 防回绕大偏差 */
    time_t dt = mih_base + (time_t)((uint64_t)el_ms * 3600 / 1000);    /* 连续显示秒 */
    struct tm tmv;
    localtime_r(&dt, &tmv);
    snprintf(d, dn, "%02u-%02u", (unsigned)(tmv.tm_mon + 1), (unsigned)tmv.tm_mday);
    snprintf(t, tn, "%02u:%02u:%02u", (unsigned)tmv.tm_hour, (unsigned)tmv.tm_min, (unsigned)tmv.tm_sec);
    snprintf(w, wn, "%s", (tmv.tm_wday >= 0 && tmv.tm_wday < 7) ? week_name[tmv.tm_wday] : "--");
}

uint8_t LOOM_Key(uint8_t up, uint8_t ok, uint8_t down, uint8_t lng)
{
    if (!loom_busy) return LOOM_KEY_NONE;
    if (lng)                                   /* 长按OK: 退出织机 */
    {
        loom_busy = 0;
        return LOOM_KEY_EXIT;
    }
    if (up)        UI_SubMenuScroll(1);
    else if (down) UI_SubMenuScroll(-1);
    else if (ok)
    {
        uint8_t sel = UI_SubMenuCur();
        if (sel == 2)                          /* 退出 */
        {
            loom_busy = 0;
            return LOOM_KEY_EXIT;
        }
        if (sel == 0)                          /* 纺织时间 */
        {
            LOOM_TimeToggle();
            loom_busy = 0;
            return LOOM_KEY_TIME;              /* main 播确认动画并回主界面 */
        }
        if (sel == 1)                          /* 纺织记忆: 只切滤镜, 导航交给 main */
        {
            UI_BoxModeSet(UI_BoxModeGet() ? 0 : 1);
            loom_busy = 0;
            return LOOM_KEY_MEMORY;
        }
    }
    return LOOM_KEY_NONE;
}

void LOOM_Tick(void)
{
    /* 预留: 织机暂无逐帧动画 */
}
