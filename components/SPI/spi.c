/* SPI 组件: SPI2 主机总线初始化 + 单字节/批量发送(供 LCD 用).
 * MISO 不接(屏只写); 具体时钟(60MHz)/设备句柄在 lcd.c lcd_init 中添加.
 * spi2_write_data 用轮询模式发送(POLLING, 不经队列), 单次长度受 max_transfer_sz 限制.
 * max_transfer_sz = 整屏帧长(284*76*2=43168): 实际按 4096B 分块发送,
 * 不再沿用旧 240×240 屏的 115200 上限(避免驱动为大值预留过多内部 RAM).
 * 此处不 include LCD.h, 避免 SPI(底层)反向依赖 LCD, 帧长以常数写死并与之对齐. */
#define SPI2_MAX_TRANSFER  (284 * 76 * 2)
#include "spi.h"
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"

spi_device_handle_t spi2_handle;

void spi2_init(void)
{
    spi_bus_config_t  spibus_structure = {
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .max_transfer_sz = SPI2_MAX_TRANSFER,
        .miso_io_num = -1,          /* 屏只写，不接 MISO */
        .mosi_io_num = GPIO_NUM_8,
        .sclk_io_num = GPIO_NUM_7,
        .quadhd_io_num = -1,
        .quadwp_io_num = -1,
    };
    spi_bus_initialize(SPI2_HOST,& spibus_structure, SPI_DMA_CH_AUTO);
}

uint8_t spi2_transfer_byte(uint8_t data)
{
    spi_transaction_t t;

    memset(&t, 0, sizeof(t));

    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 8;
    t.tx_data[0] = data;
    spi_device_polling_transmit(spi2_handle, &t);

    return t.rx_data[0];
}

void spi2_write_data(uint8_t *data, int len)
{
    spi_transaction_t t = {0};

    t.length = len * 8;                            
    t.tx_buffer = data;                            
    spi_device_polling_transmit(spi2_handle, &t);  
}