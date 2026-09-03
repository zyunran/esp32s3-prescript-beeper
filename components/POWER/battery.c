/* BATTERY 组件: 读 ADC1 电池分压 -> 电量%(见 BATTERY.h 可调分压/阈值)
 * 用 eFuse 校准曲线(esp_adc_cali)换算电压, 12dB 衰减下 ADC 高端非线性也能得到
 * 较准确电压; 校准不可用时回退线性近似. */
#include "BATTERY.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"   /* adc 跨任务互斥量 */

static const char *TAG = "BAT";
static uint8_t bat_init_done = 0;              /* 幂等守卫(旧实现重复调用会覆盖已建句柄) */
static adc_oneshot_unit_handle_t bat_adc = NULL;
static adc_cali_handle_t bat_cali = NULL;
static SemaphoreHandle_t bat_mux = NULL;   /* 电量读取互斥(ui_task 与 httpd 任务共用) */

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
    if (bat_init_done)
    {
        return;                          /* 幂等: 防重复初始化覆盖句柄/泄漏 ADC 单元 */
    }
    bat_init_done = 1;
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
    bat_mux = xSemaphoreCreateMutex();
}

/* ADC 读取在 ui_task(每2s)与 httpd 任务(/api/status)都可能调用:
 * esp_adc oneshot 非线程安全; adc_oneshot_read 是阻塞转换读取(轮询硬件完成标志),
 * 故用互斥量(两个调用方都是任务、非 ISR)而非自旋锁, 避免关中断忙等. */

/* 电量精度优化:
 *  - 多次采样去极值平均: 抑制单次 ADC 毛刺
 *  - 低通滤波: 防止 WiFi/蜂鸣/亮屏瞬时负载导致百分比跳动
 *  - 1S 锂电放电曲线分段线性查表: 比 3.0~4.2V 全程线性更接近真实剩余电量 */
#define BAT_SAMPLE_N      8
#define BAT_SAMPLE_MIN    4   /* 有效样本达到该值才去掉最大/最小再平均 */

static int32_t bat_last_mv = -1;   /* 低通滤波后的 ADC 侧电压(未乘分压比); 仅在 bat_mux 内访问 */

static int32_t bat_raw_to_mv(int raw)
{
    if (bat_cali)
    {
        int vout = 0;
        if (adc_cali_raw_to_voltage(bat_cali, raw, &vout) != ESP_OK) return -1;
        return vout;
    }
    return (int32_t)raw * 3100 / 4096;   /* 线性近似(校准不可用时的兜底) */
}

/* 1S 锂电电压 -> 百分比(分段线性插值, 比全程直线更符合实际放电曲线) */
static uint8_t bat_volt_to_pct(int32_t v)
{
    static const uint16_t mv_tab[] = {
        2900, 3200, 3400, 3600, 3700, 3800, 3850, 3900, 3950, 4000, 4100, 4200
    };
    static const uint8_t pct_tab[] = {
        0, 5, 15, 30, 45, 60, 70, 80, 85, 90, 95, 100
    };
    uint8_t i;
    if (v <= mv_tab[0]) return 0;
    if (v >= mv_tab[sizeof(mv_tab) / sizeof(mv_tab[0]) - 1]) return 100;
    for (i = 0; i < sizeof(mv_tab) / sizeof(mv_tab[0]) - 1; i++)
    {
        if (v < mv_tab[i + 1])
        {
            int32_t span = mv_tab[i + 1] - mv_tab[i];
            int32_t off  = v - mv_tab[i];
            return (uint8_t)(pct_tab[i] + (uint8_t)(off * (pct_tab[i + 1] - pct_tab[i]) / span));
        }
    }
    return 100;
}

/* 采样 + 低通滤波, 返回 ADC 侧电压(mV); 失败返回 -1.
 * 调用方必须已持有 bat_mux. */
static int32_t bat_measure_mv_locked(void)
{
    int32_t mv = -1;

    if (!bat_adc) return -1;

    {
        int32_t sum = 0;
        int  valid = 0;
        int  mn = 0x7FFFFFFF, mx = -1;
        uint8_t i;
        for (i = 0; i < BAT_SAMPLE_N; i++)
        {
            int raw = 0;
            if (adc_oneshot_read(bat_adc, BAT_ADC_CH, &raw) == ESP_OK && raw > 0)
            {
                sum += raw;
                if (raw < mn) mn = raw;
                if (raw > mx) mx = raw;
                valid++;
            }
        }
        if (valid >= BAT_SAMPLE_MIN)
        {
            sum -= mn;                         /* 去掉最大/最小, 抑制偶然毛刺 */
            sum -= mx;
            valid -= 2;
        }
        if (valid > 0)
        {
            mv = bat_raw_to_mv((int)(sum / valid));
        }

        if (mv >= 0)
        {
            /* 低通滤波: 新值占 1/4, 历史占 3/4, 显示更稳不跳变 */
            if (bat_last_mv < 0) bat_last_mv = mv;
            else                 bat_last_mv = (bat_last_mv * 3 + mv) / 4;
            mv = bat_last_mv;
        }
    }
    return mv;
}

uint8_t BAT_GetPct(void)
{
    int32_t mv;
    if (!bat_mux || xSemaphoreTake(bat_mux, portMAX_DELAY) != pdTRUE)
    {
        return 255;                          /* 锁不可用: 保守视为无电池 */
    }
    mv = bat_measure_mv_locked();
    xSemaphoreGive(bat_mux);

    if (mv < 0)
    {
        return 255;                          /* 读取/校准失败, 保守视为无电池 */
    }
    {
        int32_t v = mv * BAT_DIV;            /* 换算到电池端电压 */
        if (v < BAT_V_NONE) return 255;      /* 未接电池(纯USB) */
        return bat_volt_to_pct(v);
    }
}

uint16_t BAT_GetMillivolt(void)
{
    int32_t mv;
    if (!bat_mux || xSemaphoreTake(bat_mux, portMAX_DELAY) != pdTRUE)
    {
        return 0;                            /* 锁不可用: 视为无电池 */
    }
    mv = bat_measure_mv_locked();
    xSemaphoreGive(bat_mux);

    if (mv < 0)
    {
        return 0;                            /* 读取/校准失败 */
    }
    {
        int32_t v = mv * BAT_DIV;            /* 换算到电池端电压 */
        if (v < BAT_V_NONE) return 0;        /* 未接电池(纯USB) */
        return (uint16_t)v;
    }
}
