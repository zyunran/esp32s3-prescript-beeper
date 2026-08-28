/* OneNET Studio 平台适配层: topic 拼接($sys/{pid}/{name}/thing/...) + OneJSON 组包
 *   + display_cmd 服务调用下行解析。换平台只改本文件(鉴权在 cloud_token.c)。
 * 物模型对应(与 README「云端 OneNET」配置说明一致):
 *   属性: battery / rssi / version / alarm_cnt          -> thing/property/post
 *   事件: alarm_fire / todo_remind / daily_sign, 参数 msg -> thing/event/{id}/post
 *   服务: display_cmd, 入参 msg / timeout               -> thing/service/display_cmd/invoke */
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "cloud_priv.h"

/* topic = $sys/{pid}/{name}/thing/{suffix}, 0=成功 */
int cloud_topic(char *out, size_t n, const char *pid, const char *name, const char *suffix)
{
    int m = snprintf(out, n, "$sys/%s/%s/thing/%s", pid, name, suffix);
    return (m < 0 || (size_t)m >= n) ? -1 : 0;
}

/* msg 文本 JSON 转义(" \ 与 <0x20 控制符), 返回写入长度, -1=缓冲不足 */
static int json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in; in++)
    {
        char uesc[8];
        const char *rep = NULL;
        unsigned char c = (unsigned char)*in;
        switch (c)
        {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if (c < 0x20)
                {
                    snprintf(uesc, sizeof(uesc), "\\u%04x", c);
                    rep = uesc;
                }
                break;
        }
        size_t l = rep ? strlen(rep) : 1;
        if (o + l + 1 > n) return -1;
        if (rep) memcpy(out + o, rep, l);
        else     out[o] = (char)c;
        o += l;
    }
    out[o] = '\0';
    return (int)o;
}

/* 属性上报: OneJSON params 内仅带本次有效属性(无电池传 battery<0 跳过该项) */
int cloud_prop_post_json(char *out, size_t n, int battery, int rssi,
                         const char *ver, uint32_t alarm_cnt, uint32_t seq)
{
    char params[320];
    int m;
    if (battery >= 0 && battery <= 100)
        m = snprintf(params, sizeof(params),
                     "\"battery\":{\"value\":%d},\"rssi\":{\"value\":%d},"
                     "\"version\":{\"value\":\"%s\"},\"alarm_cnt\":{\"value\":%lu}",
                     battery, rssi, ver, (unsigned long)alarm_cnt);
    else
        m = snprintf(params, sizeof(params),
                     "\"rssi\":{\"value\":%d},\"version\":{\"value\":\"%s\"},"
                     "\"alarm_cnt\":{\"value\":%lu}",
                     rssi, ver, (unsigned long)alarm_cnt);
    if (m < 0 || m >= (int)sizeof(params)) return -1;
    m = snprintf(out, n, "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{%s}}",
                 (unsigned long)seq, params);
    return (m < 0 || (size_t)m >= n) ? -1 : 0;
}

/* 事件上报: msg 先转义再嵌入 */
int cloud_event_post_json(char *out, size_t n, const char *evt_id,
                          const char *msg, uint32_t seq)
{
    char esc[CLOUD_MSG_MAX * 6 + 4];   /* 每字节最坏 \u00XX(6字符) */
    if (json_escape(msg ? msg : "", esc, sizeof(esc)) < 0) return -1;
    int m = snprintf(out, n,
                     "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{\"msg\":{\"value\":\"%s\"}}}",
                     (unsigned long)seq, esc);
    return (m < 0 || (size_t)m >= n) ? -1 : 0;
}

/* 回执 id 白名单: 仅 [A-Za-z0-9_-] 且不超长(回执原样拼回 JSON, 防注入) */
static int id_ok(const char *s)
{
    size_t i;
    if (!s || !s[0] || strlen(s) >= CLOUD_ID_MAX) return 0;
    for (i = 0; s[i]; i++)
    {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

/* 解析 display_cmd 服务调用 {id, params:{msg[,timeout]}}: 提取 msg 与回执 id, 1=成功
 * (msg 超长整体拒收而非截断: 防截断切断 UTF-8 出乱码; id 白名单校验) */
int cloud_parse_cmd(const char *json, char *msg, size_t msgn, char *id, size_t idn)
{
    cJSON *root = cJSON_Parse(json);
    int ok = 0;
    if (!root) return 0;
    cJSON *jid = cJSON_GetObjectItem(root, "id");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *m = params ? cJSON_GetObjectItem(params, "msg") : NULL;
    if (cJSON_IsString(jid) && jid->valuestring && id_ok(jid->valuestring) &&
        cJSON_IsString(m) && m->valuestring && m->valuestring[0] &&
        strlen(m->valuestring) < msgn)
    {
        strcpy(msg, m->valuestring);
        strcpy(id, jid->valuestring);
        ok = 1;
    }
    cJSON_Delete(root);
    return ok;
}

/* 服务调用回执: {"id":..,"code":200,"params":{}} */
int cloud_cmd_reply_json(char *out, size_t n, const char *id, int code)
{
    int m = snprintf(out, n, "{\"id\":\"%s\",\"code\":%d,\"params\":{}}", id, code);
    return (m < 0 || (size_t)m >= n) ? -1 : 0;
}
