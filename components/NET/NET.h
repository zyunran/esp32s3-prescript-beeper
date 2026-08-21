#ifndef __NET_H
#define __NET_H

#include <stdint.h>

/* NET 组件: WiFi STA 联网 + SNTP 校时("联网"子菜单手动触发)
 *  - NET_Init:    初始化 WiFi/SNTP 基础(事件驱动, 不阻塞, 不联网)
 *  - NET_Connect: 手动开始联网+校时(幂等; 断连自动重连)
 *  - 时间未同步时 DateStr/TimeStr 返回 "--" 占位(主页面左侧显示)
 * 注意: 调用本组件前须先 nvs_flash_init()(在 main 中完成). */
void NET_Init(void);
void NET_Connect(void);
void NET_WifiStop(void);             /* 待机前: 停 WiFi(省电) */
void NET_WifiStart(void);            /* 唤醒后: 启 WiFi 自动重连 + 立即校时 */
uint8_t NET_ApToggle(void);            /* 开启配网: 手动开/关配网热点(STA<->APSTA), 返回1=已开 */
uint8_t NET_WifiOk(void);            /* 1=WiFi 已连上 */
uint8_t NET_TimeOk(void);            /* 1=时间已同步 */
void NET_TimeAdopt(void);            /* 采用外部有效时间(DS1302), 免联网即时显示 */
const char *NET_IpStr(void);         /* 已连 WiFi 的 IP("192.168.1.5"; 未连="" ) */
const char *NET_DateStr(void);       /* "MM-DD"(未同步 "--") */
const char *NET_TimeStr(void);       /* "HH:MM:SS"(未同步 "--:--:--") */
const char *NET_WeekStr(void);       /* "Mon".."Sun"(未同步 "--") */
const char *NET_WeatherStr(void);    /* 今日天气 "晴 36/24 75%"(按时段自动白天/晚上; 未就绪 NULL) */
uint8_t     NET_WeatherCount(void);  /* 已拉取天气天数(0=未就绪) */
const char *NET_WeatherDayStr(uint8_t idx); /* 第 idx 天 "MM-DD 晴 36/24 75%"(越界/未就绪 NULL) */
const char *NET_WeatherMadStr(void);    /* 彩蛋(纺织时间): 随机日天气词+真实区间随机温度+随机湿度 */
uint32_t    NET_WeatherAge(void);       /* 天气数据年龄(秒, 距上次拉取成功; 0=无数据) */
const char *NET_WeatherUpdatedStr(void);/* "更新于 MM-DD HH:MM"(无数据/未校时 NULL) */

/* 运行期配置(WEB 配置页用; WiFi/城市存 NVS "net") */
const char *NET_GetSsid(void);             /* 当前 WiFi 名 */
const char *NET_GetPass(void);             /* 当前 WiFi 密码 */
const char *NET_GetCity(void);             /* 天气城市(拼音) */
const char *NET_GetKey(void);              /* 心知天气 API 私钥 */
const char *NET_GetApSsid(void);           /* 配网热点名 */
const char *NET_GetApPass(void);           /* 配网热点密码 */
void NET_SetWifi(const char *ssid, const char *pass);   /* 保存WiFi并切到AP+STA连网 */
void NET_ClearWifi(void);                               /* 清除已存 WiFi, 回纯 AP 配网模式(WEB"清除"按钮) */
void NET_SetCity(const char *city);                     /* 立即置为待重拉 */
void NET_SetKey(const char *key);                       /* 改天气私钥, 立即置为待重拉 */
uint8_t NET_ScanWifi(uint8_t max, char ssids[][33], int8_t rssi[], uint8_t enc[]); /* 扫描附近WiFi, 返回条数 */

#endif
