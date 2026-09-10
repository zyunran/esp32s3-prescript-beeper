#ifndef __ALARM_H
#define __ALARM_H

#include <stdint.h>

/* ALARM: 闹钟 API。NVS "alarm";主任务/待机 tick 驱动。 */

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
