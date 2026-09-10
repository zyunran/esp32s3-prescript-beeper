#ifndef __MYLCD_H_
#define __MYLCD_H_

#include <stdint.h>
#include "color.h"

/* ST7789 284×76 横屏, SPI2
 * 接线: SCL=7 SDA=8 CS=9 RST=10 DC=11 BLK=12(低电平点亮, 悬空熄屏)
 * 全接口(含背光)单写者: 只允许 ui_task 调用, 否则画面撕裂 */
#define LCD_WIDTH   284
#define LCD_HEIGHT  76

#define LCD_RST(x)  x ? gpio_set_level(GPIO_NUM_10,1) : gpio_set_level(GPIO_NUM_10,0)
#define LCD_DC(x)   x ? gpio_set_level(GPIO_NUM_11,1) : gpio_set_level(GPIO_NUM_11,0)
#define LCD_CS(x)   x ? gpio_set_level(GPIO_NUM_9,1) : gpio_set_level(GPIO_NUM_9,0)

void lcd_write_cmd(uint8_t cmd);
void lcd_write_data(uint8_t data);
void lcd_write_data16(uint16_t data);
void lcd_write_datan(uint8_t *data,uint16_t length);
void lcd_hard_reset(void);
void lcd_set_window(uint16_t xstar, uint16_t ystar,uint16_t xend,uint16_t yend);

void lcd_clear(uint16_t color);
void lcd_init(void);
void lcd_on(void);
void lcd_off(void);                 /* 背光脚悬空关断 */
void lcd_sleep_hold(void);          /* 浅睡眠保持 CS/RST/DC 高, 防唤醒白屏 */

#endif
