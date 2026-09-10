#ifndef __BATTERY_H
#define __BATTERY_H

#include <stdint.h>

/* BATTERY: 1S 锂电电压→电量%。GPIO1 ADC1_CH0。未接=255。 */

#define BAT_ADC_UNIT ADC_UNIT_1
#define BAT_ADC_CH   ADC_CHANNEL_0      /* GPIO1 */
#define BAT_DIV      2                  /* 分压比(1:1 分压=2) */
#define BAT_V_EMPTY  2900               /* 0% 电池电压 mV(模块过放保护 2.9V) */
#define BAT_V_FULL   4200               /* 100% 电池电压 mV */
#define BAT_V_NONE   1000               /* 低于此视为未接电池 */

void   BAT_Init(void);      /* 初始化 ADC(幂等) */
uint8_t BAT_GetPct(void);   /* 0-100 电量%; 255=无电池 */
uint16_t BAT_GetMillivolt(void); /* 电池端实时电压 mV; 0=无电池/读取失败 */

#endif
