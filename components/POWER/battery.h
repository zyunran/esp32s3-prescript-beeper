#ifndef __BATTERY_H
#define __BATTERY_H

#include <stdint.h>

/* BATTERY 组件: 1S 锂电电压 -> 电量百分比(ADC1 读分压)
 *  - 引脚: GPIO1 = ADC1_CH0(改 BAT_ADC_CH 即可换引脚)
 *  - 分压: BAT_DIV = 分压比(电池V = ADC_V × 分压比; 1:1 分压即 2)
 *  - 未接电池/纯USB 时返回 255, UI 画空电池框
 *  - 适配 5V 充放电模块: 电量必须测电池端(B+/BAT+), 不能测模块 5V 输出(5V 恒定会永远显示满电)
 * 参考: 1S LiPo 2.9V(0%, 模块过放保护) ~ 4.2V(100%, 充电截止) */

#define BAT_ADC_UNIT ADC_UNIT_1
#define BAT_ADC_CH   ADC_CHANNEL_0      /* GPIO1 */
#define BAT_DIV      2                  /* 分压比(1:1 分压=2) */
#define BAT_V_EMPTY  2900               /* 0% 电池电压 mV(模块过放保护 2.9V) */
#define BAT_V_FULL   4200               /* 100% 电池电压 mV */
#define BAT_V_NONE   1000               /* 低于此视为未接电池 */

void   BAT_Init(void);      /* 初始化 ADC(幂等) */
uint8_t BAT_GetPct(void);   /* 0-100 电量%; 255=无电池 */

#endif
