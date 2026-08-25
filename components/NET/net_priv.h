#ifndef __NET_PRIV_H
#define __NET_PRIV_H

/* NET 组件内部私有头: NET.c(连接/事件/配置/API) 与 net_weather.c(拉取/解析) 共享的状态.
 * 仅组件内使用, 勿被外部包含. */

#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define NET_WEATHER_DAYS       3                    /* 拉取天数 */
#define NET_DAY_START_HOUR     6                    /* 白天/晚上天气切换: 6:00~17:59 显示白天 */
#define NET_DAY_END_HOUR       18

typedef struct {
    char date[8];       /* "MM-DD" */
    char text_day[22];  /* 天气现象: 心知常见"雷阵雨伴有冰雹"7字=21B+NUL; 12B 会把"小雨转多云"截成"小雨转多" */
    char text_night[22];/* 晚间天气现象(同容量, 缺失时回退白天) */
    char high[4];       /* "36" */
    char low[4];        /* "24" */
    char humidity[4];   /* "75" */
} net_weather_t;

/* 定义体在 NET.c(配置归属连接主文件) */
extern char           net_city[24];
extern char           net_key[48];
extern portMUX_TYPE   net_mux;

/* 定义体在 net_weather.c */
extern net_weather_t      net_w[NET_WEATHER_DAYS];
extern uint8_t            net_weather_ok;        /* 天气已拉取成功 */
extern uint8_t            net_weather_n;         /* 实际解析天数(API 可能只回 1~2 天) */
extern volatile uint8_t   net_weather_fetched;   /* 本次会话已拉取(断连后重置再拉); volatile: 跨核读写 */
extern volatile uint8_t   net_weather_busy;      /* 天气任务存活(防断连重连堆积) */
extern time_t             net_weather_at;        /* 上次拉取成功时刻(epoch) */
extern char               net_weather_raw[4096]; /* HTTP 响应缓冲(放大防截断) */
extern uint16_t           net_weather_raw_len;

void net_weather_task(void *arg);   /* GOT_IP 后由 NET.c 事件处理创建 */

#endif
