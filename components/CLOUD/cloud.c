/* CLOUD 组件主体: 配置(NVS "cloud") + cloud_task 会话管理
 * 会话策略: WiFi 已连且「远程在线」开 -> 拉起 esp-mqtt 连 OneNET(鉴权 token 每次启动现算);
 *   断线由 esp-mqtt 自动重连(10s); 关闭/改配置 -> 停客户端, 下一轮按新配置重建。
 * 数据流: 属性连上即先报一次, 之后 60s 周期上报; 事件队列逐条上报(未启用/队满即丢, 不补发);
 *   下行 display_cmd 在 mqtt 事件回调里解析 -> 单缓冲暂存, ui_task 经 CLOUD_TakeCmd 取走显示
 *   (与网页下发指令同一展示路径, LCD 单写者纪律不变) */
#include "CLOUD.h"
#include "cloud_priv.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "NET.h"
#include "BATTERY.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CLOUD";

typedef struct {
    uint8_t evt;
    char msg[CLOUD_MSG_MAX];
} cloud_evt_item_t;

/* ================= 配置(httpd 任务读写 / cloud_task 读写): 互斥量 + 快照 ================= */
static cloud_cfg_t s_cfg;
static SemaphoreHandle_t s_cfg_mux = NULL;
static const char *CFG_NS = "cloud";

static void cfg_lock(void)   { xSemaphoreTake(s_cfg_mux, portMAX_DELAY); }
static void cfg_unlock(void) { xSemaphoreGive(s_cfg_mux); }

/* ================= 运行态旗标(volatile: mqtt 任务写 / cloud_task·ui_task 读) =================
 *  - s_started/s_online: 客户端已拉起 / MQTT 已连接(KeepAlive 与上报节流依据)
 *  - s_on_shadow: 「远程在线」镜像(KeepAlive 免锁读; 配置锁内更新)
 *  - s_reload: 配置已变, 任务下一轮停旧重建 */
static esp_mqtt_client_handle_t s_client = NULL;
static volatile uint8_t s_started = 0;
static volatile uint8_t s_online = 0;
static volatile uint8_t s_on_shadow = 0;
static volatile uint8_t s_reload = 0;

/* 属性上报节流与序号(仅 cloud_task 访问) */
static uint32_t s_last_report = 0;
static uint8_t  s_report_due = 0;   /* MQTT_EVENT_CONNECTED 置 1: 连上立即先报一次 */
static uint32_t s_seq = 0;
static uint32_t s_alarm_cnt = 0;   /* 闹钟触发累计(跨任务访问, 一律走 __atomic 原子读写, 双核下裸 RMW 会撕裂) */
static uint32_t s_start_block = 0;          /* 启动失败冷却(到点前不再尝试, 防坏 key 刷日志) */

/* ================= 下行指令缓冲(httpd... mqtt 任务写 / ui_task 读): 自旋锁, 同 WEB 模式 ================= */
static char s_cmd[CLOUD_CMD_MAX];
static volatile uint8_t s_cmd_flag = 0;
static portMUX_TYPE s_cmd_mux = portMUX_INITIALIZER_UNLOCKED;

/* ================= 事件队列(main/ui 任务投递 -> cloud_task 消费) ================= */
static QueueHandle_t s_evtq = NULL;   /* 深 4: 事件偶发, 满即丢 */

void CLOUD_GetConfig(cloud_cfg_t *out)
{
    if (!out) return;
    cfg_lock();
    *out = s_cfg;
    cfg_unlock();
}

uint8_t CLOUD_IsOnline(void)  { return s_online; }
uint8_t CLOUD_KeepAlive(void) { return s_on_shadow && s_started; }

uint8_t CLOUD_TakeCmd(char *buf, size_t n)
{
    uint8_t r = 0;
    portENTER_CRITICAL(&s_cmd_mux);
    if (s_cmd_flag)
    {
        if (n > 0)
        {
            strncpy(buf, s_cmd, n - 1);
            buf[n - 1] = '\0';
        }
        s_cmd_flag = 0;
        r = 1;
    }
    portEXIT_CRITICAL(&s_cmd_mux);
    return r;
}

void CLOUD_NotifyEvent(cloud_evt_t e, const char *msg)
{
    cloud_evt_item_t it;
    if (e == CLOUD_EVT_ALARM) __atomic_fetch_add(&s_alarm_cnt, 1, __ATOMIC_RELAXED);   /* 计数与开关无关: 关闭期间也累计 */
    if (!s_started || !s_evtq) return;         /* 未启用: 丢弃(不做离线补发) */
    it.evt = (uint8_t)e;
    it.msg[0] = '\0';
    if (msg)
    {
        strncpy(it.msg, msg, sizeof(it.msg) - 1);
        it.msg[sizeof(it.msg) - 1] = '\0';
    }
    xQueueSend(s_evtq, &it, 0);   /* 队满即丢 */
}

/* ================= NVS ================= */
static void cfg_load(void)
{
    nvs_handle_t h;
    memset(&s_cfg, 0, sizeof(s_cfg));
    if (nvs_open(CFG_NS, NVS_READONLY, &h) == ESP_OK)
    {
        size_t n;
        uint8_t on = 0;
        n = sizeof(s_cfg.pid);
        nvs_get_str(h, "pid", s_cfg.pid, &n);
        n = sizeof(s_cfg.name);
        nvs_get_str(h, "name", s_cfg.name, &n);
        n = sizeof(s_cfg.key);
        nvs_get_str(h, "key", s_cfg.key, &n);
        if (nvs_get_u8(h, "on", &on) == ESP_OK) s_cfg.on = on ? 1 : 0;
        nvs_close(h);
    }
    s_on_shadow = s_cfg.on;
}

static void cfg_save(const cloud_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs open RW fail");
        return;
    }
    nvs_set_u8(h, "on", c->on ? 1 : 0);
    nvs_set_str(h, "pid", c->pid);
    nvs_set_str(h, "name", c->name);
    nvs_set_str(h, "key", c->key);
    nvs_commit(h);
    nvs_close(h);
}

void CLOUD_SetConfig(const cloud_cfg_t *in)
{
    if (!in) return;
    cfg_save(in);            /* NVS 先落盘(不持锁做 IO) */
    cfg_lock();
    s_cfg = *in;
    s_on_shadow = s_cfg.on;
    cfg_unlock();
    s_reload = 1;            /* 任务下一轮: 停旧会话按新配置重建 */
    ESP_LOGI(TAG, "config saved: on=%u pid=%s name=%s", s_on_shadow, in->pid, in->name);
}

/* ================= 会话管理 ================= */
static void cloud_teardown(void)
{
    if (s_client)
    {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_started = 0;
    s_online = 0;
    s_report_due = 0;
}

/* 属性周期上报: battery(无电池跳过) + rssi + 版本 + 闹钟累计 */
static void report_props(const cloud_cfg_t *c)
{
    char topic[CLOUD_TOPIC_MAX], payload[CLOUD_PAYLOAD_MAX];
    int bat = BAT_GetPct();
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    if (bat == 255) bat = -1;   /* 无电池: 上报里跳过该项 */
    uint32_t alarms = __atomic_load_n(&s_alarm_cnt, __ATOMIC_RELAXED);
    s_seq++;
    if (cloud_topic(topic, sizeof(topic), c->pid, c->name, "property/post") != 0) return;
    if (cloud_prop_post_json(payload, sizeof(payload), bat, rssi,
                             esp_app_get_description()->version, alarms, s_seq) != 0) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
}

static void publish_event(const cloud_cfg_t *c, const cloud_evt_item_t *it)
{
    static const char *const evt_id[] = { "alarm_fire", "todo_remind", "daily_sign" };
    char topic[CLOUD_TOPIC_MAX], suffix[32], payload[CLOUD_PAYLOAD_MAX];
    if (it->evt >= sizeof(evt_id) / sizeof(evt_id[0])) return;
    snprintf(suffix, sizeof(suffix), "event/%s/post", evt_id[it->evt]);
    s_seq++;
    if (cloud_topic(topic, sizeof(topic), c->pid, c->name, suffix) != 0) return;
    if (cloud_event_post_json(payload, sizeof(payload), evt_id[it->evt],
                              it->msg, s_seq) != 0) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
}

/* 连上 OneNET: 订阅 属性上报回执 + display_cmd 服务调用, 连上即先报一次属性 */
static void on_mqtt_connected(esp_mqtt_event_handle_t event)
{
    cloud_cfg_t c;
    char topic[CLOUD_TOPIC_MAX];
    CLOUD_GetConfig(&c);
    s_online = 1;
    s_report_due = 1;
    if (cloud_topic(topic, sizeof(topic), c.pid, c.name, "property/post/reply") == 0)
        esp_mqtt_client_subscribe(event->client, topic, 1);
    if (cloud_topic(topic, sizeof(topic), c.pid, c.name, "service/display_cmd/invoke") == 0)
        esp_mqtt_client_subscribe(event->client, topic, 1);
    ESP_LOGI(TAG, "MQTT connected");
}

/* 下行 display_cmd 服务调用: 解析 -> 暂存待 ui_task 显示 + 回执(成功 200/拒收 400) */
static void on_mqtt_data(esp_mqtt_event_handle_t event)
{
    cloud_cfg_t c;
    char full[CLOUD_TOPIC_MAX], jbuf[512], msg[CLOUD_CMD_MAX], id[CLOUD_ID_MAX] = "", reply[64];
    int code = 400;

    CLOUD_GetConfig(&c);
    /* 精确匹配本设备 topic(防其他主题尾缀误配); 片段化报文直接丢弃(实际不应出现) */
    if (event->data_len == event->total_data_len &&
        cloud_topic(full, sizeof(full), c.pid, c.name, "service/display_cmd/invoke") == 0 &&
        event->topic_len == (int)strlen(full) &&
        memcmp(event->topic, full, (size_t)event->topic_len) == 0 &&
        event->data_len < (int)sizeof(jbuf))
    {
        memcpy(jbuf, event->data, event->data_len);
        jbuf[event->data_len] = '\0';
        if (cloud_parse_cmd(jbuf, msg, sizeof(msg), id, sizeof(id)))
        {
            portENTER_CRITICAL(&s_cmd_mux);
            strcpy(s_cmd, msg);
            s_cmd_flag = 1;
            portEXIT_CRITICAL(&s_cmd_mux);
            code = 200;
        }
    }
    if (id[0])   /* 回执仅在 id 白名单通过时发送: 不用未初始化/无法关联的 id 拼 JSON */
    {
        char rtopic[CLOUD_TOPIC_MAX];
        if (cloud_topic(rtopic, sizeof(rtopic), c.pid, c.name,
                        "service/display_cmd/invoke_reply") == 0 &&
            cloud_cmd_reply_json(reply, sizeof(reply), id, code) == 0)
        {
            esp_mqtt_client_publish(event->client, rtopic, reply, 0, 1, 0);
        }
    }
    ESP_LOGI(TAG, "display_cmd: code=%d", code);
}

static void mqtt_event_cb(void *arg, esp_event_base_t base, int32_t id, void *edata)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)edata;
    switch ((esp_mqtt_event_id_t)id)
    {
        case MQTT_EVENT_CONNECTED:    on_mqtt_connected(event); break;
        case MQTT_EVENT_DISCONNECTED: s_online = 0; break;
        case MQTT_EVENT_DATA:         on_mqtt_data(event); break;
        case MQTT_EVENT_ERROR:        ESP_LOGW(TAG, "mqtt error, reconnect auto"); break;
        default: break;
    }
    (void)arg; (void)base;
}

/* 拉起客户端(鉴权 token 现算); 失败置 15s 冷却, 防坏三元组每轮刷日志 */
static void cloud_start(const cloud_cfg_t *c)
{
    char token[CLOUD_TOKEN_MAX];
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (cloud_token_gen(c->pid, c->name, c->key, token, sizeof(token)) != 0)
    {
        ESP_LOGE(TAG, "token gen fail: 检查 DeviceKey(base64)与三元组");
        s_start_block = now + 15000;
        return;
    }
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = "mqtt://mqtts.heclouds.com:1883",
        .credentials = {
            .client_id = c->name,
            .username = c->pid,
            .authentication.password = token,
        },
        .session = { .keepalive = CLOUD_KEEPALIVE_S },
        .network = { .reconnect_timeout_ms = 10000 },   /* 断线自动重连间隔 */
        .buffer = { .size = 1024, .out_size = 1024 },
    };
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client)
    {
        ESP_LOGE(TAG, "mqtt init fail");
        s_start_block = now + 15000;
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_cb, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK)
    {
        ESP_LOGE(TAG, "mqtt start fail(多为堆不足): 15s 后重试");
        esp_mqtt_client_destroy(s_client);   /* 不置 s_started: 防死客户端卡住 KeepAlive/待机门控 */
        s_client = NULL;
        s_start_block = now + 15000;
        return;
    }
    s_started = 1;
    ESP_LOGI(TAG, "mqtt start: pid=%s name=%s", c->pid, c->name);
}

static void cloud_task(void *arg)
{
    for (;;)
    {
        cloud_cfg_t c;
        cloud_evt_item_t it;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        if (s_reload)               /* 配置变更: 停旧会话, 本轮末尾按新配置重建 */
        {
            cloud_teardown();
            s_reload = 0;
        }
        CLOUD_GetConfig(&c);
        if (!c.on || !NET_WifiOk() || !c.pid[0] || !c.name[0] || !c.key[0])
        {
            if (s_started) cloud_teardown();   /* 手动关/断网: 收掉客户端 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!s_started)
        {
            if ((int32_t)(now - s_start_block) < 0)   /* 启动冷却中(差比较防回绕) */
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            cloud_start(&c);
        }
        if (s_online && (s_report_due || now - s_last_report >= CLOUD_REPORT_MS))
        {
            s_report_due = 0;
            s_last_report = now;
            report_props(&c);
        }
        while (s_online && xQueueReceive(s_evtq, &it, 0) == pdTRUE)
        {
            publish_event(&c, &it);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void CLOUD_Init(void)
{
    s_cfg_mux = xSemaphoreCreateMutex();
    s_evtq = xQueueCreate(4, sizeof(cloud_evt_item_t));
    cfg_load();
    if (xTaskCreate(cloud_task, "cloud", 4096, NULL, 2, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "cloud task create fail (云端功能不可用, 其余功能不受影响)");
    }
}
