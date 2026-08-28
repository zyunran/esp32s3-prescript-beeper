#ifndef __ALARM_H
#define __ALARM_H

#include <stdint.h>

/* ALARM 组件: 闹钟(TTL协议"过去"), 最多 16 段, 每天重复 / 按星期 / 一次性(时:分)
 * 流程: 二级菜单(添加闹钟/当前闹钟/退出) -> 添加: 设定屏(时分+重复三栏竖向滑动+光标),
 *   确认/重试/退出 为设定屏内嵌菜单; 当前闹钟: 列表(OK开关/长按OK删除)
 * 触发: UI 主任务每秒查 ALM_Check(待机浅睡眠 tick 中也会查), 到点且开启且当日未触发则返回 1(内部标记当日已触发),
 *   一次性模式到时自动关闭; 调用方在主界面空闲时 ALM_Show 显示闹钟专属指令(乱码破译)
 * 数据: NVS 持久化(namespace "alarm"); 由 UI 主任务驱动 Enter/Key/Tick */

void ALM_Init(void);                                       /* 加载 NVS 闹钟 */
void ALM_Enter(void);                                      /* 进入闹钟二级菜单 */
void ALM_Key(uint8_t up, uint8_t ok, uint8_t down, uint8_t lng); /* 按键(1=按下; lng=OK长按) */
void ALM_Tick(void);                                       /* 每主循环推进(设定屏滑动动画) */
uint8_t ALM_Busy(void);                                    /* 1=闹钟界面运行中 */
uint8_t ALM_Check(void);                                   /* 1=本时刻有闹钟到点(已标记当日触发) */
void ALM_Show(void);                                       /* 显示一条闹钟专属指令(INS_Show 乱码) */
void ALM_WebChanged(void);                                 /* 网页改了闹钟: 若处于"当前闹钟"列表则就地重建(立即反映) */
/* 槽位读写(WEB 配置用; days: bit0=周日..bit6=周六 位掩码, 0x7F=每天; once: 1=一次性) */
uint8_t ALM_Max(void);                                     /* 最大槽数 */
void ALM_GetSlot(uint8_t i, uint8_t *en, uint8_t *hh, uint8_t *mm, uint8_t *days, uint8_t *once);
void ALM_SetSlot(uint8_t i, uint8_t en, uint8_t hh, uint8_t mm, uint8_t days, uint8_t once); /* 写槽并持久化 */
void ALM_ClearSlot(uint8_t i);                                /* 清空槽位(删除闹钟)并持久化 */
void ALM_SaveBatchBegin(void);                                /* 网页批量保存开始: 写槽只改 RAM 不落盘 */
void ALM_SaveBatchEnd(void);                                  /* 批量结束: 闹钟表统一落盘一次(配对调用) */

#endif
