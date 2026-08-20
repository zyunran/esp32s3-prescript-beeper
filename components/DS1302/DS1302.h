#ifndef __DS1302_H
#define __DS1302_H

#include <stdint.h>
#include <time.h>

/* DS1302 实时时钟组件: 3 线 bit-bang(非 I2C), 模块自带纽扣电池+晶振, 断电继续走时.
 *  - GPIO2=CLK / GPIO14=DAT(双向) / GPIO21=RST(CE)
 *  - DS1302_Read: 突发读 8 字节 + BCD/范围/CH(起振)校验, 有效返回 1
 *  - DS1302_Write: 自动清写保护 + 清 CH 起振, 24 小时制写入
 * 上电流程(main.c): DS1302_Read 有效 -> settimeofday 设系统时钟 -> NET_TimeAdopt 标记时间有效
 * 联网校时(NET.c): net_sntp_synced 里 DS1302_Write 写回, 校准/初始化 DS1302 */

void DS1302_Init(void);             /* GPIO 初始化(输出低) */
uint8_t DS1302_Read(struct tm *t);  /* 1=读到有效时间(BCD/范围/振荡器已起振) */
void DS1302_Write(const struct tm *t); /* 写入本地时间(清写保护+清CH起振) */

#endif
