/* NET 组件: WiFi STA 联网 + SNTP 校时 + 心知天气 3 日预报
 *  - "联网"子菜单手动触发联网(NET_Connect), 断连自动重连
 *  - 时间有效(SNTP 校时或 DS1302 上电采用)后, 主页面左侧显示由 UI 组件负责
 *  - 拿到 IP 后自动后台拉取 3 日天气, 今日天气显示在时钟下方(UI 组件)
 *  - DS1302 硬件 RTC: 上电(main.c)有效则直接采用 -> 开机即显示时间;
 *    SNTP 校时回调写回 DS1302(校准/初始化, 断电由模块电池继续走时)
 * 参考: WIFISTR.c(WiFi STA 事件驱动) + MYRTC.c(SNTP 校时, CST-8)
 * 注意: 本组件假定已先调用 nvs_flash_init()(main 中完成). */
#include "NET.h"
#include "DS1302.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

static const char *TAG = "NET";

/* ================= 可调参数(默认值; 运行期可用 WEB 配置页改, 存 NVS "net") =================
 * 隐私说明: 工程不内置任何私人 WiFi / 天气 API key —— 首次须经 联网->开启配网 用网页保存,
 * 或直接写入 NVS "net" 命名空间(ssid/pass/city/key). 防止源码公开时泄露个人凭据. */
#define NET_CITY_DEFAULT    "chengdu"     /* 心知天气城市(拼音, 如 beijing/shanghai) */
#define NET_AP_SSID         "ESP32ODERAP"     /* 配网/配置页热点(手机连上后开 192.168.4.1) */
#define NET_AP_PASSWORD     "ESP32ODER"       /* 热点密码(WPA2, ≥8位; 改这里即可换) */
/* 国内 NTP 服务器(UDP 123, pool.ntp.org 在国内常不通, 换国内源可靠) */
#define SNTP_SERVER0   "cn.pool.ntp.org"
#define SNTP_SERVER1   "ntp.aliyun.com"
#define SNTP_SERVER2   "ntp.tencent.com"

#define NET_WEATHER_DAYS  3             /* 拉取天数 */
#define NET_WEATHER_REFRESH_MIN  30     /* 天气自动刷新间隔分钟(0=仅连上时拉一次) */

/* 运行期配置(NVS "net" 命名空间: ssid/pass/city/key) */
static char net_ssid[33] = "";
static char net_pass[65] = "";
static char net_city[24] = NET_CITY_DEFAULT;
static char net_key[48] = "";   /* 天气私钥: 默认空=不拉天气, 由网页填写(不内置 key) */
static uint8_t net_cfg_valid = 0;   /* 1=用户已通过网页配置过 WiFi(否则纯AP配网模式) */
static uint8_t sta_retry = 0;       /* STA 连续断连次数(≤2 后静默, 防反复扫描发烫/扰热点) */

static uint8_t net_wifi_ok = 0;   /* WiFi 已连上 */
static uint8_t net_time_ok = 0;   /* 时间已同步 */
static uint8_t net_started = 0;   /* 已开始联网(幂等) */

/* ================= AP 配网热点状态(省电: 无客户端超时自动关) ================= */
static uint8_t  net_ap_client = 0;           /* 1=有手机连着热点 */
static uint8_t  net_ap_timeout_task_run = 0; /* 超时检查任务是否在跑 */
static uint32_t net_ap_open_ms = 0;          /* 热点开启时刻(esp_timer ms) */

/* ================= 天气状态 ================= */
typedef struct {
    char date[8];       /* "MM-DD" */
    char text_day[12];  /* "晴"/"多云"/"雷阵雨"(3字=9B+NUL, 8B会截断残缺) */
    char text_night[12];/* "晚间天气现象" */
    char high[4];       /* "36" */
    char low[4];        /* "24" */
    char humidity[4];   /* "75" */
} net_weather_t;

/* 白天/晚上天气切换: 6:00~17:59 显示白天, 其余显示晚上 */
#define NET_DAY_START_HOUR  6
#define NET_DAY_END_HOUR    18

static net_weather_t net_w[NET_WEATHER_DAYS];
static uint8_t net_weather_ok = 0;        /* 天气已拉取成功 */
static uint8_t net_weather_n = 0;         /* 实际解析天数(API 可能只回 1~2 天) */
static uint8_t net_weather_fetched = 0;   /* 本次会话已拉取(断连后重置再拉) */
static uint8_t net_weather_busy = 0;      /* 天气任务存活(防止断连重连时堆积) */
static time_t  net_weather_at = 0;        /* 上次拉取成功时刻(epoch; 详情页旧数据时间戳) */
static char net_weather_raw[2048];        /* HTTP 响应缓冲 */
static uint16_t net_weather_raw_len = 0;

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
            net_w[i].date[0] = date->valuestring[5];
            net_w[i].date[1] = date->valuestring[6];
            net_w[i].date[2] = '-';
            net_w[i].date[3] = date->valuestring[8];
            net_w[i].date[4] = date->valuestring[9];
            net_w[i].date[5] = '\0';
        }
        else
        {
            strncpy(net_w[i].date, date->valuestring, sizeof(net_w[i].date) - 1);
            net_w[i].date[sizeof(net_w[i].date) - 1] = '\0';
        }
        strncpy(net_w[i].text_day, text->valuestring, sizeof(net_w[i].text_day) - 1);
        net_w[i].text_day[sizeof(net_w[i].text_day) - 1] = '\0';
        if (text_n && cJSON_IsString(text_n) && text_n->valuestring[0])
        {
            strncpy(net_w[i].text_night, text_n->valuestring, sizeof(net_w[i].text_night) - 1);
        }
        else
        {
            strncpy(net_w[i].text_night, net_w[i].text_day, sizeof(net_w[i].text_night) - 1);  /* 缺失回退白天 */
        }
        net_w[i].text_night[sizeof(net_w[i].text_night) - 1] = '\0';
        strncpy(net_w[i].high, high->valuestring, sizeof(net_w[i].high) - 1);
        net_w[i].high[sizeof(net_w[i].high) - 1] = '\0';
        strncpy(net_w[i].low,  low->valuestring,  sizeof(net_w[i].low) - 1);
        net_w[i].low[sizeof(net_w[i].low) - 1] = '\0';
        if (hum && cJSON_IsString(hum) && hum->valuestring[0])
        {
            strncpy(net_w[i].humidity, hum->valuestring, sizeof(net_w[i].humidity) - 1);
        }
        else
        {
            strcpy(net_w[i].humidity, "--");
        }
        net_w[i].humidity[sizeof(net_w[i].humidity) - 1] = '\0';
    }

    net_weather_n = (uint8_t)n;   /* 记实际天数: 只回 1~2 天时 Count/DayStr 不读空槽 */
    if (n > 0)
    {
        net_weather_ok = 1;
        net_weather_at = time(NULL);   /* 记拉取时刻(旧数据显示"更新于…"用) */
        ESP_LOGI(TAG, "Weather[0]: %s %s/%s", net_w[0].text_day, net_w[0].high, net_w[0].low);
    }
    cJSON_Delete(root);
}

/* 阻塞拉取一次天气(GET + 解析); URL 用运行期城市拼装 */
static void net_weather_fetch(void)
{
    char url[220];
    esp_http_client_config_t cfg;
    esp_http_client_handle_t cli;
    esp_err_t err;

    if (net_key[0] == '\0')          /* 天气私钥未配置: 跳过，防无效请求 */
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
             net_key, net_city);

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

/* 天气后台任务: 连上后拉取, 之后每 NET_WEATHER_REFRESH_MIN 分钟自动刷新一次
 * 优先级 1(低于 UI 任务 3): TLS 加密 CPU 密集, 避免抢占屏幕刷新导致卡死 */
static void net_weather_task(void *arg)
{
    if (NET_WEATHER_REFRESH_MIN == 0)
    {
        net_weather_fetch();   /* 仅连上时拉一次 */
        net_weather_fetched = 0;
        net_weather_busy = 0;
        vTaskDelete(NULL);
        return;
    }
    while (net_wifi_ok)
    {
        net_weather_fetch();
        /* 等刷新间隔(2s 步进), 中途断网则退出, 由下次 GOT_IP 重建 */
        for (int i = 0; i < (NET_WEATHER_REFRESH_MIN * 60) / 2 && net_wifi_ok; i++)
        {
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }
    net_weather_fetched = 0;   /* 断线: 允许重连后重建任务重拉 */
    net_weather_busy = 0;
    vTaskDelete(NULL);
}

/* ================= 事件处理 ================= */
static void net_sntp_synced(struct timeval *tv)
{
    time_t now;
    struct tm t;
    net_time_ok = 1;
    /* 校时成功写回 DS1302: 校准误差 + 首次联网初始化空 RTC(断电由模块电池继续走时) */
    now = time(NULL);
    localtime_r(&now, &t);
    DS1302_Write(&t);
    ESP_LOGI(TAG, "SNTP time synced");
}

/* 统一发射功率: 每次 esp_wifi_start 后调用(8dBm≈6.3mW 降温省电, 家用/配网足够).
 * esp_wifi_stop/start 会把功率重置回默认 20dBm, 故 AP 切换/重连后必须重设. */
static void net_tx_power(void)
{
    esp_wifi_set_max_tx_power(8);
}

/* 配网热点超时自动关闭: 开启后持续无手机连接超 5 分钟, 自动回 STA 停广播 beacon(降温省电).
 * 有客户端连着(配网中)不关; 热点被手动关则任务退出. */
#define NET_AP_TIMEOUT_MS  300000
#define NET_AP_TICK_MS     10000

static void net_ap_timeout_task(void *arg)
{
    for (;;)
    {
        vTaskDelay(NET_AP_TICK_MS / portTICK_PERIOD_MS);
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode != WIFI_MODE_APSTA)
        {
            net_ap_timeout_task_run = 0;   /* 热点已被关, 任务结束 */
            break;
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (net_ap_client || now - net_ap_open_ms < NET_AP_TIMEOUT_MS) continue;
        ESP_LOGI(TAG, "AP no client for %u min, auto off (cool down)", NET_AP_TIMEOUT_MS / 60000);
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        net_tx_power();
        net_ap_timeout_task_run = 0;
        break;
    }
    vTaskDelete(NULL);
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        if (id == WIFI_EVENT_STA_START)
        {
            sta_retry = 0;
            if (net_ssid[0]) esp_wifi_connect();   /* 有存好的 WiFi 才自动连 */
        }
        else if (id == WIFI_EVENT_STA_CONNECTED)
        {
            sta_retry = 0;
            net_wifi_ok = 1;
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   /* 调制解调器省电: 联网常态降耗降温 */
            ESP_LOGI(TAG, "WiFi connected");
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
            net_wifi_ok = 0;
            net_weather_fetched = 0;   /* 重连后重新拉天气 */
            sta_retry++;
            if (net_ssid[0] && sta_retry <= 1)       /* 初始1次+重试1次, 之后静默: 热点更稳定+降温 */
            {
                ESP_LOGW(TAG, "WiFi disconnected(reason=%d), retry %u", e ? (int)e->reason : -1, sta_retry);
                esp_wifi_connect();
            }
            else
            {
                ESP_LOGW(TAG, "WiFi unavailable(reason=%d) - AP config page at 192.168.4.1", e ? (int)e->reason : -1);
            }
        }
        else if (id == WIFI_EVENT_AP_STACONNECTED)   /* 配网热点有手机连上: 暂停超时自动关 */
        {
            net_ap_client = 1;
        }
        else if (id == WIFI_EVENT_AP_STADISCONNECTED)
        {
            net_ap_client = 0;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        if (!net_weather_fetched && !net_weather_busy)   /* 已联网且无天气任务在跑 -> 后台拉 */
        {
            net_weather_fetched = 1;
            net_weather_busy = 1;
            xTaskCreate(net_weather_task, "weather", 12288, NULL, 1, NULL);   /* 低优先级防抢UI, 大栈防TLS溢出 */
        }
    }
}

/* ================= 初始化 ================= */
/* 简易 DNS 应答: 热点客户端的任何域名都答成 192.168.4.1,
 * 实现 captive portal —— 手机一连热点就自动弹出配置页, 不用手输地址 */
static void net_dns_task(void *arg)
{
    struct sockaddr_in addr, from;
    socklen_t fromlen;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        vTaskDelete(NULL);
        return;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;)
    {
        fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (len < 12) continue;
        uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
        int qend = 12;
        uint16_t i;
        for (i = 0; i < qdcount; i++)          /* 跳过问题段 */
        {
            while (qend < len && buf[qend] != 0) qend += buf[qend] + 1;
            if (qend >= len) break;
            qend += 5;
        }
        if (qend >= len || qend + 16 > (int)sizeof(buf)) continue;
        buf[2] = 0x81; buf[3] = 0x80;          /* QR=应答, RA */
        buf[6] = 0; buf[7] = 1;                /* 1 条回答 */
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
        buf[qend]     = 0xC0; buf[qend + 1] = 0x0C;   /* 名指针 -> 问题名 */
        buf[qend + 2] = 0; buf[qend + 3] = 1;          /* A */
        buf[qend + 4] = 0; buf[qend + 5] = 1;          /* IN */
        buf[qend + 6] = 0; buf[qend + 7] = 0; buf[qend + 8] = 0; buf[qend + 9] = 60;
        buf[qend + 10] = 0; buf[qend + 11] = 4;        /* rdlength=4 */
        buf[qend + 12] = 192; buf[qend + 13] = 168; buf[qend + 14] = 4; buf[qend + 15] = 1;   /* 192.168.4.1 */
        sendto(sock, buf, qend + 16, 0, (struct sockaddr *)&from, fromlen);
    }
}

/* 加载持久化配置(NVS "net"); 有用户存过的 WiFi 才进入 APSTA, 否则纯AP配网 */
static void net_cfg_load(void)
{
    nvs_handle_t h;
    net_cfg_valid = 0;
    net_ssid[0] = 0;
    net_pass[0] = 0;
    if (nvs_open("net", NVS_READONLY, &h) == ESP_OK)
    {
        size_t n;
        n = sizeof(net_ssid);
        if (nvs_get_str(h, "ssid", net_ssid, &n) == ESP_OK && net_ssid[0])
        {
            net_cfg_valid = 1;             /* 用户配置过 WiFi */
        }
        if (!net_cfg_valid) net_ssid[0] = 0;
        n = sizeof(net_pass);
        if (nvs_get_str(h, "pass", net_pass, &n) != ESP_OK)
        {
            net_pass[0] = '\0';   /* 密码缺失: 保持空, 由网页重新填写(无内置回退) */
        }
        n = sizeof(net_city);
        if (nvs_get_str(h, "city", net_city, &n) != ESP_OK) strcpy(net_city, NET_CITY_DEFAULT);
        n = sizeof(net_key);
        if (nvs_get_str(h, "key", net_key, &n) != ESP_OK)
        {
            net_key[0] = '\0';   /* 无 key: 保持未配置(不内置任何 key) */
        }
        nvs_close(h);
    }
}

static void net_cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_str(h, "ssid", net_ssid);
        nvs_set_str(h, "pass", net_pass);
        nvs_set_str(h, "city", net_city);
        nvs_set_str(h, "key", net_key);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void net_sntp_init(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER0);
    esp_sntp_setservername(1, SNTP_SERVER1);
    esp_sntp_setservername(2, SNTP_SERVER2);
    esp_sntp_set_time_sync_notification_cb(net_sntp_synced);
    esp_sntp_init();

    setenv("TZ", "CST-8", 1);        /* 东八区, 无夏令时 */
    tzset();
}

void NET_Init(void)
{
    net_cfg_load();   /* 载入持久化 WiFi/城市 */

    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &net_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &net_event_handler, NULL);
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    /* STA: 存好的 WiFi(用户配置过才设, 否则不连) */
    wifi_config_t wcfg = {0};
    if (net_cfg_valid)
    {
        memcpy(wcfg.sta.ssid, net_ssid, strnlen(net_ssid, sizeof(wcfg.sta.ssid)));
        memcpy(wcfg.sta.password, net_pass, strnlen(net_pass, sizeof(wcfg.sta.password)));
    }

    /* 默认纯 STA(不开配网热点, 省电/隐私); 配网靠 联网->开启配网 手动开热点 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (net_cfg_valid) esp_wifi_set_config(WIFI_IF_STA, &wcfg);

    net_sntp_init();
    esp_wifi_start();
    net_tx_power();                  /* 统一发射功率 8dBm(降温省电, 见 net_tx_power) */
    xTaskCreate(net_dns_task, "dns", 3072, NULL, 1, NULL);   /* captive portal: 域名->192.168.4.1(开启配网时生效) */
    net_started = 1;
    ESP_LOGI(TAG, "NET STA: %s (配网热点需 联网->开启配网 手动开启)", net_cfg_valid ? net_ssid : "(未配置)");
}

/* 开启配网: 手动开/关配网热点(纯 STA <-> AP+STA), 返回 1=已开 0=已关 */
uint8_t NET_ApToggle(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA)
    {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        net_tx_power();
        net_ap_client = 0;   /* 复位客户端状态 */
        ESP_LOGI(TAG, "AP off -> STA only");
        return 0;
    }
    else
    {
        wifi_config_t acfg = {0};
        strncpy((char *)acfg.ap.ssid, NET_AP_SSID, sizeof(acfg.ap.ssid) - 1);
        acfg.ap.ssid_len = (uint8_t)strlen(NET_AP_SSID);
        strncpy((char *)acfg.ap.password, NET_AP_PASSWORD, sizeof(acfg.ap.password) - 1);
        acfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        acfg.ap.max_connection = 4;
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &acfg);
        esp_wifi_start();
        net_tx_power();   /* 开热点后重设功率(防 stop/start 重置回 20dBm) */
        net_ap_open_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (!net_ap_timeout_task_run)
        {
            net_ap_timeout_task_run = 1;
            xTaskCreate(net_ap_timeout_task, "ap_tmo", 3072, NULL, 1, NULL);
        }
        ESP_LOGI(TAG, "AP on: %s pwd=%s (192.168.4.1)", NET_AP_SSID, NET_AP_PASSWORD);
        return 1;
    }
}

/* ================= 运行期配置接口(WEB 配置页用) ================= */

/* 读取 WiFi/城市(返回指针, 勿改) */
const char *NET_GetSsid(void) { return net_ssid; }
const char *NET_GetPass(void) { return net_pass; }
const char *NET_GetCity(void) { return net_city; }
const char *NET_GetApSsid(void) { return NET_AP_SSID; }
const char *NET_GetApPass(void) { return NET_AP_PASSWORD; }

/* 延迟重连任务: 网页保存 WiFi 后, 等保存响应发出去再重连(避免射频切换打断保存请求) */
static void net_reconnect_task(void *arg)
{
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_wifi_disconnect();
    if (net_ssid[0])
    {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Connecting to WiFi: %s ...", net_ssid);
    }
    vTaskDelete(NULL);
}

/* 设置 WiFi/天气城市并持久化; WiFi 改动立即应用并重连.
 * 已是 APSTA 时只改 STA 配置并延迟重连(不重启射频, 热点不掉, 网页能收到"已保存") */
void NET_SetWifi(const char *ssid, const char *pass)
{
    strncpy(net_ssid, ssid, sizeof(net_ssid) - 1);
    net_ssid[sizeof(net_ssid) - 1] = '\0';
    strncpy(net_pass, pass, sizeof(net_pass) - 1);
    net_pass[sizeof(net_pass) - 1] = '\0';
    net_cfg_valid = 1;
    net_cfg_save();

    wifi_config_t wcfg = {0};
    memcpy(wcfg.sta.ssid, net_ssid, strnlen(net_ssid, sizeof(wcfg.sta.ssid)));
    memcpy(wcfg.sta.password, net_pass, strnlen(net_pass, sizeof(wcfg.sta.password)));

    if (net_started)
    {
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_APSTA)
        {
            esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            xTaskCreate(net_reconnect_task, "recon", 2048, NULL, 1, NULL);  /* 延迟重连: 让保存响应先发给网页 */
        }
        else
        {
            esp_wifi_stop();                            /* 兜底: 非 APSTA 才重启射频 */
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            esp_wifi_start();
            net_tx_power();
        }
    }
}

void NET_SetCity(const char *city)
{
    strncpy(net_city, city, sizeof(net_city) - 1);
    net_city[sizeof(net_city) - 1] = '\0';
    net_cfg_save();
    net_weather_ok = 0;              /* 城市变了: 等下次刷新重新拉 */
    net_weather_fetched = 0;
}

const char *NET_GetKey(void) { return net_key; }

void NET_SetKey(const char *key)
{
    strncpy(net_key, key, sizeof(net_key) - 1);
    net_key[sizeof(net_key) - 1] = '\0';
    /* 留空 = 不配置天气(不做任何内置 key 回退, 防泄露) */
    net_cfg_save();
    net_weather_ok = 0;              /* 私钥变了: 等下次刷新重新拉 */
    net_weather_fetched = 0;
}

/* 扫描附近 WiFi(APSTA 下可边开热点边扫); 结果填 ssids/rssi/enc, 返回条数 */
uint8_t NET_ScanWifi(uint8_t max, char ssids[][33], int8_t rssi[], uint8_t enc[])
{
    uint16_t num = 0, i;
    wifi_ap_record_t *ap;
    esp_err_t err;
    if (max == 0) return 0;
    ap = malloc(sizeof(wifi_ap_record_t) * max);
    if (!ap) return 0;
    err = esp_wifi_scan_start(NULL, true);          /* 阻塞扫描 ~2s */
    if (err != ESP_OK)                              /* 可能正忙(如刚连过), 稍候重试一次 */
    {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        esp_wifi_clear_ap_list();
        err = esp_wifi_scan_start(NULL, true);
    }
    if (err != ESP_OK)
    {
        free(ap);
        return 0;
    }
    esp_wifi_scan_get_ap_num(&num);
    if (num > max) num = max;
    if (esp_wifi_scan_get_ap_records(&num, ap) == ESP_OK)
    {
        for (i = 0; i < num; i++)
        {
            snprintf(ssids[i], 33, "%s", ap[i].ssid);
            rssi[i] = ap[i].rssi;
            enc[i] = (ap[i].authmode == WIFI_AUTH_OPEN) ? 0 : 1;
        }
    }
    free(ap);
    return (uint8_t)num;
}

/* 手动联网(开机已自动联网; 此函数用于断网时重新连接) */
void NET_Connect(void)
{
    if (!net_started)
    {
        net_started = 1;
        esp_wifi_start();
    }
    else if (net_ssid[0] && !net_wifi_ok)
    {
        esp_wifi_connect();
    }
}

/* ================= 对外查询 ================= */
uint8_t NET_WifiOk(void) { return net_wifi_ok; }
uint8_t NET_TimeOk(void) { return net_time_ok; }

/* ================= 待机(浅睡眠)前后: 停/启 WiFi ================= */
void NET_WifiStop(void)
{
    esp_wifi_stop();
    net_wifi_ok = 0;
    net_weather_fetched = 0;   /* 重连后重新拉天气 */
}

void NET_WifiStart(void)
{
    esp_wifi_start();          /* STA_START 事件自动重连(存好的 WiFi) */
    net_tx_power();            /* 唤醒后重设发射功率 8dBm(esp_wifi_start 会重置回 20dBm) */
    esp_sntp_restart();        /* 立即重新校时(系统时钟/DS1302 一起回准) */
}

/* 采用外部有效时间(main.c 从 DS1302 读出后调用): 免联网即标记时间有效, 开机直接显示 */
void NET_TimeAdopt(void)
{
    net_time_ok = 1;
}

const char *NET_IpStr(void)
{
    static char buf[16];
    esp_netif_ip_info_t ip;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK)
    {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        return buf;
    }
    return "";
}

const char *NET_DateStr(void)
{
    static char buf[8];
    time_t now;
    struct tm t;
    if (!net_time_ok)
    {
        return "--";
    }
    time(&now);
    localtime_r(&now, &t);
    snprintf(buf, sizeof(buf), "%02d-%02d", t.tm_mon + 1, t.tm_mday);
    return buf;
}

const char *NET_TimeStr(void)
{
    static char buf[12];
    time_t now;
    struct tm t;
    if (!net_time_ok)
    {
        return "--:--:--";
    }
    time(&now);
    localtime_r(&now, &t);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

const char *NET_WeekStr(void)
{
    static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static char buf[8];
    time_t now;
    struct tm t;
    if (!net_time_ok)
    {
        return "--";
    }
    time(&now);
    localtime_r(&now, &t);
    snprintf(buf, sizeof(buf), "%s", names[t.tm_wday]);
    return buf;
}

const char *NET_WeatherStr(void)
{
    static char buf[32];
    struct tm t;
    time_t now;
    const char *txt;
    if (!net_weather_ok)
    {
        return NULL;
    }
    /* 按当前时段自动选白天/晚上天气(6:00~17:59 白天, 其余晚上) */
    time(&now);
    localtime_r(&now, &t);
    txt = (t.tm_hour >= NET_DAY_START_HOUR && t.tm_hour < NET_DAY_END_HOUR)
          ? net_w[0].text_day : net_w[0].text_night;
    /* 主界面天气区仅 ~106px, 湿度放不下(3字天气词+温度已占满), 故不显示湿度;
     * 湿度在"联网->查看天气"详情页可见(NET_WeatherDayStr). */
    snprintf(buf, sizeof(buf), "%s %s/%s",
             txt, net_w[0].high, net_w[0].low);
    return buf;   /* "雷阵雨 36/24" */
}

uint8_t NET_WeatherCount(void)
{
    return net_weather_ok ? net_weather_n : 0;
}

/* 天气数据年龄(秒): 距上次拉取成功; 0=无数据(调用方据此判断是否"旧数据"加时间戳) */
uint32_t NET_WeatherAge(void)
{
    if (!net_weather_ok) return 0;
    return (uint32_t)(time(NULL) - net_weather_at);
}

/* "更新于 MM-DD HH:MM"(上次拉取成功时刻); 无数据或未校时返回 NULL */
const char *NET_WeatherUpdatedStr(void)
{
    static char buf[24];
    struct tm t;
    if (!net_weather_ok || !net_time_ok) return NULL;
    localtime_r(&net_weather_at, &t);
    snprintf(buf, sizeof(buf), "更新于 %02d-%02d %02d:%02d",
             t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    return buf;
}

const char *NET_WeatherDayStr(uint8_t idx)
{
    static char buf[NET_WEATHER_DAYS][32];   /* 各 idx 独立缓冲, 避免互相覆盖 */
    if (!net_weather_ok || idx >= net_weather_n)
    {
        return NULL;
    }
    /* 详情页全屏破译显示, 宽度足够, 湿度带 % */
    snprintf(buf[idx], sizeof(buf[idx]), "%s %s %s/%s %s%%",
             net_w[idx].date, net_w[idx].text_day, net_w[idx].high, net_w[idx].low,
             net_w[idx].humidity);
    return buf[idx];   /* "08-12 晴 36/24 75%" */
}

/* 彩蛋(纺织时间): 天气词在3日间随机 + 高低温在真实区间随机 + 湿度30~95%随机(每次调用随机) */
const char *NET_WeatherMadStr(void)
{
    static char buf[32];
    uint8_t idx;
    int hi, lo, rhi, rlo, hum;
    if (!net_weather_ok) return NULL;
    idx = (uint8_t)(esp_random() % NET_WEATHER_DAYS);   /* 天气词随机(晴/多云/雷阵雨…) */
    hi = atoi(net_w[idx].high);
    lo = atoi(net_w[idx].low);
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    if (hi <= lo) hi = lo + 1;
    rhi = lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
    rlo = lo + (int)(esp_random() % (uint32_t)(rhi - lo + 1));
    hum = 30 + (int)(esp_random() % 66);   /* 湿度 30~95% */
    snprintf(buf, sizeof(buf), "%s %d/%d %d%%", net_w[idx].text_day, rhi, rlo, hum);
    return buf;   /* "雷阵雨 38/29 75%" */
}
