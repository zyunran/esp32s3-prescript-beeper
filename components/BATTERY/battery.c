/* BATTERY 组件: 读 ADC1 电池分压 -> 电量%(见 BATTERY.h 可调分压/阈值)
 * 用 eFuse 校准曲线(esp_adc_cali)换算电压, 12dB 衰减下 ADC 高端非线性也能得到
 * 较准确电压; 校准不可用时回退线性近似. */
#include "BATTERY.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "BAT";
static adc_oneshot_unit_handle_t bat_adc = NULL;
static adc_cali_handle_t bat_cali = NULL;

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
    /* 校准(eFuse 两/多点): ESP32-S3 用 line_fitting 方案; 不可用则回退线性近似 */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CH,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &bat_cali) != ESP_OK)
        bat_cali = NULL;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_line_fitting(&cal_cfg, &bat_cali) != ESP_OK)
        bat_cali = NULL;
#else
    bat_cali = NULL;
#endif
    if (!bat_cali)
    {
        ESP_LOGW(TAG, "adc cali unavailable, fallback to linear");
    }
    if (adc_oneshot_config_channel(bat_adc, BAT_ADC_CH, &ccfg) != ESP_OK)
    {
        ESP_LOGW(TAG, "adc channel cfg failed");
    }
}

uint8_t BAT_GetPct(void)
{
    int raw = 0;
    int32_t mv, v;
    if (!bat_adc || adc_oneshot_read(bat_adc, BAT_ADC_CH, &raw) != ESP_OK || raw <= 0)
    {
        return 255;                          /* 读不到/读零: 视为无电池 */
    }
    if (bat_cali)
    {
        int vout = 0;
        if (adc_cali_raw_to_voltage(bat_cali, raw, &vout) != ESP_OK)
        {
            return 255;                      /* 校准换算失败, 保守视为无电池 */
        }
        mv = vout;
    }
    else
    {
        mv = (int32_t)raw * 3100 / 4096;     /* 线性近似(校准不可用时的兜底) */
    }
    v = mv * BAT_DIV;                        /* 换算到电池端电压 */
    if (v < BAT_V_NONE) return 255;          /* 未接电池(纯USB) */
    if (v <= BAT_V_EMPTY) return 0;
    if (v >= BAT_V_FULL) return 100;
    return (uint8_t)((v - BAT_V_EMPTY) * 100 / (BAT_V_FULL - BAT_V_EMPTY));
}
