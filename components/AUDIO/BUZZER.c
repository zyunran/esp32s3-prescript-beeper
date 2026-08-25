/* BUZZER 组件: 自 INSTRUCTION 拆出(v1.03 后)
 * 原实现见 INSTRUCTION.c 历史; 全部状态与调度原样迁移, 仅更名 bz_ 前缀 */
#include "BUZZER.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_random.h"

static uint32_t bz_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static volatile uint8_t  bz_remain;     /* 剩余蜂鸣次数 */
static volatile uint8_t  bz_sounding;   /* 正在响 */
static volatile uint32_t bz_stop;       /* 本次蜂鸣结束时刻 */
static volatile uint32_t bz_next;       /* 下次蜂鸣开始时刻 */
static volatile uint8_t  bz_enabled = 1;/* 蜂鸣总开关(设置可调) */

void BUZZER_Init(void)
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

static void bz_gpio_on(void)  { gpio_set_level(GPIO_NUM_15, 0); }  /* 低=响 */
static void bz_gpio_off(void) { gpio_set_level(GPIO_NUM_15, 1); }  /* 高=静 */

void BUZZER_Beep(uint8_t times)
{
    if (bz_sounding)
    {
        bz_gpio_off();          /* 正在响: 先断 GPIO 再重新调度, 防卡响 */
        bz_sounding = 0;
    }
    bz_remain = times;
    bz_next = bz_now_ms() + 30; /* 立即开始 */
}

void BUZZER_SetEnable(uint8_t on)
{
    bz_enabled = on ? 1 : 0;
    if (!bz_enabled && bz_sounding)
    {
        bz_gpio_off();          /* 正在响则立即停 */
        bz_sounding = 0;
    }
}

void BUZZER_Stop(void)
{
    bz_gpio_off();
    bz_sounding = 0;
    bz_remain = 0;
}

void BUZZER_Tick(void)
{
    uint32_t now = bz_now_ms();
    if (!bz_enabled)
    {
        return;                       /* 蜂鸣已关, 不启动蜂鸣 */
    }
    if (!bz_sounding && bz_remain > 0 && now >= bz_next)
    {
        bz_gpio_on();
        bz_sounding = 1;
        bz_stop = now + BZ_BEEP_MS;
        bz_remain--;
    }
    else if (bz_sounding && now >= bz_stop)
    {
        bz_gpio_off();
        bz_sounding = 0;
        bz_next = now + BZ_BEEP_MIN +
                  esp_random() % (BZ_BEEP_MAX - BZ_BEEP_MIN);
    }
}
