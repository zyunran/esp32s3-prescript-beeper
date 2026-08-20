/* SPI 组件: SPI2 主机总线初始化 + 单字节/批量发送(供 LCD 用).
 * MISO 不接(屏只写); 具体时钟(60MHz)/设备句柄在 lcd.c lcd_init 中添加.
 * spi2_write_data 用轮询模式发送(POLLING, 不经队列), 单次长度受 max_transfer_sz 限制.
 * max_transfer_sz = 4096: 与 fb_blit/lcd_clear 的实际分块大小一致(整屏 43KB 也是分块发的),
 * 避免让驱动为整屏帧长预留过大内部 DMA 缓冲. */
#define SPI2_MAX_TRANSFER  4096
#include "spi.h"
#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "SPI";
spi_device_handle_t spi2_handle;

void spi2_init(void)
{
    esp_err_t err;
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
    err = spi_bus_initialize(SPI2_HOST, &spibus_structure, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return;   /* SPI 不可用: 后续写屏本就是空转, 避免悬垂句柄 */
    }
}

uint8_t spi2_transfer_byte(uint8_t data)
{
    spi_transaction_t t;

    if (!spi2_handle) return 0xFF;   /* 总线/设备未就绪: 返回无效, 防悬垂句柄崩溃 */
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

    if (!spi2_handle || !data || len <= 0) return;   /* 防悬垂句柄/空参数 */
    t.length = len * 8;                            
    t.tx_buffer = data;                            
    spi_device_polling_transmit(spi2_handle, &t);  
}