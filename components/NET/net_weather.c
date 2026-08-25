/* NET 组件 - 天气子模块: 状态定义 + 心知天气 HTTP 拉取/解析(自 NET.c 拆出)
 * 共享内部状态见 net_priv.h; 会话/事件/配置/API 仍归 NET.c */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "NET.h"
#include "net_priv.h"

static const char *TAG = "NETW";

net_weather_t net_w[NET_WEATHER_DAYS];
uint8_t net_weather_ok = 0;        /* 天气已拉取成功 */
uint8_t net_weather_n = 0;         /* 实际解析天数(API 可能只回 1~2 天) */
volatile uint8_t net_weather_fetched = 0;  /* 本次会话已拉取(断连后重置再拉); volatile: 事件任务与天气任务跨核读写 */
volatile uint8_t net_weather_busy = 0;     /* 天气任务存活(防止断连重连时堆积); volatile: 同上 */
time_t  net_weather_at = 0;        /* 上次拉取成功时刻(epoch; 详情页旧数据时间戳) */
char net_weather_raw[4096];        /* HTTP 响应缓冲(3日+夜间/湿度 JSON 可能逼近 2KB, 放大防截断) */
uint16_t net_weather_raw_len = 0;

/* ================= 天气拉取(HTTP + cJSON) ================= */

/* 累积 HTTP 响应体 */
static esp_err_t net_weather_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        if (net_weather_raw_len + evt->data_len < sizeof(net_weather_raw))
        {
            memcpy(&net_weather_raw[net_weather_raw_len], evt->data, evt->data_len);
            net_weather_raw_len += evt->data_len;
        }
    }
    return ESP_OK;
}

/* 按缓冲容量截断到完整 UTF-8 字符边界: 消除 strncpy 截出的残缺尾字节(显示方框/错位) */
static void net_utf8_clip(char *s, size_t cap)
{
    size_t i = 0, last = 0;
    while (s[i] != '\0')
    {
        size_t len = 1;
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        if (i + len > cap - 1) break;        /* 整个字符放不下: 停, 视为截断 */
        i += len;
        last = i;
    }
    s[last] = '\0';
}

/* 解析 results[0].daily[]: date/text_day/high/low */
static void net_weather_parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *results, *res0, *daily, *d;
    int n, i;
    if (!root)
    {
        ESP_LOGW(TAG, "Weather JSON parse failed");
        return;
    }
    results = cJSON_GetObjectItem(root, "results");
    res0 = results ? cJSON_GetArrayItem(results, 0) : NULL;
    daily = res0 ? cJSON_GetObjectItem(res0, "daily") : NULL;
    n = daily ? cJSON_GetArraySize(daily) : 0;
    if (n > NET_WEATHER_DAYS) n = NET_WEATHER_DAYS;

    /* 天气槽与显示读互斥(net_mux): 解析清空/填充期间, UI 读取不看到半新旧混合或全空槽 */
    portENTER_CRITICAL(&net_mux);
    memset(net_w, 0, sizeof(net_w));   /* 清残留: 解析跳过槽不再把上次/空数据显示为有效(C1) */
    uint8_t filled = 0;
    for (i = 0; i < n; i++)
    {
        cJSON *date, *text, *text_n, *high, *low, *hum;
        d = cJSON_GetArrayItem(daily, i);
        date = cJSON_GetObjectItem(d, "date");
        text = cJSON_GetObjectItem(d, "text_day");
        text_n = cJSON_GetObjectItem(d, "text_night");
        high = cJSON_GetObjectItem(d, "high");
        low  = cJSON_GetObjectItem(d, "low");
        hum  = cJSON_GetObjectItem(d, "humidity");
        if (!date || !text || !high || !low ||
            !cJSON_IsString(date) || !cJSON_IsString(text) ||
            !cJSON_IsString(high) || !cJSON_IsString(low))
        {
            continue;
        }
        /* date "2026-08-12" -> "08-12" */
        if (strlen(date->valuestring) >= 10)
        {
            net_w[filled].date[0] = date->valuestring[5];
            net_w[filled].date[1] = date->valuestring[6];
            net_w[filled].date[2] = '-';
            net_w[filled].date[3] = date->valuestring[8];
            net_w[filled].date[4] = date->valuestring[9];
            net_w[filled].date[5] = '\0';
        }
        else
        {
            strncpy(net_w[filled].date, date->valuestring, sizeof(net_w[filled].date) - 1);
            net_w[filled].date[sizeof(net_w[filled].date) - 1] = '\0';
        }
        strncpy(net_w[filled].text_day, text->valuestring, sizeof(net_w[filled].text_day) - 1);
        net_w[filled].text_day[sizeof(net_w[filled].text_day) - 1] = '\0';
        net_utf8_clip(net_w[filled].text_day, sizeof(net_w[filled].text_day));   /* 截断不切残字 */
        if (text_n && cJSON_IsString(text_n) && text_n->valuestring[0])
        {
            strncpy(net_w[filled].text_night, text_n->valuestring, sizeof(net_w[filled].text_night) - 1);
        }
        else
        {
            strncpy(net_w[filled].text_night, net_w[filled].text_day, sizeof(net_w[filled].text_night) - 1);  /* 缺失回退白天 */
        }
        net_w[filled].text_night[sizeof(net_w[filled].text_night) - 1] = '\0';
        net_utf8_clip(net_w[filled].text_night, sizeof(net_w[filled].text_night));
        strncpy(net_w[filled].high, high->valuestring, sizeof(net_w[filled].high) - 1);
        net_w[filled].high[sizeof(net_w[filled].high) - 1] = '\0';
        strncpy(net_w[filled].low,  low->valuestring,  sizeof(net_w[filled].low) - 1);
        net_w[filled].low[sizeof(net_w[filled].low) - 1] = '\0';
        if (hum && cJSON_IsString(hum) && hum->valuestring[0])
        {
            strncpy(net_w[filled].humidity, hum->valuestring, sizeof(net_w[filled].humidity) - 1);
        }
        else
        {
            strcpy(net_w[filled].humidity, "--");
        }
        net_w[filled].humidity[sizeof(net_w[filled].humidity) - 1] = '\0';
        filled++;                            /* 成功填充的槽计数: 写入始终连续无空洞, net_weather_n 语义正确 */
    }

    net_weather_n = filled;   /* 只记成功填充槽: 失败/跳过槽不再泄露陈旧数据(修 C1) */
    if (filled > 0)
    {
        net_weather_ok = 1;
        net_weather_at = time(NULL);   /* 记拉取时刻(旧数据显示"更新于…"用) */
    }
    else
    {
        net_weather_ok = 0;   /* HTTP 成功但无有效槽(空响应/字段缺失): 撤掉上次 ok, 防主屏显示 " /" 垃圾天气 */
    }
    portEXIT_CRITICAL(&net_mux);
    if (filled > 0)
    {
        ESP_LOGI(TAG, "Weather[0]: %s %s/%s", net_w[0].text_day, net_w[0].high, net_w[0].low);
    }
    cJSON_Delete(root);
}

/* 阻塞拉取一次天气(GET + 解析); URL 用运行期城市拼装 */
static void net_weather_fetch(void)
{
    char url[220];
    char city[24], key[48];          /* 快照: 防拉取中途 net_city/net_key 被网页改动造成半新旧混合 */
    esp_http_client_config_t cfg;
    esp_http_client_handle_t cli;
    esp_err_t err;

    portENTER_CRITICAL(&net_mux);              /* 快照与 WEB 写 city/key 互斥(M4) */
    strncpy(city, net_city, sizeof(city) - 1); city[sizeof(city) - 1] = '\0';
    strncpy(key, net_key, sizeof(key) - 1);    key[sizeof(key) - 1] = '\0';
    portEXIT_CRITICAL(&net_mux);

    if (key[0] == '\0')              /* 天气私钥未配置: 跳过，防无效请求 */
    {
        ESP_LOGW(TAG, "weather key 未配置, 跳过拉取(网页填写后生效)");
        return;
    }
    if (esp_get_free_heap_size() < 40000)   /* 堆紧张: 跳过本次, 防 TLS 时 OOM 卡死 */
    {
        ESP_LOGW(TAG, "heap low(%u), skip weather", (unsigned)esp_get_free_heap_size());
        return;
    }

    snprintf(url, sizeof(url),
             "https://api.seniverse.com/v3/weather/daily.json"
             "?key=%s&location=%s&language=zh-Hans&unit=c&start=0&days=3",
             key, city);

    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.event_handler = net_weather_http_event;
    cfg.buffer_size = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    net_weather_raw_len = 0;
    cli = esp_http_client_init(&cfg);
    if (!cli)
    {
        ESP_LOGW(TAG, "Weather http init failed");
        return;
    }
    err = esp_http_client_perform(cli);
    esp_http_client_cleanup(cli);

    if (err == ESP_OK && net_weather_raw_len > 0)
    {
        net_weather_raw[net_weather_raw_len] = '\0';
        net_weather_parse(net_weather_raw);
    }
    else
    {
        ESP_LOGW(TAG, "Weather fetch failed: %s", esp_err_to_name(err));
    }
}

/* 天气后台任务: 每次连上(GOT_IP)拉取一次即退出.
 * 路径1「按需短会话」里没有驻留周期刷新的场景: 会话空闲即断, 周期刷新既到不了也会拖长会话.
 * 优先级 1(低于 UI 任务 3): TLS 加密 CPU 密集, 避免抢占屏幕刷新导致卡死 */
void net_weather_task(void *arg)
{
    net_weather_fetch();
    net_weather_fetched = 0;   /* 允许后续会话重连时重建任务再拉 */
    net_weather_busy = 0;
    vTaskDelete(NULL);
}

