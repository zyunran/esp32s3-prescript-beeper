#ifndef __LCDCHINESE_H
#define __LCDCHINESE_H

#include <stdint.h>

/* 全量 16×16 字库(定义在 lcdzh.c, 存 flash, 不占 RAM) */
extern const uint16_t lcd_font_codes[];
extern const uint8_t  lcd_font_data[][32];
extern const uint16_t lcd_font_count;

/* 查一个 UTF-8 字符(ASCII 或中文)的 16×16 字模; 全字库二分 → 默认方框 */
const uint8_t *lcd_font_get(const char *ch);

#endif
