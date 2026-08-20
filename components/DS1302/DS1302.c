/* DS1302 实时时钟组件: 3 线 bit-bang(GPIO2=CLK / GPIO14=DAT / GPIO21=RST/CE).
 * 协议要点(DS1302 数据手册, 非 I2C):
 *  - CE 高有效, 整个读写期间保持高, 结束拉低; DAT 为双向线
 *  - 命令字节 8 位 LSB 先发; 写数据 LSB 先发; 读数据 LSB 先收
 *  - 写: 数据在 SCLK 上升沿被 DS1302 锁存(主控在上升沿前把 DAT 摆好)
 *  - 读: 命令字节第 8 位下降沿后 DS1302 开始驱动 DAT, 主控在 SCLK 拉高前采样(数据已稳定)
 *  - 时间寄存器为 BCD; 突发读 0xBF 从秒起连读 8 字节, 突发写 0xBE 连写
 *  - 写前须清写保护(0x8E), 写秒时清 CH(bit7) 使振荡器启动 */
#include "DS1302.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include <stdio.h>

#define DS1302_CLK    GPIO_NUM_2     /* 时钟 */
#define DS1302_DAT    GPIO_NUM_14    /* 双向数据 */
#define DS1302_RST    GPIO_NUM_21    /* 片选 CE */

#define DS1302_READ_BURST   0xBF     /* 从秒寄存器起连读 8 字节(秒..年+WP) */
#define DS1302_WRITE_BURST  0xBE     /* 连写 8 字节 */
#define DS1302_WP_REG       0x8E     /* 写保护寄存器(写命令) */

static void rtc_delay(void) { esp_rom_delay_us(2); }   /* SCLK 脉宽 >= 2us(手册 ~250ns), 更稳 */

static void rtc_clk(uint8_t v) { gpio_set_level(DS1302_CLK, v); }
static void rtc_rst(uint8_t v) { gpio_set_level(DS1302_RST, v); }

static void rtc_dat_out(uint8_t v)
{
    gpio_set_direction(DS1302_DAT, GPIO_MODE_OUTPUT);
    gpio_set_level(DS1302_DAT, v);
}

/* 读方向: 释放 DAT 由 DS1302 驱动(开内部上拉, 无芯片时读全1 -> BCD 校验判无效) */
static void rtc_dat_release(void)
{
    gpio_set_direction(DS1302_DAT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DS1302_DAT, GPIO_PULLUP_ONLY);
}

static int rtc_dat_read(void) { return gpio_get_level(DS1302_DAT); }

/* 发 1 字节(LSB 先) */
static void rtc_tx_byte(uint8_t d)
{
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        rtc_dat_out((d >> i) & 1);
        rtc_delay();
        rtc_clk(1); rtc_delay();
        rtc_clk(0); rtc_delay();
    }
}

/* 收 1 字节(LSB 先): 数据在 SCLK 下降沿更新, 采样在其后的低电平期 */
static uint8_t rtc_rx_byte(void)
{
    uint8_t i, d = 0;
    rtc_dat_release();
    for (i = 0; i < 8; i++)
    {
        d |= (uint8_t)(rtc_dat_read() << i);
        rtc_clk(1); rtc_delay();
        rtc_clk(0); rtc_delay();
    }
    return d;
}

static void rtc_begin(void) { rtc_rst(1); rtc_delay(); rtc_delay(); }   /* CE 高, 传输开始 */
static void rtc_end(void)   { rtc_clk(0); rtc_delay(); rtc_rst(0); rtc_delay(); }  /* CE 低, 结束 */

void DS1302_Init(void)
{
    gpio_set_direction(DS1302_CLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(DS1302_DAT, GPIO_MODE_OUTPUT);
    gpio_set_direction(DS1302_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(DS1302_CLK, 0);
    gpio_set_level(DS1302_DAT, 0);
    gpio_set_level(DS1302_RST, 0);
}

/* ================= BCD 工具 ================= */
static uint8_t rtc_bcd2u(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }
static uint8_t rtc_u2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* 高/低 4 位都 <=9 才算合法 BCD */
static uint8_t rtc_bcd_ok(uint8_t v) { return ((v & 0x0F) <= 9) && ((v & 0xF0) <= 0x90); }

/* 突发读 + 校验: 全部字段 BCD 合法且范围合理 + 秒 CH=0(振荡器运行) + 时 24h 制(bit7=0) */
uint8_t DS1302_Read(struct tm *t)
{
    uint8_t buf[8], i;
    uint8_t sec, min, hour, mday, mon, year;
    rtc_begin();
    rtc_tx_byte(DS1302_READ_BURST);
    for (i = 0; i < 8; i++) buf[i] = rtc_rx_byte();
    rtc_end();

    /* 有效性: 全部字段 BCD 合法 + 秒 CH=0(振荡器运行) + 时 24h 制(bit7=0) */
    {
        uint8_t ok = 1;
        if (!rtc_bcd_ok(buf[0]) || (buf[0] & 0x80)) ok = 0;   /* 秒: CH 置位=振荡器停 */
        if (!rtc_bcd_ok(buf[1])) ok = 0;                       /* 分 */
        if (!rtc_bcd_ok(buf[2]) || (buf[2] & 0x80)) ok = 0;    /* 时: bit7=12h 制, 拒 */
        if (!rtc_bcd_ok(buf[3])) ok = 0;                       /* 日 */
        if (!rtc_bcd_ok(buf[4])) ok = 0;                       /* 月 */
        if (!rtc_bcd_ok(buf[6])) ok = 0;                       /* 年 */
        if (ok)
        {
            sec  = rtc_bcd2u(buf[0]);
            min  = rtc_bcd2u(buf[1]);
            hour = rtc_bcd2u(buf[2]);
            mday = rtc_bcd2u(buf[3]);
            mon  = rtc_bcd2u(buf[4]);
            year = rtc_bcd2u(buf[6]);
            if (sec > 59 || min > 59 || hour > 23) ok = 0;
            if (mday < 1 || mday > 31) ok = 0;
            if (mon < 1 || mon > 12) ok = 0;
        }
        if (!ok)
        {
            /* 硬件诊断: 秒CH=1=全新未起振(联网校时后自动初始化); 全FF/乱值=未接/供电/接线问题 */
            printf("[DS1302] read invalid, raw sec..yr,wp: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            return 0;
        }
    }

    t->tm_sec  = sec;
    t->tm_min  = min;
    t->tm_hour = hour;
    t->tm_mday = mday;
    t->tm_mon  = mon - 1;
    t->tm_year = (int)(2000 + year) - 1900;   /* DS1302 两位年 00-99 -> 2000-2099 */
    t->tm_isdst = 0;
    return 1;
}

/* 写入本地时间: 先清写保护, 再突发写 8 字节(秒清 CH 起振, 时用 24h 制) */
void DS1302_Write(const struct tm *t)
{
    uint8_t buf[8], i;
    rtc_begin();
    rtc_tx_byte(DS1302_WP_REG);
    rtc_tx_byte(0x00);             /* WP=0 允许写 */
    rtc_end();

    buf[0] = (uint8_t)(rtc_u2bcd((uint8_t)t->tm_sec) & 0x7F);          /* 清 CH 起振 */
    buf[1] = rtc_u2bcd((uint8_t)t->tm_min);
    buf[2] = rtc_u2bcd((uint8_t)t->tm_hour);
    buf[3] = rtc_u2bcd((uint8_t)t->tm_mday);
    buf[4] = rtc_u2bcd((uint8_t)(t->tm_mon + 1));
    buf[5] = rtc_u2bcd((uint8_t)(t->tm_wday + 1));                      /* tm_wday 0=周日..6=周六 -> DS1302 星期 1=周日..7=周六 */
    buf[6] = rtc_u2bcd((uint8_t)(t->tm_year % 100));
    buf[7] = 0x00;                /* WP 保持关闭, 下次直接可写 */

    rtc_begin();
    rtc_tx_byte(DS1302_WRITE_BURST);
    for (i = 0; i < 8; i++) rtc_tx_byte(buf[i]);
    rtc_end();
}
