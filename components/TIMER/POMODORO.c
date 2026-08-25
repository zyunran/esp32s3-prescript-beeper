/* POMODORO 组件: 番茄钟(设定/工作/休息 三态 + 暂停)
 * 绘制走 UI_Scr* 帧缓冲接口(LCD 单写者: 仅 ui_task 调用), 秒变化才整屏重绘. */
#include "POMODORO.h"
#include "UI.h"
#include "esp_timer.h"
#include <stdio.h>

typedef enum { PH_SET, PH_WORK, PH_BREAK } pom_ph_t;

static pom_ph_t ph = PH_SET;
static uint16_t work_min = POM_WORK_DEFAULT;   /* 设定的工作分钟 */
static uint32_t start_ms = 0;                  /* 当前阶段开始时刻 ms */
static int64_t pause_acc = 0;                  /* 暂停累计 ms */
static uint32_t pause_at = 0;                  /* 本次暂停起点(0=运行中) */
static uint32_t last_sec = 0xFFFF;             /* 上次绘制的剩余秒 */
static uint8_t  round_n = 0;                   /* 已完成的工作轮数 */
static uint8_t  exit_req = 0;                  /* 长按OK请求退出 */
static uint8_t  done_pending = 0;              /* 到点事件待上报一次 */

static uint32_t p_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 本阶段总时长 ms */
static uint32_t phase_total(void)
{
    return (uint32_t)(ph == PH_WORK ? work_min : POM_BREAK_MIN) * 60000u;
}

/* 剩余秒(int64 防下溢/回绕; 含暂停补偿) */
static int64_t remain_s(void)
{
    /* 已用时间 = 累计运行时间 - 已暂停累计;
     * 当前正在暂停时冻结在 pause_at 时刻, 不再把暂停时段算入已用. */
    int64_t anchor = pause_at ? (int64_t)pause_at : (int64_t)p_now();
    int64_t used = anchor - (int64_t)start_ms - pause_acc;
    int64_t r = ((int64_t)phase_total() - used + 999) / 1000;
    return r < 0 ? 0 : r;
}

static void phase_begin(pom_ph_t next)
{
    ph = next;
    start_ms = p_now();
    pause_acc = 0;
    pause_at = 0;
    last_sec = 0xFFFF;                         /* 强制重绘 */
}

static void pom_render(void)
{
    char line[40];
    UI_ScrClear(UI_COLOR_BG);
    if (ph == PH_SET)
    {
        UI_ScrTextCenter(6,  "番茄钟", UI_COLOR_MENU, UI_COLOR_BG);
        snprintf(line, sizeof(line), "工作 %u 分钟", (unsigned)work_min);
        UI_ScrTextCenter(28, line, UI_COLOR_FRAME, UI_COLOR_BG);
        UI_ScrTextCenter(56, "上下调整 / OK开始", UI_COLOR_MENU, UI_COLOR_BG);
    }
    else
    {
        int64_t s = remain_s();
        snprintf(line, sizeof(line), "%02d:%02d", (int)(s / 60), (int)(s % 60));
        UI_ScrTextCenter(4,  (ph == PH_WORK) ? "工作中" : "休息中",
                         (ph == PH_WORK) ? UI_COLOR_MENU : UI_COLOR_FRAME, UI_COLOR_BG);
        UI_ScrTextCenter(26, line, UI_COLOR_TIME, UI_COLOR_BG);
        if (pause_at) UI_ScrTextCenter(58, "已暂停 OK继续", UI_COLOR_MENU, UI_COLOR_BG);
        else
        {
            snprintf(line, sizeof(line), "第 %u 轮", (unsigned)round_n + 1);
            UI_ScrTextCenter(58, line, UI_COLOR_MENU, UI_COLOR_BG);
        }
    }
    UI_ScrBlit();
}

void POM_Init(void)
{
    ph = PH_SET;
    exit_req = 0;
    done_pending = 0;
}

void POM_Enter(void)
{
    ph = PH_SET;
    work_min = POM_WORK_DEFAULT;
    round_n = 0;
    exit_req = 0;
    done_pending = 0;
    pause_acc = 0;
    pause_at = 0;
    last_sec = 0xFFFF;
    pom_render();
}

void POM_Exit(void) { exit_req = 1; }

pom_ret_t POM_Key(uint8_t up, uint8_t ok, uint8_t down)
{
    if (exit_req) return POM_EXIT;
    if (ph == PH_SET)
    {
        if (up || down)
        {
            work_min += up ? POM_WORK_STEP : -POM_WORK_STEP;
            if (work_min > POM_WORK_MAX) work_min = POM_WORK_MIN;
            if (work_min < POM_WORK_MIN) work_min = POM_WORK_MAX;   /* 循环调节 */
            pom_render();
        }
        else if (ok)
        {
            round_n = 0;
            phase_begin(PH_WORK);
            pom_render();
        }
    }
    else if (ok)                               /* 工作/休息中: 暂停<->继续 */
    {
        if (pause_at) { pause_acc += (int64_t)(p_now() - pause_at); pause_at = 0; }
        else pause_at = p_now();
        pom_render();
    }
    return exit_req ? POM_EXIT : POM_RUN;
}

pom_ret_t POM_Tick(uint8_t render)
{
    if (exit_req) return POM_EXIT;
    if (ph == PH_SET) return POM_RUN;
    if (!pause_at && !done_pending)
    {
        if (remain_s() <= 0)
        {
            done_pending = 1;                  /* 先报 DONE(蜂鸣), 下次 Tick 切相 */
        }
    }
    if (done_pending)
    {
        done_pending = 0;
        if (ph == PH_WORK) { round_n++; phase_begin(PH_BREAK); }
        else               { phase_begin(PH_WORK); }
        pom_render();
        return POM_DONE;
    }
    if (render)
    {
        int64_t s = remain_s();
        if ((uint32_t)s != last_sec)
        {
            last_sec = (uint32_t)s;
            pom_render();
        }
    }
    return POM_RUN;
}
