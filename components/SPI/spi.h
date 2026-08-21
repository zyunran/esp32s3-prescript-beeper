#ifndef __MYSPI_H_
#define __MYSPI_H_

/* SPI 组件: SPI2 主机总线(供 LCD 用).
 * - spi2_init:       初始化 SPI2 总线(60MHz 时钟与设备句柄在 lcd.c lcd_init 添加, MISO 不接)
 * - spi2_write_data: 批量发送(POLLING 轮询模式, 不经队列; 单次长度受 max_transfer_sz 限制)
 * 句柄 spi2_handle 由 lcd 组件复用. */

#include "driver/spi_master.h"

extern spi_device_handle_t spi2_handle;

void spi2_init(void);
void spi2_write_data(uint8_t *data, int len);
#endif
