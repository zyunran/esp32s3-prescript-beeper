/* POWER 组件: 自 main.c 拆出(v1.03 后), 状态与行为原样迁移.
 * 待机浅睡眠: 本板 GPIO 唤醒触发硬件睡眠拒绝(ESP_ERR_SLEEP_REJECT, 实测),
 * 故用「定时器 tick 睡眠」: 每 50ms 睡一片, 醒来查按键/闹钟再睡;
 * 按键响应 ≤50ms; CPU 睡眠期占 >99%, 比全速运行省电数倍.
 * 白屏防护由宿主 lcd_sleep_hold 保证(睡眠期间 CS/RST/DC 保持高). */
#include "POWER.h"
#include "LCD.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "freertos/task.h"
#include <stdio.h>

#define STANDBY_TICK_US  50000ULL /* 定时唤醒片长 50ms(按键响应≤50ms, 与功耗折中) */

static pwr_host_t h;                       /* 宿主回调集(PWR_Init 注册) */
static uint32_t last_act = 0;              /* 距上次操作时刻(ms), 提为组件内全局供待机/息屏判定 */
static uint8_t  scr_on = 1;                /* 屏幕背光状态(1=亮) */
static uint32_t standby_reenter_at = 0;    /* 按键唤醒后的待机冷却截止(防按住反复进出空转) */

static uint32_t pw_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void PWR_Init(const pwr_host_t *host) { if (host) h = *host; }

uint8_t  PWR_ScreenOn(void)   { return scr_on; }
uint32_t PWR_LastAct(void)    { return last_act; }
void     PWR_Activity(uint32_t now_ms) { last_act = now_ms; }

void PWR_Wake(uint32_t now_ms)
{
    if (!scr_on) lcd_on();
    scr_on = 1;
    last_act = now_ms;
}

void PWR_LcdOff(void)
{
    lcd_off();
    scr_on = 0;
}

uint8_t PWR_StandbyAllowed(uint32_t now_ms)
{
    return (now_ms >= standby_reenter_at);   /* 仅剩按键唤醒后的防抖冷却 */
}

void PWR_StandbyEnter(void)
{
    uint8_t alarm = 0;
    printf("[STBY] enter\n");
    if (h.on_enter) h.on_enter();                  /* 断网等宿主收尾 */
    if (h.input_task) vTaskSuspend(h.input_task);  /* 挂起按键轮询: 否则 20ms 轮询持续唤醒 CPU 削省电;
                                                    * 唤醒判断改由睡眠片直接查 GPIO(cb), 不影响响应 */
    if (h.sensor_suspend) h.sensor_suspend();      /* 挂起六轴采样(同上) */
    esp_sleep_enable_timer_wakeup(STANDBY_TICK_US);
    for (;;)
    {
        if (h.btn_pressed && h.btn_pressed()) break;          /* 按键唤醒 */
        if (h.alarm_due && h.alarm_due()) { alarm = 1; break; }/* 闹钟到点(含本分钟) */
        if (esp_light_sleep_start() != ESP_OK)                 /* 定时器片睡眠 */
        {
            vTaskDelay(20 / portTICK_PERIOD_MS);               /* 防御: 被拒时降速重试, 防忙转 */
        }
    }
    if (h.sensor_resume) h.sensor_resume();
    if (h.input_task) vTaskResume(h.input_task);   /* 唤醒键按下沿在恢复后入队(长按从按下时刻起算) */
    printf("[STBY] wake %s\n", alarm ? "alarm" : "btn");
    {   uint32_t now = pw_now();
        if (!alarm) standby_reenter_at = now + 400;   /* 按键唤醒冷却 400ms */
        if (alarm && h.on_alarm_wake) h.on_alarm_wake();
        if (alarm) PWR_Wake(now);
    }
    /* 按键唤醒: 不预亮屏, 按键事件入队后按"第一键仅唤醒"处理 */
}
