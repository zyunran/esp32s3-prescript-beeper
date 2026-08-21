#ifndef __MYLCD_H_
#define __MYLCD_H_

#include <stdint.h>

/* 2.25寸屏: 横屏 284x76, 列偏移18, 行偏移82
 *
 * 接线 (SPI2 硬件SPI):
 *   屏 SCL  -> GPIO7   屏 CS  -> GPIO9
 *   屏 SDA  -> GPIO8   屏 DC  -> GPIO11
 *   屏 RST  -> GPIO10  屏 BLK -> GPIO12(低电平点亮, 悬空=熄屏)
 *   屏 VCC  -> 3.3V    屏 GND -> GND   (MISO 不接)
 */
#define LCD_WIDTH   284
#define LCD_HEIGHT  76

#define LCD_RST(x)  x ? gpio_set_level(GPIO_NUM_10,1) : gpio_set_level(GPIO_NUM_10,0)
#define LCD_DC(x)   x ? gpio_set_level(GPIO_NUM_11,1) : gpio_set_level(GPIO_NUM_11,0)
#define LCD_CS(x)   x ? gpio_set_level(GPIO_NUM_9,1) : gpio_set_level(GPIO_NUM_9,0)
/* 注意: LCD 全接口(含背光 lcd_on/lcd_off)是单写者约定 —— 只允许 ui_task 调用,
 * 任何 httpd/其他任务直写帧缓冲或 GPIO 都会撕裂画面(见审查 #16) */

/* 本屏显存 284×76, 分辨率为 284x76 横屏 */

#define WHITE           0xFFFF      /* 白色 */
#define BLACK           0x0000      /* 黑色 */
#define RED             0xF800      /* 红色 */
#define GREEN           0x07E0      /* 绿色 */
#define BLUE            0x001F      /* 蓝色 */ 
#define MAGENTA         0XF81F      /* 品红色/紫红色 = BLUE + RED */
#define YELLOW          0XFFE0      /* 黄色 = GREEN + RED */
#define CYAN            0X07FF      /* 青色 = GREEN + BLUE */  
#define BROWN           0XBC40      /* 棕色 */
#define BRRED           0XFC07      /* 棕红色 */
#define GRAY            0X8430      /* 灰色 */ 
#define DARKBLUE        0X01CF      /* 深蓝色 */
#define LIGHTBLUE       0X7D7C      /* 浅蓝色 */ 
#define GRAYBLUE        0X5458      /* 灰蓝色 */ 
#define LIGHTGREEN      0X841F      /* 浅绿色 */  
#define LGRAY           0XC618      /* 浅灰色(PANNEL),窗体背景色 */ 
#define LGRAYBLUE       0XA651      /* 浅灰蓝色(中间层颜色) */ 
#define LBBLUE          0X2B12      /* 浅棕蓝色(选择条目的反色) */

/* ================= 全局界面主题色(运行时可改, 默认护眼"柔和绿") =================
 * 主界面/子菜单/抽卡/破译共用背景与文字色; 变量由 lcd.c 定义,
 * WEB 配置页可改并写入 NVS, 重启后仍生效. */
extern uint16_t UI_COLOR_BG;    /* 背景 */
extern uint16_t UI_COLOR_MENU;  /* 菜单文字 */
extern uint16_t UI_COLOR_FRAME; /* 选中项线框 */
extern uint16_t UI_COLOR_ICON;  /* 指令图标 */
extern uint16_t UI_COLOR_TIME;  /* 左侧时钟 */
extern uint16_t UI_COLOR_DATE;  /* 左上角日期 */








void lcd_write_cmd(uint8_t cmd);
void lcd_write_data(uint8_t data);
void lcd_write_data16(uint16_t data);
void lcd_write_datan(uint8_t *data,uint16_t length);
void lcd_hard_reset(void);
void lcd_set_window(uint16_t xstar, uint16_t ystar,uint16_t xend,uint16_t yend);

void lcd_clear(uint16_t color);
void lcd_init(void);
void lcd_on(void);                       /* 显示: 背光点亮 */
void lcd_off(void);                      /* 熄屏: 背光脚悬空真正关断 */
void lcd_sleep_hold(void);               /* 浅睡眠保持 CS/RST/DC 输出高(防唤醒白屏) */

#endif
