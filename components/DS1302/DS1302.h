#ifndef __DS1302_H
#define __DS1302_H

#include <stdint.h>
#include <time.h>

/* DS1302: RTC 读写 API。 */

void DS1302_Init(void);             /* GPIO 初始化(输出低) */
uint8_t DS1302_Read(struct tm *t);  /* 1=读到有效时间(BCD/范围/振荡器已起振) */
void DS1302_Write(const struct tm *t); /* 写入本地时间(清写保护+清CH起振) */

#endif
