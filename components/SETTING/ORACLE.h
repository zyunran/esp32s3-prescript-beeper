#ifndef __ORACLE_H
#define __ORACLE_H

#include <stdint.h>

/* ORACLE 组件: 随机神谕推送
 *  - 每天在校时段(SETTING 配置次数/时段)内随机 N 个时刻
 *  - UI 主任务每秒查 ORACLE_Due, 到时自行决定是否推送(调用方在空闲时 INS_ShowRandom),
 *    随后无论是否推送都调 ORACLE_Delivered 消耗该时刻 */

uint8_t ORACLE_Due(void);       /* 1=当前时刻已到下一推送点(自动按日/设置重排程) */
void ORACLE_Delivered(void);    /* 消耗当前推送点, 推进到下一时刻 */
uint32_t ORACLE_Count(void);    /* 神谕累计接收次数(NVS "oracle"/"cnt" 持久化) */


/* 每日签计数(NVS "info"/"dsign"): 换日首刷自增一次, 系统信息页展示 */
void ORACLE_DsignInc(void);

#endif
