/* SPI: SPI2 批量发送(轮询,≤4096B)。时钟/设备句柄在 lcd_init。单写者=ui_task。 */
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

void spi2_write_data(uint8_t *data, int len)
{
    esp_err_t err;
    spi_transaction_t t = {0};

    if (!spi2_handle || !data || len <= 0) return;   /* 防悬垂句柄/空参数 */
    t.length = len * 8;
    t.tx_buffer = data;
    err = spi_device_polling_transmit(spi2_handle, &t);
    if (err != ESP_OK)
    {
        static uint8_t logged = 0;   /* 整屏 11 块/帧, 失败会连发: 只记首条, 防无声丢帧 */
        if (!logged)
        {
            ESP_LOGE(TAG, "polling_transmit failed: %s (帧内容可能缺失, 重绘自愈)",
                     esp_err_to_name(err));
            logged = 1;
        }
    }
}