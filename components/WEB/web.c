/* WEB 组件: 内嵌配置页 + REST API
 * 路径1 完全按需: httpd 静态常驻, 但射频平时关闭, 实际可达窗口 = 联网会话 / 配网热点期间
 * (会话空闲超时/手动断/待机即射频停, 此时本服务不可达)。每个请求都 NET_Touch() 续期会话。
 * 联网后手机/PC 浏览器访问 http://<esp32-ip>/ 打开配置页, 可:
 *   - 增删改指令库(每行一条, 支持 {#RRGGBB}/{} {RAND:1-10} {TIMER})
 *   - 设置闹钟(每天重复/按星期/一次性, 最多 3 个) / 待办管理
 *   - 改 UI 主题色(背景/菜单/选中框/图标/时钟/日期) / 音量/蜂鸣/熄屏时长
 *   - 改 WiFi/城市/天气私钥, 扫描附近 WiFi; 设使用者名称
 *   - 设备状态(电量/堆/开机次数/IP/时间/天气), 下发指令(含 made in heaven 彩蛋), 拼点人格图鉴
 * 全部写入 NVS 持久化(命名空间: 指令"ins" / 闹钟"alarm" / 颜色"cfg" / 设置"set" 等), 重启后仍生效.
 * API:
 *   GET  /api/cfg      -> 当前配置 JSON
 *   POST /api/cfg      -> 接收 JSON 应用并保存 {"ok":1}
 *   POST /api/beep|reboot|scan|send     -> 蜂鸣测试/重启/WiFi扫描/下发指令
 *   GET  /api/status|gacha|todo  POST /api/todo -> 设备状态/拼点图鉴/待办增删查
 */
#include "web.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "LCD.h"
#include "UI.h"
#include "INSTRUCTION.h"
#include "BUZZER.h"
#include "ALARM.h"
#include "NET.h"
#include "SETTING.h"
#include "GACHA.h"
#include "TODO.h"
#include "ANSWER.h"
#include "BATTERY.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "WEB";

/* 凭据脱敏掩码: /api/cfg 不回传 WiFi 密码/天气 key 明文(该端口 LAN 内任意设备可访问);
 * 配置页以掩码占位, 保存时遇到该串视为"保持不变", 不覆盖已存凭据 */
#define WEB_SECRET_MASK "********"

/* 配置已改标志: 网页保存配置/待办后置 1, ui_task 检测后重绘主界面应用(实时生效+绘制统一避免并发).
 * httpd 任务写、ui_task 读/清: volatile 防跨核缓存(单字节写读本原子) */
static volatile uint8_t web_dirty = 0;
uint8_t WEB_ConfigDirty(void)      { return web_dirty; }
void    WEB_ConfigDirtyClear(void) { web_dirty = 0; }

/* ================= 主题色项(名称<->全局变量) ================= */
typedef struct { const char *name; uint16_t *color; } web_color_t;
static const web_color_t web_colors[] = {
    { "bg",    &UI_COLOR_BG },
    { "menu",  &UI_COLOR_MENU },
    { "frame", &UI_COLOR_FRAME },
    { "icon",  &UI_COLOR_ICON },
    { "time",  &UI_COLOR_TIME },
    { "date",  &UI_COLOR_DATE },
};
#define WEB_COLOR_N (sizeof(web_colors) / sizeof(web_colors[0]))

/* 从 NVS 读主题色(缺省保持默认) */
static void web_colors_load(void)
{
    nvs_handle_t h;
    uint8_t i;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK)
    {
        for (i = 0; i < WEB_COLOR_N; i++)
        {
            uint16_t v;
            if (nvs_get_u16(h, web_colors[i].name, &v) == ESP_OK)
            {
                *web_colors[i].color = v;
            }
        }
        nvs_close(h);
    }
}

static void web_colors_save(void)
{
    nvs_handle_t h;
    uint8_t i;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK)
    {
        for (i = 0; i < WEB_COLOR_N; i++)
        {
            nvs_set_u16(h, web_colors[i].name, *web_colors[i].color);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

/* RGB565 -> 6位RGB888十六进制(无#) */
static void rgb565_to_hex(uint16_t c, char *out)
{
    uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    snprintf(out, 8, "%02X%02X%02X",
             (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

/* 6位RGB888十六进制 -> RGB565 */
static uint16_t hex_to_rgb565(const char *hex)
{
    long v = strtol(hex, NULL, 16);
    uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* UTF-8 合法性 + 最大字节数校验(WEB 输入防截断乱码) */
static int web_utf8_valid(const char *s, size_t max_len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t left;
    if (!s) return 0;
    left = strlen(s);
    if (left > max_len) return 0;
    while (*p)
    {
        size_t need, i;
        if (*p < 0x80) { p++; left--; continue; }
        if ((*p & 0xE0) == 0xC0) { if (*p < 0xC2) return 0; need = 2; }
        else if ((*p & 0xF0) == 0xE0) need = 3;
        else if ((*p & 0xF8) == 0xF0) return 0;   /* 设备显示链按 ≤3 字节: 4 字节(emoji 等)一律拒绝 */
        else return 0;
        if (left < need) return 0;
        for (i = 1; i < need; i++)
        {
            if ((p[i] & 0xC0) != 0x80) return 0;
        }
        if (need == 3)
        {
            if (*p == 0xE0 && p[1] < 0xA0) return 0;
            if (*p == 0xED && p[1] > 0x9F) return 0;
        }
        p += need;
        left -= need;
    }
    return 1;
}

/* 6位十六进制颜色(不含 #) */
static int web_hex_color_valid(const char *s)
{
    size_t i;
    if (!s || strlen(s) != 6) return 0;
    for (i = 0; i < 6; i++)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

/* 指令库文本: 总长/NVS上限/UTF-8/每行长度/条数校验 */
static int web_ins_text_valid(const char *s)
{
    const char *p = s, *nl;
    uint8_t n = 0;
    if (!web_utf8_valid(s, 8192)) return 0;
    while (*p)
    {
        size_t len;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= INS_PRESET_LEN) return 0;
        if (len > 0)
        {
            n++;
            if (n > INS_PRESET_MAX) return 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 1;
}

/* ================= 配置页(内嵌 HTML, 面向小白: 状态栏/分组/快捷配色) ================= */
const char web_page[];   /* 页面 HTML 本体在 web_page.c(纯常量, 与逻辑分离) */

/* ================= API 处理 ================= */

static esp_err_t web_handler_root(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期: 网页活动=联网会话活跃, 防空闲自动断误判(路径1) */
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_page, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/cfg */
static esp_err_t web_api_cfg_get(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    cJSON *root = cJSON_CreateObject();
    cJSON *colors = cJSON_CreateObject();
    cJSON *ins = cJSON_CreateArray();
    cJSON *alarms = cJSON_CreateArray();
    uint8_t n, i;
    char hx[8];

    for (i = 0; i < WEB_COLOR_N; i++)
    {
        rgb565_to_hex(*web_colors[i].color, hx);
        cJSON_AddStringToObject(colors, web_colors[i].name, hx);
    }
    cJSON_AddItemToObject(root, "colors", colors);

    {
        const char *const *pres = INS_Presets(&n);
        for (i = 0; i < n; i++)
        {
            cJSON_AddItemToArray(ins, cJSON_CreateString(pres[i]));
        }
    }
    cJSON_AddItemToObject(root, "ins", ins);

    /* 答案之书自定义答案(每类一行一条, 内置答案固定不可改) */
    {
        cJSON *ans = cJSON_CreateObject();
        cJSON_AddStringToObject(ans, "c0", ANS_Custom(0));   /* 回答 */
        cJSON_AddStringToObject(ans, "c1", ANS_Custom(1));   /* 吃什么 */
        cJSON_AddStringToObject(ans, "c2", ANS_Custom(2));   /* 喝什么 */
        cJSON_AddStringToObject(ans, "c3", ANS_Custom(3));   /* 玩什么 */
        cJSON_AddItemToObject(root, "ans", ans);
    }

    {
        uint8_t max = ALM_Max();
        for (i = 0; i < max; i++)
        {
            uint8_t en, hh, mm, days, once;
            cJSON *a = cJSON_CreateObject();
            ALM_GetSlot(i, &en, &hh, &mm, &days, &once);
            cJSON_AddNumberToObject(a, "en", en);
            cJSON_AddNumberToObject(a, "hh", hh);
            cJSON_AddNumberToObject(a, "mm", mm);
            cJSON_AddNumberToObject(a, "days", days);
            cJSON_AddNumberToObject(a, "once", once);
            cJSON_AddItemToArray(alarms, a);
        }
    }
    cJSON_AddItemToObject(root, "alarms", alarms);

    /* 连接/系统 */
    {
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddStringToObject(wifi, "ssid", NET_GetSsid());
        /* 凭据脱敏: 密码/key 不回传明文; 已配置->掩码"********"(保存时保持), 未配置->空 */
        cJSON_AddStringToObject(wifi, "pass", (NET_GetPass()[0]) ? WEB_SECRET_MASK : "");
        cJSON_AddItemToObject(root, "wifi", wifi);
        cJSON_AddStringToObject(root, "city", NET_GetCity());
        cJSON_AddStringToObject(root, "key", (NET_GetKey()[0]) ? WEB_SECRET_MASK : "");
        cJSON_AddStringToObject(root, "user", INS_UserName());
        {
            /* 使用者列表(与设备端子菜单一致, 网页下拉选择) */
            uint8_t un = 0, k;
            const char *const *ul = UI_UserList(&un);
            cJSON *uarr = cJSON_AddArrayToObject(root, "users");
            for (k = 0; k < un; k++) cJSON_AddItemToArray(uarr, cJSON_CreateString(ul[k]));
        }
        cJSON_AddNumberToObject(root, "beep", SET_Beep());
        cJSON_AddNumberToObject(root, "key_sound", SET_KeySound());
        cJSON_AddNumberToObject(root, "vol", SET_Vol());
        cJSON_AddNumberToObject(root, "timeout", SET_TimeoutSec());
        cJSON_AddNumberToObject(root, "oracle_n", SET_OracleN());
        cJSON_AddNumberToObject(root, "oracle_win", SET_OracleWin());
        cJSON_AddNumberToObject(root, "cursor", UI_GetCursorStyle());
        cJSON *status = cJSON_CreateObject();
        cJSON_AddNumberToObject(status, "wifi", NET_WifiOk() ? 1 : 0);
        {
            char ip[16];
            NET_IpStrCopy(ip, sizeof(ip));   /* 拷贝版: 避免 /api/cfg 与 UI 刷新共用静态缓冲 */
            cJSON_AddStringToObject(status, "ip", ip);
        }
        cJSON_AddItemToObject(root, "status", status);
        cJSON_AddStringToObject(root, "ap_ssid", NET_GetApSsid());
        cJSON_AddStringToObject(root, "ap_pass", NET_GetApPass());
    }

    /* 破译参数 */
    {
        uint16_t def, gb, dl, rv;
        char h1[8], h2[8];
        cJSON *garble = cJSON_CreateObject();
        INS_GetParams(&def, &gb, &dl, &rv);
        rgb565_to_hex(def, h1);
        rgb565_to_hex(gb, h2);
        cJSON_AddStringToObject(garble, "def", h1);
        cJSON_AddStringToObject(garble, "gb", h2);
        cJSON_AddNumberToObject(garble, "dl", dl);
        cJSON_AddNumberToObject(garble, "rv", rv);
        cJSON_AddNumberToObject(garble, "fnt", INS_Font());   /* 破译字号 0..3 = 16/24/32/64px */
        cJSON_AddItemToObject(root, "garble", garble);
    }

    {
        char *s = cJSON_PrintUnformatted(root);
        if (s)
        {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, s);
            cJSON_free(s);
        }
        else
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/cfg */
/* ================= 配置应用(web_api_cfg_post 按项分发) ================= */
static int web_apply_colors(cJSON *root)
{
    cJSON *colors = cJSON_GetObjectItem(root, "colors");
    uint8_t i;
    if (!colors) return 1;
    if (!cJSON_IsObject(colors)) return 0;
    for (i = 0; i < WEB_COLOR_N; i++)
    {
        cJSON *it = cJSON_GetObjectItem(colors, web_colors[i].name);
        if (it && (!cJSON_IsString(it) || !web_hex_color_valid(it->valuestring))) return 0;
    }
    for (i = 0; i < WEB_COLOR_N; i++)
    {
        cJSON *it = cJSON_GetObjectItem(colors, web_colors[i].name);
        if (it) *web_colors[i].color = hex_to_rgb565(it->valuestring);
    }
    web_colors_save();
    return 1;
}

static int web_apply_ins(cJSON *root)
{
    cJSON *ins = cJSON_GetObjectItem(root, "ins");
    if (!ins) return 1;
    if (!cJSON_IsString(ins) || !web_ins_text_valid(ins->valuestring)) return 0;
    if (!INS_PresetsFromText(ins->valuestring)) return 0;   /* 落盘失败: 整体保存按失败处理 */
    return 1;
}

/* 答案之书整类答案(内置+自定义全量): {ans:{c0:"..",c1:"..",c2:"..",c3:".."}} 每行一条 */
static int web_ans_text_valid(const char *s)
{
    const char *p = s, *nl;
    uint8_t n = 0;
    if (!web_utf8_valid(s, ANS_TOTAL_MAX * ANS_LINE_MAX)) return 0;
    while (*p)
    {
        size_t len;
        nl = strchr(p, '\n');
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= ANS_LINE_MAX) return 0;   /* 单条答案过长 */
        if (len > 0)
        {
            n++;
            if (n > ANS_TOTAL_MAX) return 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 1;
}

static int web_apply_ans(cJSON *root)
{
    cJSON *ans = cJSON_GetObjectItem(root, "ans");
    uint8_t c;
    if (!ans) return 1;
    if (!cJSON_IsObject(ans)) return 0;
    for (c = 0; c < ANS_CAT_N; c++)
    {
        static const char *keys[ANS_CAT_N] = { "c0", "c1", "c2", "c3" };
        cJSON *it = cJSON_GetObjectItem(ans, keys[c]);
        if (!it) continue;
        if (!cJSON_IsString(it) || !web_ans_text_valid(it->valuestring)) return 0;
        ANS_FromText(c, it->valuestring);
    }
    return 1;
}

static int web_apply_alarms(cJSON *root)
{
    cJSON *alarms = cJSON_GetObjectItem(root, "alarms");
    int n;
    uint8_t max, i;
    if (!alarms) return 1;
    if (!cJSON_IsArray(alarms)) return 0;
    n = cJSON_GetArraySize(alarms);
    max = ALM_Max();
    if (n < 0 || n > max) return 0;
    for (i = 0; i < (uint8_t)n; i++)
    {
        cJSON *a = cJSON_GetArrayItem(alarms, i);
        if (!cJSON_IsObject(a)) return 0;
        cJSON *en = cJSON_GetObjectItem(a, "en");
        cJSON *hh = cJSON_GetObjectItem(a, "hh");
        cJSON *mm = cJSON_GetObjectItem(a, "mm");
        cJSON *days = cJSON_GetObjectItem(a, "days");
        cJSON *once = cJSON_GetObjectItem(a, "once");
        if (!cJSON_IsNumber(en) || !cJSON_IsNumber(hh) || !cJSON_IsNumber(mm)) return 0;
        if ((en->valueint != 0 && en->valueint != 1) ||
            hh->valueint < 0 || hh->valueint > 23 ||
            mm->valueint < 0 || mm->valueint > 59) return 0;
        if (days && (!cJSON_IsNumber(days) || days->valueint < 0 || days->valueint > 127)) return 0;
        if (once && (!cJSON_IsNumber(once) || (once->valueint != 0 && once->valueint != 1))) return 0;
    }
    for (i = 0; i < (uint8_t)n; i++)
    {
        cJSON *a = cJSON_GetArrayItem(alarms, i);
        cJSON *en = cJSON_GetObjectItem(a, "en");
        cJSON *hh = cJSON_GetObjectItem(a, "hh");
        cJSON *mm = cJSON_GetObjectItem(a, "mm");
        cJSON *days = cJSON_GetObjectItem(a, "days");
        cJSON *once = cJSON_GetObjectItem(a, "once");
        ALM_SetSlot(i, (uint8_t)(en->valueint ? 1 : 0),
                    (uint8_t)hh->valueint, (uint8_t)mm->valueint,
                    days ? (uint8_t)days->valueint : 0x7F,
                    once ? (uint8_t)(once->valueint ? 1 : 0) : 0);
    }
    for (i = (uint8_t)n; i < max; i++)   /* 网页只列已设闹钟: 未列出的槽全部关闭 */
    {
        ALM_SetSlot(i, 0, 0, 0, 0x7F, 0);
    }
    return 1;
}

static int web_apply_net(cJSON *root)
{
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    const char *ssid = NULL, *pass = NULL;
    if (wifi)
    {
        if (!cJSON_IsObject(wifi)) return 0;
        cJSON *s = cJSON_GetObjectItem(wifi, "ssid");
        cJSON *p = cJSON_GetObjectItem(wifi, "pass");
        if (!s || !p || !cJSON_IsString(s) || !cJSON_IsString(p)) return 0;
        if (s->valuestring[0] == '\0' || !web_utf8_valid(s->valuestring, 32)) return 0;
        if (!web_utf8_valid(p->valuestring, 64)) return 0;
        ssid = s->valuestring;
        pass = p->valuestring;
    }
    cJSON *city = cJSON_GetObjectItem(root, "city");
    if (city)
    {
        if (!cJSON_IsString(city) || !web_utf8_valid(city->valuestring, 23)) return 0;
        if (city->valuestring[0]) NET_SetCity(city->valuestring);
    }
    cJSON *key = cJSON_GetObjectItem(root, "key");
    if (key)
    {
        if (!cJSON_IsString(key) || !web_utf8_valid(key->valuestring, 47)) return 0;
        /* 掩码 = 保持原 key; 唯一例外: 原 key 恰好就是掩码串时按字面接受(消除歧义) */
        if (strcmp(key->valuestring, WEB_SECRET_MASK) != 0 || strcmp(NET_GetKey(), WEB_SECRET_MASK) == 0)
            NET_SetKey(key->valuestring);   /* 空=不配置天气 */
    }
    if (ssid)
    {
        /* 掩码密码 = 保持不变; 唯一例外: 已存密码恰好是掩码串时按字面接受(消除歧义) */
        if (strcmp(pass, WEB_SECRET_MASK) != 0 || strcmp(NET_GetPass(), WEB_SECRET_MASK) == 0)
            NET_SetWifi(ssid, pass);
        else NET_SetWifi(ssid, NET_GetPass());   /* 密码未动: 沿用已存密码(兼容只换 SSID) */
    }
    return 1;
}

static int web_apply_user(cJSON *root)
{
    cJSON *user = cJSON_GetObjectItem(root, "user");
    if (!user) return 1;
    if (!cJSON_IsString(user) || user->valuestring[0] == '\0') return 0;
    if (!web_utf8_valid(user->valuestring, INS_USER_NAME_MAX - 1) ||
        strchr(user->valuestring, '\n') || strchr(user->valuestring, '\r')) return 0;
    INS_SetUserName(user->valuestring);
    return 1;
}

static int web_apply_sound(cJSON *root)
{
    cJSON *beep = cJSON_GetObjectItem(root, "beep");
    if (beep)
    {
        if (!cJSON_IsNumber(beep) || (beep->valueint != 0 && beep->valueint != 1)) return 0;
        SET_SetBeep((uint8_t)(beep->valueint ? 1 : 0));
    }
    cJSON *vol = cJSON_GetObjectItem(root, "vol");
    if (vol)
    {
        if (!cJSON_IsNumber(vol) || vol->valueint < 0 || vol->valueint > 100) return 0;
        SET_SetVol((uint8_t)vol->valueint);
    }
    return 1;
}

static int web_apply_key_sound(cJSON *root)
{
    cJSON *ks = cJSON_GetObjectItem(root, "key_sound");
    if (!ks) return 1;
    if (!cJSON_IsNumber(ks) || ks->valueint < 0 || ks->valueint > 3) return 0;
    SET_SetKeySound((uint8_t)ks->valueint);
    return 1;
}

static int web_apply_timeout(cJSON *root)
{
    cJSON *timeout = cJSON_GetObjectItem(root, "timeout");
    if (timeout)
    {
        int v;
        if (!cJSON_IsNumber(timeout)) return 0;
        v = timeout->valueint;
        if (v != 0 && v != 30 && v != 60 && v != 300) return 0;
        SET_SetTimeout((uint16_t)v);
    }
    cJSON *on = cJSON_GetObjectItem(root, "oracle_n");
    if (on)
    {
        int v;
        if (!cJSON_IsNumber(on)) return 0;
        v = on->valueint;
        if (v != 0 && v != 1 && v != 3 && v != 5 && v != 9) return 0;
        SET_SetOracleN((uint8_t)v);
    }
    cJSON *ow = cJSON_GetObjectItem(root, "oracle_win");
    if (ow)
    {
        if (!cJSON_IsNumber(ow) || ow->valueint < 0 || ow->valueint > 3) return 0;
        SET_SetOracleWin((uint8_t)ow->valueint);
    }
    return 1;
}

static int web_apply_cursor(cJSON *root)
{
    cJSON *cur = cJSON_GetObjectItem(root, "cursor");
    if (!cur) return 1;
    if (!cJSON_IsNumber(cur) || cur->valueint < 0 || cur->valueint >= UI_CURSOR_N) return 0;
    SET_SetCursor((uint8_t)cur->valueint);
    return 1;
}

static int web_apply_decode(cJSON *root)
{
    cJSON *garble = cJSON_GetObjectItem(root, "garble");
    if (!garble) return 1;
    if (!cJSON_IsObject(garble)) return 0;
    {
        cJSON *def = cJSON_GetObjectItem(garble, "def");
        cJSON *gb  = cJSON_GetObjectItem(garble, "gb");
        cJSON *dl  = cJSON_GetObjectItem(garble, "dl");
        cJSON *rv  = cJSON_GetObjectItem(garble, "rv");
        cJSON *fnt = cJSON_GetObjectItem(garble, "fnt");
        if (def || gb)
        {
            if (!def || !gb || !cJSON_IsString(def) || !cJSON_IsString(gb) ||
                !web_hex_color_valid(def->valuestring) || !web_hex_color_valid(gb->valuestring)) return 0;
        }
        if (dl && (!cJSON_IsNumber(dl) || dl->valueint < 5)) return 0;
        if (rv && (!cJSON_IsNumber(rv) || rv->valueint < 10)) return 0;
        if (fnt && (!cJSON_IsNumber(fnt) || fnt->valueint < 0 || fnt->valueint > 3)) return 0;
        if (def && gb)
        {
            INS_SetParams((uint16_t)hex_to_rgb565(def->valuestring),
                          (uint16_t)hex_to_rgb565(gb->valuestring),
                          dl ? (uint16_t)dl->valueint : 18,
                          rv ? (uint16_t)rv->valueint : 80);
        }
        if (fnt) INS_SetFont((uint8_t)fnt->valueint);   /* 破译字号(INS_SetFont 内部钳位+存NVS) */
    }
    return 1;
}

/* ================= cfg 保存事务化(B2) =================
 * 先整体校验(零副作用), 全部合法后才逐个应用落库:
 * 避免"前面字段已写 NVS、后面字段非法返回 400"的半保存. */
static int web_cfg_validate(cJSON *root)
{
    cJSON *colors = cJSON_GetObjectItem(root, "colors");
    if (colors)
    {
        uint8_t i;
        if (!cJSON_IsObject(colors)) return 0;
        for (i = 0; i < WEB_COLOR_N; i++)
        {
            cJSON *it = cJSON_GetObjectItem(colors, web_colors[i].name);
            if (it && (!cJSON_IsString(it) || !web_hex_color_valid(it->valuestring))) return 0;
        }
    }
    cJSON *ins = cJSON_GetObjectItem(root, "ins");
    if (ins)
    {
        if (!cJSON_IsString(ins) || !web_ins_text_valid(ins->valuestring)) return 0;
    }
    cJSON *ans = cJSON_GetObjectItem(root, "ans");
    if (ans)
    {
        uint8_t c;
        static const char *keys[ANS_CAT_N] = { "c0", "c1", "c2", "c3" };
        if (!cJSON_IsObject(ans)) return 0;
        for (c = 0; c < ANS_CAT_N; c++)
        {
            cJSON *it = cJSON_GetObjectItem(ans, keys[c]);
            if (it && (!cJSON_IsString(it) || !web_ans_text_valid(it->valuestring))) return 0;
        }
    }
    cJSON *alarms = cJSON_GetObjectItem(root, "alarms");
    if (alarms)
    {
        int n, i;
        if (!cJSON_IsArray(alarms)) return 0;
        n = cJSON_GetArraySize(alarms);
        if (n < 0 || n > (int)ALM_Max()) return 0;
        for (i = 0; i < n; i++)
        {
            cJSON *a = cJSON_GetArrayItem(alarms, i);
            cJSON *en, *hh, *mm, *days, *once;
            if (!cJSON_IsObject(a)) return 0;
            en = cJSON_GetObjectItem(a, "en");
            hh = cJSON_GetObjectItem(a, "hh");
            mm = cJSON_GetObjectItem(a, "mm");
            days = cJSON_GetObjectItem(a, "days");
            once = cJSON_GetObjectItem(a, "once");
            if (!cJSON_IsNumber(en) || !cJSON_IsNumber(hh) || !cJSON_IsNumber(mm)) return 0;
            if ((en->valueint != 0 && en->valueint != 1) ||
                hh->valueint < 0 || hh->valueint > 23 ||
                mm->valueint < 0 || mm->valueint > 59) return 0;
            if (days && (!cJSON_IsNumber(days) || days->valueint < 0 || days->valueint > 127)) return 0;
            if (once && (!cJSON_IsNumber(once) || (once->valueint != 0 && once->valueint != 1))) return 0;
        }
    }
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (wifi)
    {
        cJSON *s, *p;
        if (!cJSON_IsObject(wifi)) return 0;
        s = cJSON_GetObjectItem(wifi, "ssid");
        p = cJSON_GetObjectItem(wifi, "pass");
        if (!s || !p || !cJSON_IsString(s) || !cJSON_IsString(p)) return 0;
        if (s->valuestring[0] == '\0' || !web_utf8_valid(s->valuestring, 32)) return 0;
        if (!web_utf8_valid(p->valuestring, 64)) return 0;
    }
    cJSON *city = cJSON_GetObjectItem(root, "city");
    if (city && (!cJSON_IsString(city) || !web_utf8_valid(city->valuestring, 23))) return 0;
    cJSON *key = cJSON_GetObjectItem(root, "key");
    if (key && (!cJSON_IsString(key) || !web_utf8_valid(key->valuestring, 47))) return 0;
    cJSON *user = cJSON_GetObjectItem(root, "user");
    if (user)
    {
        if (!cJSON_IsString(user) || user->valuestring[0] == '\0') return 0;
        if (!web_utf8_valid(user->valuestring, INS_USER_NAME_MAX - 1) ||
            strchr(user->valuestring, '\n') || strchr(user->valuestring, '\r')) return 0;
    }
    cJSON *beep = cJSON_GetObjectItem(root, "beep");
    if (beep && (!cJSON_IsNumber(beep) || (beep->valueint != 0 && beep->valueint != 1))) return 0;
    cJSON *vol = cJSON_GetObjectItem(root, "vol");
    if (vol && (!cJSON_IsNumber(vol) || vol->valueint < 0 || vol->valueint > 100)) return 0;
    cJSON *timeout = cJSON_GetObjectItem(root, "timeout");
    if (timeout)
    {
        int v;
        if (!cJSON_IsNumber(timeout)) return 0;
        v = timeout->valueint;
        if (v != 0 && v != 30 && v != 60 && v != 300) return 0;
    }
    cJSON *on = cJSON_GetObjectItem(root, "oracle_n");
    if (on)
    {
        int v;
        if (!cJSON_IsNumber(on)) return 0;
        v = on->valueint;
        if (v != 0 && v != 1 && v != 3 && v != 5 && v != 9) return 0;
    }
    cJSON *ow = cJSON_GetObjectItem(root, "oracle_win");
    if (ow && (!cJSON_IsNumber(ow) || ow->valueint < 0 || ow->valueint > 3)) return 0;
    cJSON *cur = cJSON_GetObjectItem(root, "cursor");
    if (cur && (!cJSON_IsNumber(cur) || cur->valueint < 0 || cur->valueint >= UI_CURSOR_N)) return 0;
    cJSON *garble = cJSON_GetObjectItem(root, "garble");
    if (garble)
    {
        cJSON *def = cJSON_GetObjectItem(garble, "def");
        cJSON *gb  = cJSON_GetObjectItem(garble, "gb");
        cJSON *dl  = cJSON_GetObjectItem(garble, "dl");
        cJSON *rv  = cJSON_GetObjectItem(garble, "rv");
        cJSON *fnt = cJSON_GetObjectItem(garble, "fnt");
        if (!cJSON_IsObject(garble)) return 0;
        if (def || gb)
        {
            if (!def || !gb || !cJSON_IsString(def) || !cJSON_IsString(gb) ||
                !web_hex_color_valid(def->valuestring) || !web_hex_color_valid(gb->valuestring)) return 0;
        }
        if (dl && (!cJSON_IsNumber(dl) || dl->valueint < 5)) return 0;
        if (rv && (!cJSON_IsNumber(rv) || rv->valueint < 10)) return 0;
        if (fnt && (!cJSON_IsNumber(fnt) || fnt->valueint < 0 || fnt->valueint > 3)) return 0;
    }
    cJSON *ks = cJSON_GetObjectItem(root, "key_sound");
    if (ks && (!cJSON_IsNumber(ks) || ks->valueint < 0 || ks->valueint > 3)) return 0;
    return 1;
}

/* 接收请求体(统一样板): 长度上限校验 + malloc + 循环 recv + NUL 结尾; 失败返回 NULL */
static char *web_recv_body(httpd_req_t *req, int max_len)
{
    int total = req->content_len, recvd = 0;
    char *buf;
    if (total <= 0 || total > max_len)
    {
        return NULL;
    }
    buf = malloc((size_t)total + 1);
    if (!buf)
    {
        return NULL;
    }
    while (recvd < total)
    {
        int r = httpd_req_recv(req, buf + recvd, (size_t)(total - recvd));
        if (r <= 0)
        {
            free(buf);
            return NULL;
        }
        recvd += r;
    }
    buf[recvd] = '\0';
    return buf;
}

static esp_err_t web_api_cfg_post(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    char *buf = web_recv_body(req, 32768);   /* 含指令库+答案文本框+闹钟, 上限 32KB */
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    /* B2: 先整体校验(零副作用), 通过后才逐个应用落库——非法输入不再"改一半还400" */
    if (!web_cfg_validate(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid field");
        return ESP_OK;
    }

    /* 按配置项逐个应用(已整体校验通过, apply 链不会半路失败) */
    if (!web_apply_colors(root) || !web_apply_ins(root) || !web_apply_ans(root) ||
        !web_apply_alarms(root) ||
        !web_apply_net(root) || !web_apply_user(root) || !web_apply_sound(root) ||
        !web_apply_timeout(root) || !web_apply_cursor(root) || !web_apply_key_sound(root) || !web_apply_decode(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid field");
        return ESP_OK;
    }

    cJSON_Delete(root);
    web_dirty = 1;   /* 通知 ui_task 重绘主界面(主题色/使用者实时生效) */
    ESP_LOGI(TAG, "config applied");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* 扫描附近 WiFi(供配置页点击选择, 免手输 SSID) */
static esp_err_t web_api_scan(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    enum { SCAN_MAX = 20 };
    char ssids[SCAN_MAX][33];
    int8_t rssi[SCAN_MAX];
    uint8_t enc[SCAN_MAX];
    uint8_t n = NET_ScanWifi(SCAN_MAX, ssids, rssi, enc), i;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "wifi_list");
    for (i = 0; i < n; i++)
    {
        cJSON *w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "ssid", ssids[i]);
        cJSON_AddNumberToObject(w, "rssi", rssi[i]);
        cJSON_AddNumberToObject(w, "encrypted", enc[i]);
        cJSON_AddItemToArray(arr, w);
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 抽卡图鉴: 按罪人分组返回全部人格与已抽标记(★=已抽), 供网页图鉴展示 */
static esp_err_t web_api_gacha(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    uint8_t s, n = GACHA_CoinSinnerN();
    uint16_t total = GACHA_CoinTotal();
    cJSON *root = cJSON_CreateObject();
    cJSON *sinners = cJSON_AddArrayToObject(root, "sinners");
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "owned", GACHA_CoinOwnedCount());
    for (s = 0; s < n; s++)
    {
        uint16_t off = GACHA_CoinSinnerOff(s);
        uint16_t cnt = GACHA_CoinSinnerCount(s);
        uint16_t i, ow = 0;
        cJSON *so = cJSON_CreateObject();
        cJSON *items = cJSON_AddArrayToObject(so, "items");
        cJSON_AddStringToObject(so, "name", GACHA_CoinSinnerName(s));
        cJSON_AddNumberToObject(so, "total", cnt);
        for (i = 0; i < cnt; i++)
        {
            cJSON *it = cJSON_CreateObject();
            uint8_t owned = GACHA_CoinOwned(off + i);
            if (owned) ow++;
            cJSON_AddStringToObject(it, "name", GACHA_CoinName(off + i));
            cJSON_AddNumberToObject(it, "owned", owned);
            cJSON_AddItemToArray(items, it);
        }
        cJSON_AddNumberToObject(so, "owned", ow);
        cJSON_AddItemToArray(sinners, so);
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 设备状态(网页状态页): 电量/堆/开机次数/IP/WiFi/时间/天气 */
static esp_err_t web_api_status(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    uint8_t pct = BAT_GetPct();
    char ip[16], date[8], time[12], wx[32];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "battery", (pct <= 100) ? pct : -1);
    cJSON_AddNumberToObject(root, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "boot", SET_BootCount());
    NET_IpStrCopy(ip, sizeof(ip));
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ssid", NET_GetSsid());
    cJSON_AddNumberToObject(root, "wifi", NET_WifiOk() ? 1 : 0);
    NET_DateStrCopy(date, sizeof(date));
    NET_TimeStrCopy(time, sizeof(time));
    cJSON_AddStringToObject(root, "date", date);
    cJSON_AddStringToObject(root, "time", time);
    cJSON_AddStringToObject(root, "weather", NET_WeatherStrCopy(wx, sizeof(wx)) ? wx : "暂无");
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 待办列表: 返回 {todos:[{text,done},...]} */
static esp_err_t web_api_todo_get(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    uint8_t n = TODO_Count(), i;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "todos");
    for (i = 0; i < n; i++)
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "text", TODO_Text(i));
        cJSON_AddNumberToObject(t, "done", TODO_Done(i));
        cJSON_AddItemToArray(arr, t);
    }
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

/* 待办操作: {op:"add",text} | {op:"toggle",idx} | {op:"del",idx} | {op:"clear"} */
static esp_err_t web_api_todo_post(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    char *buf = web_recv_body(req, 2048);
    cJSON *root, *op, *text, *idx;
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
        return ESP_OK;
    }
    root = cJSON_Parse(buf);
    free(buf);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    op = cJSON_GetObjectItem(root, "op");
    if (!op || !cJSON_IsString(op))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad op");
        return ESP_OK;
    }
    if (strcmp(op->valuestring, "add") == 0)
    {
        text = cJSON_GetObjectItem(root, "text");
        if (!text || !cJSON_IsString(text) || text->valuestring[0] == '\0' ||
            !web_utf8_valid(text->valuestring, TODO_TEXT_MAX - 1) ||
            strchr(text->valuestring, '\n') || strchr(text->valuestring, '\r'))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad text");
            return ESP_OK;
        }
        TODO_Add(text->valuestring);
    }
    else if (strcmp(op->valuestring, "toggle") == 0)
    {
        idx = cJSON_GetObjectItem(root, "idx");
        if (!idx || !cJSON_IsNumber(idx) || idx->valueint < 0 || idx->valueint >= TODO_Count())
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
            return ESP_OK;
        }
        TODO_Toggle((uint8_t)idx->valueint, 0);   /* 网页触发: 不绘屏(绘制归 ui_task, web_dirty 刷新) */
    }
    else if (strcmp(op->valuestring, "del") == 0)
    {
        idx = cJSON_GetObjectItem(root, "idx");
        if (!idx || !cJSON_IsNumber(idx) || idx->valueint < 0 || idx->valueint >= TODO_Count())
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
            return ESP_OK;
        }
        TODO_Del((uint8_t)idx->valueint);
    }
    else if (strcmp(op->valuestring, "clear") == 0)
    {
        TODO_Clear();
    }
    else
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad op");
        return ESP_OK;
    }
    web_dirty = 1;   /* 待办改了: 若设备在待办界面, ui_task 刷新列表 */
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* ================= 下发指令(网页 -> 设备破译显示) ================= */
static char web_pending_cmd[96];   /* 下发指令上限 96B: 加"致{使用者}:"(≤24B)后须落在 INS_ShowIns 合成缓冲(138B)内 */
static volatile uint8_t web_pending_flag = 0;
/* 缓冲由 httpd 任务写(web_api_send)、ui_task 读(WEB_TakeCmd): 自旋锁串行化(S5) */
static portMUX_TYPE web_mux = portMUX_INITIALIZER_UNLOCKED;

/* POST /api/send: 接收 {cmd:"..."} 缓存, 由 ui_task 取出用乱码破译显示 */
static esp_err_t web_api_send(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    char *buf = web_recv_body(req, 2048);
    cJSON *root, *cmd;
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
        return ESP_OK;
    }
    root = cJSON_Parse(buf);
    free(buf);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd || !cJSON_IsString(cmd) || cmd->valuestring[0] == '\0' ||
        !web_utf8_valid(cmd->valuestring, sizeof(web_pending_cmd) - 1))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad cmd");
        return ESP_OK;
    }
    portENTER_CRITICAL(&web_mux);
    strncpy(web_pending_cmd, cmd->valuestring, sizeof(web_pending_cmd) - 1);
    web_pending_cmd[sizeof(web_pending_cmd) - 1] = '\0';
    web_pending_flag = 1;
    portEXIT_CRITICAL(&web_mux);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* ui_task 取出待显示指令(取走后清空), 返回 1=有待显示 */
uint8_t WEB_TakeCmd(char *buf, size_t n)
{
    uint8_t r = 0;
    portENTER_CRITICAL(&web_mux);
    if (web_pending_flag)
    {
        if (n > 0)
        {
            strncpy(buf, web_pending_cmd, n - 1);
            buf[n - 1] = '\0';
        }
        web_pending_flag = 0;
        r = 1;
    }
    portEXIT_CRITICAL(&web_mux);
    return r;
}

/* 试响: 蜂鸣器响一下(验证蜂鸣/音量) */
static esp_err_t web_api_beep(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    BUZZER_Beep(1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* 重启设备 */
static esp_err_t web_api_reboot(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    vTaskDelay(200 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/* 添加使用者(网页端「添加」按钮): 收 {name}, 加入列表并持久化(NVS, 同指令库模式) */
static esp_err_t web_api_user_add(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    char *buf = web_recv_body(req, 512);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    uint8_t ok = 0;
    if (root)
    {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        if (name && cJSON_IsString(name) && name->valuestring[0] &&
            web_utf8_valid(name->valuestring, UI_USER_NAME_MAX - 1) &&
            !strchr(name->valuestring, '\n') && !strchr(name->valuestring, '\r'))
        {
            ok = UI_UserAdd(name->valuestring);
        }
        cJSON_Delete(root);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, ok ? "{\"ok\":1}" : "{\"ok\":0}");
    return ESP_OK;
}

/* 清除已存 WiFi: 回纯 AP 配网模式(配置页"清除"按钮) */
static esp_err_t web_api_clearwifi(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    NET_ClearWifi();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* 手机连上热点的 captive portal 探测路径 -> 直接返回配置页(自动弹出, 免手输IP) */
static esp_err_t web_captive(httpd_req_t *req)
{
    NET_Touch();   /* 会话续期(路径1): 防空闲自动断误判 */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, web_page);
    return ESP_OK;
}

/* ================= 入口 ================= */
void WEB_Init(void)
{
    web_colors_load();   /* 加载持久化主题色 */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;          /* 大栈: 解析 + 大JSON响应 */
    cfg.max_uri_handlers = 24;       /* 主13 + captive portal 探测 7 + 余量 */
    cfg.max_open_sockets = 4;        /* 单手机够用, 少占堆 */
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "httpd start, free heap=%u", (unsigned)esp_get_free_heap_size());
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd start FAILED");
        return;
    }
    ESP_LOGI(TAG, "httpd started OK");
    httpd_uri_t r1 = { .uri = "/",           .method = HTTP_GET,  .handler = web_handler_root,   .user_ctx = NULL };
    httpd_uri_t r2 = { .uri = "/api/cfg",    .method = HTTP_GET,  .handler = web_api_cfg_get,    .user_ctx = NULL };
    httpd_uri_t r3 = { .uri = "/api/cfg",    .method = HTTP_POST, .handler = web_api_cfg_post,   .user_ctx = NULL };
    httpd_uri_t r4 = { .uri = "/api/beep",   .method = HTTP_POST, .handler = web_api_beep,       .user_ctx = NULL };
    httpd_uri_t r5 = { .uri = "/api/reboot", .method = HTTP_POST, .handler = web_api_reboot,     .user_ctx = NULL };
    httpd_uri_t r6 = { .uri = "/api/scan",   .method = HTTP_POST, .handler = web_api_scan,       .user_ctx = NULL };
    httpd_uri_t r7 = { .uri = "/api/send",   .method = HTTP_POST, .handler = web_api_send,       .user_ctx = NULL };
    httpd_uri_t r8 = { .uri = "/api/gacha",  .method = HTTP_GET,  .handler = web_api_gacha,       .user_ctx = NULL };
    httpd_uri_t r9 = { .uri = "/api/todo",   .method = HTTP_GET,  .handler = web_api_todo_get,    .user_ctx = NULL };
    httpd_uri_t r10 = { .uri = "/api/todo",  .method = HTTP_POST, .handler = web_api_todo_post,   .user_ctx = NULL };
    httpd_uri_t r11 = { .uri = "/api/status", .method = HTTP_GET, .handler = web_api_status,      .user_ctx = NULL };
    httpd_uri_t r12 = { .uri = "/api/user",   .method = HTTP_POST, .handler = web_api_user_add,   .user_ctx = NULL };
    httpd_uri_t r13 = { .uri = "/api/clearwifi", .method = HTTP_POST, .handler = web_api_clearwifi, .user_ctx = NULL };
    httpd_register_uri_handler(server, &r1);
    httpd_register_uri_handler(server, &r2);
    httpd_register_uri_handler(server, &r3);
    httpd_register_uri_handler(server, &r4);
    httpd_register_uri_handler(server, &r5);
    httpd_register_uri_handler(server, &r6);
    httpd_register_uri_handler(server, &r7);
    httpd_register_uri_handler(server, &r8);
    httpd_register_uri_handler(server, &r9);
    httpd_register_uri_handler(server, &r10);
    httpd_register_uri_handler(server, &r11);
    httpd_register_uri_handler(server, &r12);
    httpd_register_uri_handler(server, &r13);

    /* captive portal 探测路径(安卓/iOS/Windows): 全返回配置页, 手机连热点自动弹出 */
    {
        static const char *probes[] = {
            "/generate_204", "/connectivity-check.gstatic.com",
            "/library/test/success.html", "/hotspot-detect.html",
            "/ncsi.txt", "/connecttest.txt", "/gen_204"
        };
        uint8_t k;
        for (k = 0; k < sizeof(probes) / sizeof(probes[0]); k++)
        {
            httpd_uri_t p = { .uri = probes[k], .method = HTTP_GET, .handler = web_captive, .user_ctx = NULL };
            httpd_register_uri_handler(server, &p);
        }
    }
    ESP_LOGI(TAG, "config page: WiFi已保存=%s -> http://192.168.4.1/ 或 http://<router-ip>/",
             NET_GetSsid()[0] ? "是(STA直连)" : "否(连热点配网)");
}
