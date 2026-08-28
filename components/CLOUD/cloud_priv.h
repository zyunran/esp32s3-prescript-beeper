#ifndef __CLOUD_PRIV_H
#define __CLOUD_PRIV_H

#include <stdint.h>
#include <stddef.h>

/* CLOUD 组件内部接口(cloud.c 任务主体 / cloud_onenet.c 平台适配 / cloud_token.c 鉴权) */

#define CLOUD_TOKEN_MAX    512     /* 鉴权 password 上限(res/sign URL 编码后) */
#define CLOUD_TOPIC_MAX    128     /* $sys/{pid}/{name}/thing/... 上限 */
#define CLOUD_PAYLOAD_MAX  512     /* 属性/事件/回执组包上限 */
#define CLOUD_MSG_MAX      64      /* 事件 msg 文本上限 */
#define CLOUD_ID_MAX       16      /* 平台回执 id 串上限(仅 [A-Za-z0-9_-]) */
#define CLOUD_REPORT_MS    60000UL /* 属性周期上报间隔 */
#define CLOUD_KEEPALIVE_S  60      /* MQTT keepalive 秒 */

/* cloud_token.c: 设备级安全鉴权 token -> out(password 字段), 0=成功 负数=失败 */
int cloud_token_gen(const char *pid, const char *name, const char *key_b64,
                    char *out, size_t outn);

/* cloud_onenet.c: OneNET 平台适配(topic 拼接 / OneJSON 组包 / 下行解析), 0=成功 负数=失败 */
int cloud_topic(char *out, size_t n, const char *pid, const char *name, const char *suffix);
int cloud_prop_post_json(char *out, size_t n, int battery, int rssi,
                         const char *ver, uint32_t alarm_cnt, uint32_t seq);
int cloud_event_post_json(char *out, size_t n, const char *evt_id,
                          const char *msg, uint32_t seq);
int cloud_parse_cmd(const char *json, char *msg, size_t msgn, char *id, size_t idn); /* 1=成功 */
int cloud_cmd_reply_json(char *out, size_t n, const char *id, int code);

#endif
