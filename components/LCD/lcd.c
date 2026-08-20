#include "lcd.h"
#include "lcdfont.h"
#include "lcdchinese.h"
#include "spi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 界面主题色(运行时可改: WEB 配置页写入并持久化 NVS; 默认护眼"柔和绿") */
uint16_t UI_COLOR_BG     = 0x10C2;   /* #141A14 深暖绿黑 */
uint16_t UI_COLOR_MENU   = 0xCF18;   /* #C8E0C0 淡绿白 */
uint16_t UI_COLOR_FRAME  = 0x8EB1;   /* #8FD48A 柔和绿 */
uint16_t UI_COLOR_ICON   = 0x7E9A;   /* #7FD0D0 柔和青 */
uint16_t UI_COLOR_TIME   = 0xCF18;   /* #C8E0C0 淡绿白 */
uint16_t UI_COLOR_DATE   = 0xCF18;   /* #C8E0C0 淡绿白 */

void lcd_write_cmd(uint8_t cmd)
{
    LCD_DC(0);
    spi2_write_data(&cmd,1);
}

void lcd_write_data(uint8_t data)
{
    LCD_DC(1);
    spi2_write_data(&data,1);
}

void lcd_write_data16(uint16_t data)
{
    uint8_t databuf[2] = {0,0};
    databuf[0] = data >> 8 ;
    databuf[1] = data & 0xFF ;
    LCD_DC(1);
    spi2_write_data(databuf,2);
}

void lcd_write_datan(uint8_t *data,uint16_t length)
{
    LCD_DC(1);
    spi2_write_data(data,length);
}

void lcd_hard_reset(void)
{
    LCD_RST(0);
    vTaskDelay(pdMS_TO_TICKS(100));   /* 复位拉低 ≥100ms(ST7789 要求 ≥10µs, 留足余量) */
    LCD_RST(1);
    vTaskDelay(pdMS_TO_TICKS(100));   /* 释放后等待稳定 */
}

/* 背光: GPIO12, 有源背光, 低电平点亮(无 PWM, 仅 显示/熄屏 两态)
 * 熄屏必须把引脚置输入悬空(高阻)才能真正关断背光; 拉低仍是点亮. */
static void lcd_bl_init(void)
{
    gpio_set_drive_capability(GPIO_NUM_12, GPIO_DRIVE_CAP_3);
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_12, 0);      /* 低电平点亮 */
}

void lcd_on(void)
{
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_12, 0);      /* 拉低点亮背光 */
    vTaskDelay(pdMS_TO_TICKS(10));       /* 背光点亮后稳定 10ms(pdMS_TO_TICKS 已正确把 ms 换算成 tick) */
}

void lcd_off(void)
{
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);   /* 输入悬空 = 真正熄屏 */
    gpio_set_pull_mode(GPIO_NUM_12, GPIO_FLOATING);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* 浅睡眠期间保持 LCD 控制脚为输出(空闲高电平): 防系统 isolate 把引脚浮空 ->
 * RST 浮空拉低 -> 屏控制器复位 -> 唤醒白屏; CS 浮空拉低 -> 屏吃 SPI 毛刺 -> 显示错乱.
 * 在启动(初始化完成后)调用一次即可: 睡眠方向配置持久, 睡眠时按当前电平(高)保持. */
void lcd_sleep_hold(void)
{
    LCD_CS(1);
    LCD_RST(1);
    LCD_DC(1);                              /* 确保空闲高电平 */
    gpio_sleep_set_direction(GPIO_NUM_9,  GPIO_MODE_OUTPUT);
    gpio_sleep_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_sleep_set_direction(GPIO_NUM_11, GPIO_MODE_OUTPUT);
}

#define LCD_COLUMN_OFFSET   18
#define LCD_LINE_OFFSET     82

void lcd_set_window(uint16_t xstar, uint16_t ystar,uint16_t xend,uint16_t yend)
{
    lcd_write_cmd(0x2a);
    lcd_write_data16(xstar + LCD_COLUMN_OFFSET);
    lcd_write_data16(xend + LCD_COLUMN_OFFSET);
    lcd_write_cmd(0x2b);
    lcd_write_data16(ystar + LCD_LINE_OFFSET);
    lcd_write_data16(yend + LCD_LINE_OFFSET);
    lcd_write_cmd(0x2c);
}

/* 全屏清屏: 用 2KB 小块填充缓冲反复发送, 不再申请 42KB 全屏 lcd_buf(省 RAM, 行为不变) */
#define LCD_CLEAR_FILL  2048
void lcd_clear(uint16_t color)
{
    static uint8_t fill[LCD_CLEAR_FILL];
    uint32_t rem = (uint32_t)LCD_WIDTH * LCD_HEIGHT * 2;
    uint16_t i;
    for (i = 0; i < LCD_CLEAR_FILL / 2; i++)
    {
        fill[i * 2]     = (color >> 8) & 0xFF;
        fill[i * 2 + 1] =  color       & 0xFF;
    }
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    while (rem)
    {
        uint32_t n = (rem > LCD_CLEAR_FILL) ? LCD_CLEAR_FILL : rem;
        lcd_write_datan(fill, (uint16_t)n);
        rem -= n;
    }
}

void lcd_init(void)
{
    spi2_init();

    spi_device_interface_config_t   spidevice_structure = {0};
    spidevice_structure.clock_source = SPI_CLK_SRC_DEFAULT;
    spidevice_structure.clock_speed_hz = 60000000;
    spidevice_structure.mode = 0;
    spidevice_structure.queue_size = 7;
    spidevice_structure.spics_io_num = GPIO_NUM_9;
    if (spi_bus_add_device(SPI2_HOST, &spidevice_structure, &spi2_handle) != ESP_OK)
    {
        spi2_handle = NULL;   /* 添加失败: 置空句柄, 使后续写屏显式失效而非崩溃 */
    }

    gpio_config_t gpio_init_struct;
        /* WR管脚 */
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;                 /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;                       /* 配置输出模式 */
    gpio_init_struct.pin_bit_mask = 1ull << GPIO_NUM_11 ;           /* 配置引脚位掩码 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;          /* 失能下拉 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;               /* 使能上拉 */
    gpio_config(&gpio_init_struct);                                 /* 引脚配置 */
    /* 背光: GPIO12, 低电平点亮(普通GPIO, 无PWM, 见 lcd_bl_init) */
    lcd_bl_init();
    /* RST管脚 */
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;                 /* 失能引脚中断 */
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;                       /* 配置输出模式 */
    gpio_init_struct.pin_bit_mask = 1ull << GPIO_NUM_10;           /* 配置引脚位掩码 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;          /* 失能下拉 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;               /* 使能上拉 */
    gpio_config(&gpio_init_struct);                                 /* 引脚配置 */

    lcd_hard_reset();
    lcd_on();
    vTaskDelay(pdMS_TO_TICKS(100));   /* 复位后稳定 100ms */

    lcd_write_cmd(0x11);    /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));   /* Sleep out 后需 120ms 才能发后续命令(ST7789 规格) */

    lcd_write_cmd(0xB1);
    lcd_write_data(0x05);
    lcd_write_data(0x3C);
    lcd_write_data(0x3C);

    lcd_write_cmd(0xB2);
    lcd_write_data(0x05);
    lcd_write_data(0x3C);
    lcd_write_data(0x3C);

    lcd_write_cmd(0xB3);
    lcd_write_data(0x05);
    lcd_write_data(0x3C);
    lcd_write_data(0x3C);
    lcd_write_data(0x05);
    lcd_write_data(0x3C);
    lcd_write_data(0x3C);

    lcd_write_cmd(0xB4);    /* Dot inversion */
    lcd_write_data(0x03);

    lcd_write_cmd(0xC0);
    lcd_write_data(0x28);
    lcd_write_data(0x08);
    lcd_write_data(0x04);

    lcd_write_cmd(0xC1);
    lcd_write_data(0xC0);

    lcd_write_cmd(0xC2);
    lcd_write_data(0x0D);
    lcd_write_data(0x00);

    lcd_write_cmd(0xC3);
    lcd_write_data(0x8D);
    lcd_write_data(0x2A);

    lcd_write_cmd(0xC4);
    lcd_write_data(0x8D);
    lcd_write_data(0xEE);

    lcd_write_cmd(0xC5);    /* VCOM */
    lcd_write_data(0x1A);

    lcd_write_cmd(0x36);    /* MX, MY, RGB mode: 横屏 */
    lcd_write_data(0x70);

    lcd_write_cmd(0xE0);    /* Gamma */
    lcd_write_data(0x04);
    lcd_write_data(0x22);
    lcd_write_data(0x07);
    lcd_write_data(0x0A);
    lcd_write_data(0x2E);
    lcd_write_data(0x30);
    lcd_write_data(0x25);
    lcd_write_data(0x2A);
    lcd_write_data(0x28);
    lcd_write_data(0x26);
    lcd_write_data(0x2E);
    lcd_write_data(0x3A);
    lcd_write_data(0x00);
    lcd_write_data(0x01);
    lcd_write_data(0x03);
    lcd_write_data(0x13);

    lcd_write_cmd(0xE1);
    lcd_write_data(0x04);
    lcd_write_data(0x16);
    lcd_write_data(0x06);
    lcd_write_data(0x0D);
    lcd_write_data(0x2D);
    lcd_write_data(0x26);
    lcd_write_data(0x23);
    lcd_write_data(0x27);
    lcd_write_data(0x27);
    lcd_write_data(0x25);
    lcd_write_data(0x2D);
    lcd_write_data(0x3B);
    lcd_write_data(0x00);
    lcd_write_data(0x01);
    lcd_write_data(0x04);
    lcd_write_data(0x13);

    lcd_write_cmd(0x3A);    /* 65K 色彩 */
    lcd_write_data(0x05);

    lcd_write_cmd(0x29);    /* Display on */

    lcd_clear(UI_COLOR_BG);
}
