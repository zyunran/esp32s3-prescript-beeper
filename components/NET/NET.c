/* NET 组件: WiFi STA 联网 + SNTP 校时 + 心知天气 3 日预报(路径1: 完全按需、离线优先)
 *  - 射频默认关闭: 只有 联网->连接网络(NET_Connect) 手动打开; 不再空闲自动断, 避免 OTA 下载中断
 *  - 会话结束(NET_SessionEnd / NET_WifiStop): 静止射频(省电 + 零网络暴露面); 唤醒/待机不再自动重连
 *  - STA 已连时再按"连接网络" = 手动断开; 网页每个请求都 NET_Touch() 续期, 防气象屏期间被误断
 *  - 拿到 IP 后后台拉取一次 3 日天气(会话短, 无驻留周期刷新); 今日天气显示在时钟下方(UI 组件)
 *  - 天气为 3 日预报, 有效窗口 72h: 窗口内按时显示, 超期且无新校正则隐藏(离线看板)
 *  - DS1302 硬件 RTC: 上电(main.c)有效则直接采用 -> 开机即显示时间;
 *    SNTP 校时回调写回 DS1302(校准/初始化, 断电由模块电池继续走时)
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
#include "net_priv.h"

static const char *TAG = "NET";

/* ================= 可调参数(默认值; 运行期可用 WEB 配置页改, 存 NVS "net") =================
 * 隐私说明: 工程不内置任何私人 WiFi / 天气 API key —— 首次须经 联网->配网 用网页保存,
 * 或直接写入 NVS "net" 命名空间(ssid/pass/city/key). 防止源码公开时泄露个人凭据. */
#define NET_CITY_DEFAULT    "chengdu"     /* 心知天气城市(拼音, 如 beijing/shanghai) */
#define NET_AP_SSID         "ESP32ODERAP"     /* 配网/配置页热点(手机连上后开 192.168.4.1) */
/* 热点密码不再硬编码(公开仓库曾内置, 任何人可连热点): 每台设备首次启动随机生成 8 位并存 NVS(见 net_ap_pass) */
/* 国内 NTP 服务器(UDP 123, pool.ntp.org 在国内常不通, 换国内源可靠) */
#define SNTP_SERVER0   "cn.pool.ntp.org"
#define SNTP_SERVER1   "ntp.aliyun.com"
#define SNTP_SERVER2   "ntp.tencent.com"


#define NET_WEATHER_VALID_SEC  (72UL * 3600)  /* 路径1: 3 日预报有效窗口=72h, 超期无新校正则隐藏 */

/* 运行期配置(NVS "net" 命名空间: ssid/pass/city/key) */
static char net_ssid[33] = "";
static char net_pass[65] = "";
char net_city[24] = NET_CITY_DEFAULT;
char net_key[48] = "";   /* 天气私钥: 默认空=不拉天气, 由网页填写(不内置 key) */
/* 运行期配置跨任务互斥(M4): WEB 写 city/key, 天气任务取快照; 自旋锁只护纯内存拷贝(临界区内无 NVS) */
portMUX_TYPE net_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t net_cfg_valid = 0;   /* 1=用户已通过网页配置过 WiFi(否则纯AP配网模式) */
static uint8_t sta_retry = 0;       /* STA 连续断连次数(≤2 后静默, 防反复扫描发烫/扰热点) */

static uint8_t net_wifi_ok = 0;   /* WiFi 已连上 */
static uint8_t net_time_ok = 0;   /* 时间已同步 */
static uint8_t net_radio_on = 0;  /* 射频已启动(esp_wifi_start 成功且未被 stop): 按需会话的"在网"标志 */
static uint8_t net_manual_off = 0;/* 本次断开为手动交接(NET_SessionEnd): 抑制 DISCONNECTED 事件里的自动重连 */
static uint32_t net_last_activity = 0;  /* 会话最近一次活动(NET_Connect/NET_Touch)时刻 ms(保留给外部查询) */

/* ================= AP 配网热点状态(省电: 无客户端超时自动关) ================= */
static uint8_t  net_ap_client = 0;           /* 1=有手机连着热点 */
static uint8_t  net_ap_timeout_task_run = 0; /* 超时检查任务是否在跑 */
static uint32_t net_ap_open_ms = 0;          /* 热点开启时刻(esp_timer ms) */
static char     net_ap_pass[16] = "";        /* 配网热点密码: 每台随机生成(去 0O1lI 混淆字形), NVS "net"/"appass" 持久化 */
static TaskHandle_t net_dns_task_h = NULL;   /* captive portal DNS 任务句柄(仅热点开启时运行) */
static volatile uint8_t net_dns_active = 0;  /* 1=DNS 任务运行; 置 0 后任务自行收尾关 fd(不外部强删, 防 socket 泄漏) */

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

/* 统一发射功率: 每次射频启动后 8dBm≈6.3mW(降温省电, 家用/配网足够).
 * esp_wifi_stop/start 会把功率重置回默认 20dBm, 故每次启动后必须重设. */
static void net_tx_power(void)
{
    esp_wifi_set_max_tx_power(8);
}

/* 统一射频启停(路径1): 所有 esp_wifi_start/stop 都走这里, 保证 net_radio_on 与真实状态一致,
 * 避免某条路径只 stop 不置标志, 导致 NET_Connect 对"射频是否在跑"误判. */
static void net_radio_stop(void)
{
    esp_wifi_stop();
    net_radio_on = 0;
}

static void net_radio_start(void)
{
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK)
    {
        ESP_LOGW(TAG, "wifi start: %s", esp_err_to_name(e));
        return;
    }
    net_radio_on = 1;
    net_tx_power();   /* 启动会重置功率, 统一重设 8dBm */
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
        if (!net_radio_on)   /* 射频已被外部关停(待机/会话结束): AP 管理无意义, 退出(防待机复拉射频) */
        {
            net_ap_timeout_task_run = 0;
            break;
        }
        wifi_mode_t mode = WIFI_MODE_NULL;   /* 防御初始化: get_mode 失败时不读未初始化值 */
        esp_wifi_get_mode(&mode);
        if (mode != WIFI_MODE_APSTA)
        {
            net_ap_timeout_task_run = 0;   /* 热点已被关, 任务结束 */
            break;
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (net_ap_client || now - net_ap_open_ms < NET_AP_TIMEOUT_MS) continue;
        ESP_LOGI(TAG, "AP no client for %u min, auto off (cool down)", NET_AP_TIMEOUT_MS / 60000);
        net_radio_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        net_radio_start();
        net_ap_timeout_task_run = 0;
        net_dns_active = 0;   /* DNS 随超时关热点一起停(任务自行收尾关 fd) */
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
            net_manual_off = 0;   /* 新一轮会话开始: 清手动断标志, 之后掉线恢复自动重连 */
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
            if (net_manual_off)        /* 路径1: 用户手动断开(NET_SessionEnd), 不自动重连 */
            {
                net_manual_off = 0;
                ESP_LOGI(TAG, "STA session manually ended");
            }
            else
            {
                sta_retry++;
                if (net_ssid[0] && sta_retry <= 1)   /* 初始1次+重试1次, 之后静默: 热点更稳定+降温 */
                {
                    ESP_LOGW(TAG, "WiFi disconnected(reason=%d), retry %u", e ? (int)e->reason : -1, sta_retry);
                    esp_wifi_connect();
                }
                else
                {
                    ESP_LOGW(TAG, "WiFi unavailable(reason=%d) - AP config page at 192.168.4.1", e ? (int)e->reason : -1);
                }
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
            if (xTaskCreate(net_weather_task, "weather", 12288, NULL, 1, NULL) != pdPASS)
            {
                net_weather_fetched = 0;   /* 任务分配失败: 复位标志, 下次 GOT_IP 再试(防 busy 永久卡死) */
                net_weather_busy = 0;
                ESP_LOGW(TAG, "weather task create failed, will retry");
            }
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
        net_dns_task_h = NULL;   /* 自清句柄: 外部只置 net_dns_active=0, 不 vTaskDelete */
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
        net_dns_task_h = NULL;
        vTaskDelete(NULL);
        return;
    }
    /* 非阻塞 + 20ms 轮询: 关热点时外部置 net_dns_active=0, 本任务自行退出并 close(fd),
     * 避免外部 vTaskDelete 强删阻塞中的任务导致 socket(FD)永久泄漏(清单 M1) */
    fcntl(sock, F_SETFL, O_NONBLOCK);
    uint8_t buf[512];
    while (net_dns_active)
    {
        fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (len < 0)
        {
            vTaskDelay(20 / portTICK_PERIOD_MS);   /* 无数据: 轮询, 也让关停信号尽快生效 */
            continue;
        }
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
    close(sock);             /* 收尾: 关键一步, 关 fd 防泄漏 */
    net_dns_task_h = NULL;   /* 自清句柄: 允许下次开启热点重建 */
    vTaskDelete(NULL);
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
        n = sizeof(net_ap_pass);
        if (nvs_get_str(h, "appass", net_ap_pass, &n) != ESP_OK) net_ap_pass[0] = '\0';
        nvs_close(h);
    }
    /* AP 密码首次确保: 无则随机生成 8 位(易辨认字符集), 持久化; 不改则跨重启稳定 */
    if (net_ap_pass[0] == '\0')
    {
        static const char cs[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
        uint8_t k;
        for (k = 0; k < 8; k++) net_ap_pass[k] = cs[esp_random() % (sizeof(cs) - 1)];
        net_ap_pass[8] = '\0';
        if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK)
        {
            nvs_set_str(h, "appass", net_ap_pass);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "AP pass generated (privacy: per-device, not hardcoded)");
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

/* 联网会话内启动/重启 SNTP 校时(路径1: SNTP 只在会话内跑, 平时射频关, 不挂空转协议栈) */
static uint8_t net_sntp_started = 0;
static void net_sntp_start(void)
{
    if (!net_sntp_started)
    {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, SNTP_SERVER0);
        esp_sntp_setservername(1, SNTP_SERVER1);
        esp_sntp_setservername(2, SNTP_SERVER2);
        esp_sntp_set_time_sync_notification_cb(net_sntp_synced);
        esp_sntp_init();
        net_sntp_started = 1;
    }
    else
    {
        esp_sntp_restart();   /* 已初始化(不远的会话重连): 立即重新校时, 消除 DS1302 漂移 */
    }
}

void NET_Init(void)
{
    net_cfg_load();   /* 载入持久化 WiFi/城市 */

    setenv("TZ", "CST-8", 1);        /* 东八区, 无夏令时(离线也需要: main.c 用 mktime 换算 DS1302 时间) */
    tzset();

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
        size_t sl = strnlen(net_ssid, sizeof(wcfg.sta.ssid) - 1);
        memcpy(wcfg.sta.ssid, net_ssid, sl);
        wcfg.sta.ssid[sl] = '\0';   /* 满长也确保 NUL, 防 WiFi 库 strlen 越界; wifi_sta_config_t 无 ssid_len 字段(NUL 结尾协议), 32B SSID 是 ESP-IDF 接口固有限制 */
        size_t pl = strnlen(net_pass, sizeof(wcfg.sta.password) - 1);
        memcpy(wcfg.sta.password, net_pass, pl);
        wcfg.sta.password[pl] = '\0';
    }

    /* 默认纯 STA(不开配网热点, 省电/隐私); 配网靠 联网->配网 手动开热点 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (net_cfg_valid) esp_wifi_set_config(WIFI_IF_STA, &wcfg);

    /* 路径1 完全按需: 射频在此不启动(不自动连网、不挂 SNTP) —— 默认离线, 零功耗/零暴露面.
     * 由 联网->连接网络(NET_Connect) 按需打开射频并校时. */
    ESP_LOGI(TAG, "NET ready (radio OFF; 联网按需 NET_Connect; 配网热点需 联网->配网 手动开启)");
}

/* 当前配网热点状态(只读查询: 射频模式为 AP+STA 即视为开启) */
uint8_t NET_ApOn(void)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return 0;
    return (mode == WIFI_MODE_APSTA) ? 1 : 0;
}

/* 开启配网: 手动开/关配网热点(纯 STA <-> AP+STA), 返回 1=已开 0=已关 */
uint8_t NET_ApToggle(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;   /* 防御初始化: get_mode 失败时不读未初始化值 */
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA)
    {
        net_radio_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        net_radio_start();
        net_ap_client = 0;   /* 复位客户端状态 */
        net_dns_active = 0;  /* DNS 随热点关闭(任务自行收尾关 fd) */
        ESP_LOGI(TAG, "AP off -> STA only");
        return 0;
    }
    else
    {
        wifi_config_t acfg = {0};
        strncpy((char *)acfg.ap.ssid, NET_AP_SSID, sizeof(acfg.ap.ssid) - 1);
        acfg.ap.ssid_len = (uint8_t)strlen(NET_AP_SSID);
        strncpy((char *)acfg.ap.password, net_ap_pass, sizeof(acfg.ap.password) - 1);
        acfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        acfg.ap.max_connection = 4;
        net_radio_stop();
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &acfg);
        net_radio_start();   /* 内部含 net_tx_power 重设(防 stop/start 重置回 20dBm) */
        net_ap_open_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (!net_dns_task_h)
        {
            net_dns_active = 1;
            xTaskCreate(net_dns_task, "dns", 3072, NULL, 1, &net_dns_task_h);  /* captive portal 随热点启动 */
        }
        if (!net_ap_timeout_task_run)
        {
            net_ap_timeout_task_run = 1;
            xTaskCreate(net_ap_timeout_task, "ap_tmo", 3072, NULL, 1, NULL);
        }
        ESP_LOGI(TAG, "AP on: %s (192.168.4.1)", NET_AP_SSID);
        return 1;
    }
}

/* ================= 运行期配置接口(WEB 配置页用) ================= */

/* 读取 WiFi/城市(返回指针, 勿改) */
const char *NET_GetSsid(void) { return net_ssid; }
const char *NET_GetPass(void) { return net_pass; }
const char *NET_GetCity(void) { return net_city; }
const char *NET_GetApSsid(void) { return NET_AP_SSID; }
const char *NET_GetApPass(void) { return net_ap_pass; }

/* 延迟重连任务: 网页保存 WiFi 后, 等保存响应发出去再重连(避免射频切换打断保存请求) */
static void net_reconnect_task(void *arg)
{
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_wifi_disconnect();
    if (net_ssid[0])
    {
        esp_wifi_connect();
        ESP_LOGI(TAG, "reconnect: connecting to saved WiFi (SSID hidden for privacy)");
    }
    vTaskDelete(NULL);
}

/* 设置 WiFi/天气城市并持久化; WiFi 改动立即应用并重连.
 * 只在射频开着时动硬件; 无论 STA 还是配网 APSTA 会话: 只写新 STA 配置 + 延迟重连,
 * 不重启射频 —— 保证网页"保存"响应先发出去(重启射频会掐掉负责本请求的链路). */
void NET_SetWifi(const char *ssid, const char *pass)
{
    /* 网页每次「保存到设备」都会带着 WiFi 字段进来(密码为掩码=沿用旧值):
     * ssid/pass 均未变化时直接返回 —— 否则改个颜色/闹钟保存一次就断连重连一次
     * (重连会触发 GOT_IP 重拉天气, 云端 MQTT 也随之掉线一次) */
    if (strcmp(net_ssid, ssid) == 0 && strcmp(net_pass, pass) == 0)
    {
        return;
    }
    strncpy(net_ssid, ssid, sizeof(net_ssid) - 1);
    net_ssid[sizeof(net_ssid) - 1] = '\0';
    strncpy(net_pass, pass, sizeof(net_pass) - 1);
    net_pass[sizeof(net_pass) - 1] = '\0';
    net_cfg_valid = 1;
    net_cfg_save();

    wifi_config_t wcfg = {0};
    {
        size_t sl = strnlen(net_ssid, sizeof(wcfg.sta.ssid) - 1);
        memcpy(wcfg.sta.ssid, net_ssid, sl);
        wcfg.sta.ssid[sl] = '\0';   /* 满长也确保 NUL, 防 WiFi 库 strlen 越界; wifi_sta_config_t 无 ssid_len 字段(NUL 结尾协议), 32B SSID 是 ESP-IDF 接口固有限制 */
        size_t pl = strnlen(net_pass, sizeof(wcfg.sta.password) - 1);
        memcpy(wcfg.sta.password, net_pass, pl);
        wcfg.sta.password[pl] = '\0';
    }

    if (net_radio_on)
    {
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        xTaskCreate(net_reconnect_task, "recon", 2048, NULL, 1, NULL);  /* 延迟重连: 让保存响应先发给网页 */
    }
}

/* 清除后处理任务: 等网页的 {"ok":1} 响应先发出去, 再全停射频并按配网模式开热点(供手机重新配网) */
static void net_clearwifi_task(void *arg)
{
    vTaskDelay(600 / portTICK_PERIOD_MS);   /* 延迟: 让响应送达浏览器, 避免射频操作掐断本请求 */
    wifi_mode_t mode = WIFI_MODE_NULL;   /* 防御初始化: get_mode 失败时不读未初始化值 */
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA)            /* 先归位纯 STA(否则 NET_ApToggle 会误判为"关热点") */
    {
        net_radio_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        net_radio_start();
    }
    NET_ApToggle();                         /* 回配网模式: 开启热点(此时 mode=STA, 走"开"分支) */
    vTaskDelete(NULL);
}

/* 清除已存 WiFi(网页"清除"按钮): 删 ssid/pass, 回到纯 AP 配网模式(清空后自动开配网热点) */
void NET_ClearWifi(void)
{
    net_ssid[0] = '\0';
    net_pass[0] = '\0';
    net_cfg_valid = 0;
    net_cfg_save();                 /* 空 ssid/pass 写回 NVS: 下次 net_cfg_load 判定为纯 AP */
    if (net_radio_on)
    {
        xTaskCreate(net_clearwifi_task, "clrwifi", 2048, NULL, 1, NULL);   /* 响应先发, 射频后动 */
    }
    ESP_LOGI(TAG, "saved WiFi cleared (配网热点稍候自动开启)");
}

void NET_SetCity(const char *city)
{
    portENTER_CRITICAL(&net_mux);
    strncpy(net_city, city, sizeof(net_city) - 1);
    net_city[sizeof(net_city) - 1] = '\0';
    portEXIT_CRITICAL(&net_mux);
    net_cfg_save();
    net_weather_ok = 0;              /* 城市变了: 等下次刷新重新拉 */
    net_weather_fetched = 0;
}

const char *NET_GetKey(void) { return net_key; }

void NET_SetKey(const char *key)
{
    portENTER_CRITICAL(&net_mux);
    strncpy(net_key, key, sizeof(net_key) - 1);
    net_key[sizeof(net_key) - 1] = '\0';
    portEXIT_CRITICAL(&net_mux);
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
            snprintf(ssids[i], 33, "%.32s", ap[i].ssid);   /* %s 可能读到未 NUL 结尾的 32B ssid 越界; %.32s 限制 */
            rssi[i] = ap[i].rssi;
            enc[i] = (ap[i].authmode == WIFI_AUTH_OPEN) ? 0 : 1;
        }
    }
    free(ap);
    return (uint8_t)num;
}

/* 按需开启联网会话(路径1): 射频没开则启动+立即校时, 然后连已存 WiFi; 记为会话活动起点 */
void NET_Connect(void)
{
    if (!net_radio_on)
    {
        net_radio_start();                     /* 启动射频(STA_START 事件会自动连已存 WiFi) */
        if (!net_radio_on) return;             /* 启动失败(如射频忙): 下次再按即可 */
        net_sntp_start();                      /* 会话内立即校时, 消除 DS1302 长期漂移 */
    }
    else if (net_ssid[0] && !net_wifi_ok)
    {
        esp_wifi_connect();                    /* 射频已在跑(如手动断开后): 直接重连 */
    }
    NET_Touch();
}

/* ================= 对外查询 ================= */
uint8_t NET_WifiOk(void) { return net_wifi_ok; }
uint8_t NET_SessionOn(void) { return net_radio_on; }   /* 联网会话进行中(射频开=会话在) */
uint8_t NET_TimeOk(void) { return net_time_ok; }

/* ================= 联网会话停止(路径1) ================= */

/* 全停射频: 待机进浅睡眠 / 纯 STA 手动断开时调用. 最省电、零暴露面. */
void NET_WifiStop(void)
{
    net_radio_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);   /* mode 归位 STA: 若此前开过配网热点, 避免残留 APSTA 配置使再按 配网 误判为"已开" */
    if (net_sntp_started) esp_sntp_stop();   /* 会话结束停 SNTP(仅当启用过才停, 防未初始化调用) */
    net_wifi_ok = 0;
    net_weather_fetched = 0;   /* 下次会话重新拉天气 */
    net_dns_active = 0;        /* 停 WiFi: DNS(captive portal)任务一并停(防配网热点空转耗电) */
}

/* 结束 STA 联网会话(目前仅由 联网->连接网络 手动开关):
 * 配网热点开着时只断 STA 关联(保留热点与配置页), 否则整机射频停. */
void NET_SessionEnd(void)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA)          /* 在配网(AP 开着): 只断 STA, 不动热点 */
    {
        net_manual_off = 1;               /* 抑制 DISCONNECTED 事件里的自动重连 */
        esp_wifi_disconnect();
        net_wifi_ok = 0;
        net_weather_fetched = 0;
        ESP_LOGI(TAG, "STA session ended (AP hotspot kept)");
    }
    else
    {
        net_manual_off = 1;               /* 全停前也置位: esp_wifi_stop 可能触发 DISCONNECTED, 防御重连 */
        NET_WifiStop();
    }
}

/* 会话活动续期: WEB 每个请求调用, 供外部查询/后续策略使用 */
void NET_Touch(void)
{
    net_last_activity = (uint32_t)(esp_timer_get_time() / 1000);
}

/* 距上次会话活动(NET_Connect/NET_Touch)的毫秒数; 未在网返回 UINT32_MAX */
uint32_t NET_SessionIdleMs(void)
{
    if (!net_radio_on)
    {
        return UINT32_MAX;   /* 射频已停: 无会话可言 */
    }
    return (uint32_t)(esp_timer_get_time() / 1000) - net_last_activity;
}

/* 采用外部有效时间(main.c 从 DS1302 读出后调用): 免联网即标记时间有效, 开机直接显示 */
void NET_TimeAdopt(void)
{
    net_time_ok = 1;
}

uint8_t NET_IpStrCopy(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!buf || n == 0) return 0;
    buf[0] = '\0';
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK)
    {
        snprintf(buf, n, IPSTR, IP2STR(&ip.ip));
    }
    return 1;
}

/* 兼容只读接口: 仍返回静态缓冲; 新代码请优先用 *_Copy 版本避免跨任务共享. */
const char *NET_IpStr(void)
{
    static char buf[16];
    NET_IpStrCopy(buf, sizeof(buf));
    return buf;
}

uint8_t NET_DateStrCopy(char *buf, size_t n)
{
    time_t now;
    struct tm t;
    if (!buf || n == 0) return 0;
    if (!net_time_ok)
    {
        snprintf(buf, n, "--");
        return 1;
    }
    time(&now);
    localtime_r(&now, &t);
    snprintf(buf, n, "%02d-%02d", t.tm_mon + 1, t.tm_mday);
    return 1;
}

const char *NET_DateStr(void)
{
    static char buf[8];
    NET_DateStrCopy(buf, sizeof(buf));
    return buf;
}

uint8_t NET_TimeStrCopy(char *buf, size_t n)
{
    time_t now;
    struct tm t;
    if (!buf || n == 0) return 0;
    if (!net_time_ok)
    {
        snprintf(buf, n, "--:--:--");
        return 1;
    }
    time(&now);
    localtime_r(&now, &t);
    snprintf(buf, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    return 1;
}

const char *NET_TimeStr(void)
{
    static char buf[12];
    NET_TimeStrCopy(buf, sizeof(buf));
    return buf;
}

uint8_t NET_WeekStrCopy(char *buf, size_t n)
{
    static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    time_t now;
    struct tm t;
    if (!buf || n == 0) return 0;
    if (!net_time_ok)
    {
        snprintf(buf, n, "--");
        return 1;
    }
    time(&now);
    localtime_r(&now, &t);
    snprintf(buf, n, "%s", names[t.tm_wday]);
    return 1;
}

const char *NET_WeekStr(void)
{
    static char buf[8];
    NET_WeekStrCopy(buf, sizeof(buf));
    return buf;
}

/* 路径1「离线看板」: 3 日预报只有 72h 有效窗口(超出即「今天」已滑出预报区),
 * 超期且无新校正则视为无数据, 各显示面(主屏/详情/状态/彩蛋)统一隐藏 */
static uint8_t net_weather_valid_now(void)
{
    return net_weather_ok && net_time_ok &&   /* 时间未校时(无 DS1302 且未联网)不判窗口, 与 DateStr 同门控 */
           (uint32_t)(time(NULL) - net_weather_at) <= NET_WEATHER_VALID_SEC;
}

uint8_t NET_WeatherStrCopy(char *buf, size_t n)
{
    struct tm t;
    time_t now;
    const char *txt;
    if (!buf || n == 0 || !net_weather_valid_now())
    {
        return 0;
    }
    /* 按当前时段自动选白天/晚上天气(6:00~17:59 白天, 其余晚上) */
    time(&now);
    localtime_r(&now, &t);
    /* 主界面天气区仅 ~106px, 湿度放不下(3字天气词+温度已占满), 故不显示湿度;
     * 湿度在"联网->查看天气"详情页可见(NET_WeatherDayStr). */
    portENTER_CRITICAL(&net_mux);   /* 与天气任务的清空/填充互斥: 防读到全空/半新旧槽 */
    if (net_weather_n < 1)   /* 锁内复核有有效槽: 防 ok 残留(解析失败撤标)与"跑赢清空"读到空槽输出 " /" */
    {
        portEXIT_CRITICAL(&net_mux);
        return 0;
    }
    txt = (t.tm_hour >= NET_DAY_START_HOUR && t.tm_hour < NET_DAY_END_HOUR)
          ? net_w[0].text_day : net_w[0].text_night;
    snprintf(buf, n, "%s %s/%s",
             txt, net_w[0].high, net_w[0].low);
    portEXIT_CRITICAL(&net_mux);
    return 1;   /* 已写入 buf: "雷阵雨 36/24" */
}

const char *NET_WeatherStr(void)
{
    static char buf[32];
    if (!NET_WeatherStrCopy(buf, sizeof(buf)))
    {
        return NULL;
    }
    return buf;   /* "雷阵雨 36/24" */
}

uint8_t NET_WeatherCount(void)
{
    return net_weather_valid_now() ? net_weather_n : 0;
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
    static char buf[NET_WEATHER_DAYS][40];   /* 各 idx 独立缓冲, 避免互相覆盖; 40B 容 7 字天气词(21B)+温度+湿度 */
    if (!net_weather_valid_now() || idx >= net_weather_n)
    {
        return NULL;
    }
    /* 详情页全屏破译显示, 宽度足够, 湿度带 % */
    portENTER_CRITICAL(&net_mux);   /* 与天气任务清空/填充互斥 */
    snprintf(buf[idx], sizeof(buf[idx]), "%s %s %s/%s %s%%",
             net_w[idx].date, net_w[idx].text_day, net_w[idx].high, net_w[idx].low,
             net_w[idx].humidity);
    portEXIT_CRITICAL(&net_mux);
    return buf[idx];   /* "08-12 晴 36/24 75%" */
}

/* 彩蛋(纺织时间): 天气词在3日间随机 + 高低温在真实区间随机 + 湿度30~95%随机(每次调用随机) */
const char *NET_WeatherMadStr(void)
{
    static char buf[40];   /* 7 字天气词(21B)+随机温度+湿度 */
    uint8_t idx;
    int hi, lo, rhi, rlo, hum;
    if (!net_weather_valid_now()) return NULL;
    idx = (uint8_t)(esp_random() % NET_WEATHER_DAYS);   /* 天气词随机(晴/多云/雷阵雨…) */
    portENTER_CRITICAL(&net_mux);   /* 与天气任务清空/填充互斥 */
    hi = atoi(net_w[idx].high);
    lo = atoi(net_w[idx].low);
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    if (hi <= lo) hi = lo + 1;
    rhi = lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
    rlo = lo + (int)(esp_random() % (uint32_t)(rhi - lo + 1));
    hum = 30 + (int)(esp_random() % 66);   /* 湿度 30~95% */
    snprintf(buf, sizeof(buf), "%s %d/%d %d%%", net_w[idx].text_day, rhi, rlo, hum);
    portEXIT_CRITICAL(&net_mux);
    return buf;   /* "雷阵雨 38/29 75%" */
}
