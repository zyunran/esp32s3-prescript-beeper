#ifndef __CLOUD_H
#define __CLOUD_H

#include <stdint.h>
#include <stddef.h>

/* CLOUD 组件: OneNET Studio(物联网平台) MQTT 接入(v1.14)
 * 三链路:
 *   上行属性: battery/rssi/version/alarm_cnt 连上即报 + 60s 周期上报
 *   上行事件: 闹钟到点/待办提醒/每日神谕 -> OneNET 事件记录
 *   下行指令: 平台服务 display_cmd -> 屏幕乱码破译显示(与网页下发指令同路径)
 * 配置: NVS "cloud"(on/pid/name/key), 网页「云端」卡片(/api/cloud)配置; 「远程在线」默认关,
 *   开启后 CLOUD_KeepAlive() 阻止浅睡眠断网(云端持续在线, 待机耗电相应增加)
 * 接入: mqtt://mqtts.heclouds.com:1883, clientId=设备名, username=产品ID,
 *   password=安全鉴权 token(cloud_token.c, HMAC-SHA256; 与 tools/onenet_token.py 同规则);
 *   topic 拼接/OneJSON 组包/下行解析收拢在 cloud_onenet.c 平台适配层, 换平台只改该层 */

#define CLOUD_PID_MAX   40   /* 产品ID(OneNET 实际 8~10 位, 留余量) */
#define CLOUD_NAME_MAX  48   /* 设备名 */
#define CLOUD_KEY_MAX   80   /* 设备密钥 base64 */
#define CLOUD_CMD_MAX   96   /* 云端下发指令上限: 与网页指令一致(加"致X:"后须落 INS_ShowIns 合成缓冲 138B 内) */

typedef struct {
    uint8_t on;                    /* 「远程在线」开关 */
    char pid[CLOUD_PID_MAX];       /* 产品ID */
    char name[CLOUD_NAME_MAX];     /* 设备名 */
    char key[CLOUD_KEY_MAX];       /* 设备密钥(网页不回显明文, 掩码=保持) */
} cloud_cfg_t;

typedef enum {
    CLOUD_EVT_ALARM = 0,   /* 闹钟到点 */
    CLOUD_EVT_TODO,        /* 待办提醒到点(msg=待办文本, 可 NULL) */
    CLOUD_EVT_DAILY,       /* 每日神谕已推送 */
} cloud_evt_t;

void CLOUD_Init(void);                       /* 加载 NVS 配置 + 创建云端任务(未启用不连) */
void CLOUD_GetConfig(cloud_cfg_t *out);      /* 锁内快照(WEB 读取用) */
void CLOUD_SetConfig(const cloud_cfg_t *in); /* 保存 NVS + 通知任务按新配置重建会话(WEB 保存用) */
uint8_t CLOUD_IsOnline(void);                /* 1=MQTT 已连接 OneNET */
uint8_t CLOUD_KeepAlive(void);               /* 1=云端会话保持中(ui_task 待机门控: 开启且已拉起客户端) */
uint8_t CLOUD_GetOn(void);                   /* 「远程在线」开关当前值(联网子菜单标签用) */
void CLOUD_NotifyEvent(cloud_evt_t e, const char *msg); /* 事件上报入口(任意任务; 非阻塞, 未启用/队满即丢) */
uint8_t CLOUD_TakeCmd(char *buf, size_t n);  /* 取云端下发显示指令(取走即清), 1=有 */

#endif
