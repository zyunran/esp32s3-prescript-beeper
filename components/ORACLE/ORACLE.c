/* ORACLE 组件: 随机神谕推送
 * 每天在校时段内随机 N 个时刻, 到时 ORACLE_Due 返回 1;
 * 自动跨日/设置变更重排程(与 SETTING 的次数/时段对比).
 * 统计: 每次 ORACLE_Delivered 累计接收次数, NVS "oracle"/"cnt" 持久化. */
#include "ORACLE.h"
#include "SETTING.h"
#include "NET.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include <time.h>

static uint8_t  ora_armed;      /* 已排程 */
static uint8_t  ora_n;          /* 排程条数(与设置对比) */
static uint8_t  ora_win;        /* 排程时段(与设置对比) */
static int      ora_day = -1;   /* 排程日期 */
static uint16_t ora_times[9];   /* 各推送时刻(当日分钟) */
static uint8_t  ora_idx;        /* 下一个待触发 */

/* ================= 接收次数统计(NVS "oracle"/"cnt") ================= */
static uint32_t ora_count = 0;
static uint8_t  ora_count_loaded = 0;

static void ora_count_load(void)
{
    nvs_handle_t h;
    if (nvs_open("oracle", NVS_READONLY, &h) == ESP_OK)
    {
        nvs_get_u32(h, "cnt", &ora_count);
        nvs_close(h);
    }
    ora_count_loaded = 1;
}

static void ora_count_save(void)
{
    nvs_handle_t h;
    if (nvs_open("oracle", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u32(h, "cnt", ora_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int ora_mday(void)
{
    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);
    return t.tm_mday;
}

static uint16_t ora_min_of_day(void)
{
    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);
    return (uint16_t)(t.tm_hour * 60 + t.tm_min);
}

static void ora_schedule(void)
{
    const uint16_t *range = SET_OracleWinRange();
    uint16_t start = range[0], end = range[1];
    uint8_t n = SET_OracleN(), i;

    if (n > 9) n = 9;
    if (n == 0 || end <= start)
    {
        ora_idx = n;              /* 关闭: 永不触发 */
        return;
    }
    for (i = 0; i < n; i++)
    {
        ora_times[i] = (uint16_t)(start + esp_random() % (end - start));
    }
    /* 升序排序, 依次触发 */
    for (i = 1; i < n; i++)
    {
        uint16_t v = ora_times[i];
        uint8_t j = i;
        while (j > 0 && ora_times[j - 1] > v)
        {
            ora_times[j] = ora_times[j - 1];
            j--;
        }
        ora_times[j] = v;
    }
    ora_idx = 0;
}

uint8_t ORACLE_Due(void)
{
    uint8_t n, win;

    if (!NET_TimeOk())
    {
        return 0;                 /* 未校时, 无时间概念 */
    }
    /* 跨日或设置变更 -> 重排程 */
    n = SET_OracleN();
    win = SET_OracleWin();
    if (!ora_armed || n != ora_n || win != ora_win || ora_mday() != ora_day)
    {
        ora_armed = 1;
        ora_n = n;
        ora_win = win;
        ora_day = ora_mday();
        ora_schedule();
    }
    if (ora_idx >= ora_n)
    {
        return 0;                 /* 今日已推完 */
    }
    return (ora_min_of_day() >= ora_times[ora_idx]) ? 1 : 0;
}

void ORACLE_Delivered(void)
{
    ora_idx++;
    if (!ora_count_loaded) ora_count_load();
    ora_count++;
    ora_count_save();
}

uint32_t ORACLE_Count(void)
{
    if (!ora_count_loaded) ora_count_load();
    return ora_count;
}
