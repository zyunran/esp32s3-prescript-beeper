/* BATTERY 组件: 读 ADC1 电池分压 -> 电量%(见 BATTERY.h 可调分压/阈值) */
#include "BATTERY.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "BAT";
static adc_oneshot_unit_handle_t bat_adc = NULL;

void BAT_Init(void)
{
    adc_oneshot_unit_init_cfg_t icfg = {
        .unit_id = BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,        /* 0~3.1V 量程(12位) */
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_new_unit(&icfg, &bat_adc) != ESP_OK)
    {
        bat_adc = NULL;
        ESP_LOGW(TAG, "adc init failed");
        return;
    }
    if (adc_oneshot_config_channel(bat_adc, BAT_ADC_CH, &ccfg) != ESP_OK)
    {
        ESP_LOGW(TAG, "adc channel cfg failed");
    }
}

uint8_t BAT_GetPct(void)
{
    int raw = 0;
    uint32_t mv;
    int32_t v;
    if (!bat_adc || adc_oneshot_read(bat_adc, BAT_ADC_CH, &raw) != ESP_OK || raw <= 0)
    {
        return 255;                          /* 读不到/读零: 视为无电池 */
    }
    mv = (uint32_t)raw * 3100 / 4096;        /* 12位 11dB 衰减, 满量程约 3.1V(线性近似) */
    v = (int32_t)mv * BAT_DIV;               /* 换算到电池端电压 */
    if (v < BAT_V_NONE) return 255;          /* 未接电池(纯USB) */
    if (v <= BAT_V_EMPTY) return 0;
    if (v >= BAT_V_FULL) return 100;
    return (uint8_t)((v - BAT_V_EMPTY) * 100 / (BAT_V_FULL - BAT_V_EMPTY));
}
