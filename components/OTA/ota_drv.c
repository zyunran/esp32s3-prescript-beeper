/* ota_drv.c - OTA 升级驱动
 *  - 下载地址与 SHA256 仅存 NVS "ota"，源码不硬编码任何 URL/凭证（隐私）
 *  - 后台任务流式下载到备用 OTA 分区，支持 HTTPS/重定向
 *  - 校验通过后设置启动分区并自动重启
 */
#include "ota_drv.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>

#define OTA_NS "ota"
#define OTA_TASK_STACK 8192
#define OTA_HTTP_BUF   2048

static const char *TAG = "OTA";

static char s_url[OTA_URL_MAX] = "";
static char s_sha[OTA_SHA256_MAX] = "";
static volatile ota_state_t s_state = OTA_STATE_IDLE;
static volatile uint8_t s_busy = 0;
static volatile int s_progress = -1;
static char s_status[64] = "未开始";

static void ota_set_status(const char *s)
{
    strncpy(s_status, s, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

static char ota_lower(char c)
{
    if (c >= 'A' && c <= 'F') return (char)(c - 'A' + 'a');
    return c;
}

static uint8_t ota_sha_hex_equal(const uint8_t *digest)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 32; i++)
    {
        char hi = hex[digest[i] >> 4];
        char lo = hex[digest[i] & 0x0F];
        if (ota_lower(s_sha[i * 2]) != hi || ota_lower(s_sha[i * 2 + 1]) != lo)
        {
            return 0;
        }
    }
    return 1;
}

static void ota_fail(esp_ota_handle_t handle, const char *msg)
{
    if (handle) esp_ota_abort(handle);
    s_state = OTA_STATE_FAIL;
    s_busy = 0;
    s_progress = -1;
    ota_set_status(msg);
}

static void ota_task(void *arg)
{
    esp_http_client_config_t cfg = {0};
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t handle = 0;
    const esp_partition_t *part = NULL;
    mbedtls_sha256_context sha;
    uint8_t digest[32];
    char buf[OTA_HTTP_BUF];
    int total = 0, content_len = -1;
    esp_err_t err;

    s_state = OTA_STATE_DOWNLOADING;
    s_progress = 0;
    ota_set_status("正在连接服务器...");

    cfg.url = s_url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = OTA_HTTP_BUF;
    cfg.disable_auto_redirect = false;   /* GitHub Release 会 302 跳转 */
    cfg.max_redirection_count = 5;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.user_agent = "ESP32-ODER-OTA";

    client = esp_http_client_init(&cfg);
    if (!client)
    {
        ota_fail(0, "HTTP 初始化失败");
        goto out;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ota_fail(0, "无法打开下载地址");
        goto out;
    }

    {
        int status = esp_http_client_get_status_code(client);
        int redirects = 0;

        /* GitHub Release 直链会 302 到 objects.githubusercontent.com；
         * 低层 esp_http_client_open() 不会自动跟随重定向，必须手动处理。 */
        while (status >= 300 && status < 400 && redirects < 5)
        {
            ESP_LOGW(TAG, "HTTP redirect %d, follow...", status);
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            err = esp_http_client_open(client, 0);
            if (err != ESP_OK)
            {
                ota_fail(0, "重定向后无法连接");
                goto out;
            }
            status = esp_http_client_get_status_code(client);
            redirects++;
        }

        if (status >= 400)
        {
            char m[64];
            snprintf(m, sizeof(m), "服务器返回 %d", status);
            ota_fail(0, m);
            goto out;
        }
        if (redirects >= 5)
        {
            ota_fail(0, "重定向次数过多");
            goto out;
        }
    }

    content_len = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "OTA start: url=%s status=200 content_len=%d", s_url, content_len);

    part = esp_ota_get_next_update_partition(NULL);
    if (!part)
    {
        ota_fail(0, "无 OTA 分区");
        goto out;
    }

    err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK)
    {
        ota_fail(0, "OTA 分区打开失败");
        goto out;
    }

    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    ota_set_status("正在下载固件...");
    {
        int last_log = 0;
        for (;;)
        {
            int r = esp_http_client_read(client, buf, sizeof(buf));
            if (r < 0)
            {
                ESP_LOGE(TAG, "read failed: %d (downloaded %d/%d)", r, total, content_len);
                ota_fail(handle, "下载中断");
                mbedtls_sha256_free(&sha);
                goto out;
            }
            if (r == 0) break;

            if (esp_ota_write(handle, buf, (size_t)r) != ESP_OK)
            {
                ota_fail(handle, "固件写入失败");
                mbedtls_sha256_free(&sha);
                goto out;
            }
            mbedtls_sha256_update(&sha, (unsigned char *)buf, (size_t)r);

            total += r;
            if (content_len > 0)
            {
                s_progress = total * 100 / content_len;
                if (s_progress > 100) s_progress = 100;
            }
            if (total - last_log >= 256 * 1024)
            {
                last_log = total;
                ESP_LOGI(TAG, "downloading %d/%d", total, content_len);
            }
        }
    }

    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (content_len > 0 && total != content_len)
    {
        ESP_LOGE(TAG, "size mismatch: got %d, expected %d", total, content_len);
        ota_fail(0, "下载大小不匹配");
        goto out;
    }

    s_state = OTA_STATE_VERIFYING;
    ota_set_status("正在校验固件...");

    err = esp_ota_end(handle);
    handle = 0;
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed: %d (downloaded %d bytes, content_len %d)",
                 (int)err, total, content_len);
        ota_fail(0, "固件无效或下载不完整");
        goto out;
    }

    if (s_sha[0])
    {
        if (strlen(s_sha) != 64 || !ota_sha_hex_equal(digest))
        {
            ota_fail(0, "SHA256 校验失败");
            goto out;
        }
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK)
    {
        ota_fail(0, "设置启动分区失败");
        goto out;
    }

    s_state = OTA_STATE_DONE;
    s_progress = 100;
    s_busy = 0;
    ota_set_status("升级成功，即将重启...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();
    return;

out:
    if (client)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    vTaskDelete(NULL);
}

esp_err_t ota_drv_init(void)
{
    nvs_handle_t h;
    s_state = OTA_STATE_IDLE;
    s_busy = 0;
    s_progress = -1;
    ota_set_status("未开始");

    if (nvs_open(OTA_NS, NVS_READONLY, &h) == ESP_OK)
    {
        size_t n = sizeof(s_url);
        nvs_get_str(h, "url", s_url, &n);
        n = sizeof(s_sha);
        nvs_get_str(h, "sha", s_sha, &n);
        nvs_close(h);
    }
    return ESP_OK;
}

esp_err_t ota_drv_set_url(const char *url)
{
    nvs_handle_t h;
    if (!url || strlen(url) >= sizeof(s_url)) return ESP_ERR_INVALID_ARG;
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    if (nvs_open(OTA_NS, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_str(h, "url", s_url);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

esp_err_t ota_drv_set_sha256(const char *sha)
{
    nvs_handle_t h;
    if (!sha || strlen(sha) >= sizeof(s_sha)) return ESP_ERR_INVALID_ARG;
    strncpy(s_sha, sha, sizeof(s_sha) - 1);
    s_sha[sizeof(s_sha) - 1] = '\0';
    if (nvs_open(OTA_NS, NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_str(h, "sha", s_sha);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

const char *ota_drv_url(void)     { return s_url; }
const char *ota_drv_sha256(void)  { return s_sha; }

uint8_t ota_drv_configured(void)
{
    return s_url[0] != '\0';
}

esp_err_t ota_drv_start(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!ota_drv_configured()) return ESP_ERR_INVALID_STATE;

    s_busy = 1;
    s_state = OTA_STATE_DOWNLOADING;
    s_progress = 0;
    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL, 1, NULL) != pdPASS)
    {
        s_busy = 0;
        s_state = OTA_STATE_FAIL;
        ota_set_status("创建 OTA 任务失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint8_t ota_drv_busy(void)    { return s_busy; }
uint8_t ota_drv_success(void) { return s_state == OTA_STATE_DONE; }
uint8_t ota_drv_failed(void)  { return s_state == OTA_STATE_FAIL; }
int     ota_drv_progress(void){ return s_progress; }

void ota_drv_status(char *buf, size_t n)
{
    if (!buf || n == 0) return;
    if (s_state == OTA_STATE_DOWNLOADING && s_progress >= 0)
    {
        snprintf(buf, n, "%s (%d%%)", s_status, s_progress);
    }
    else
    {
        strncpy(buf, s_status, n - 1);
        buf[n - 1] = '\0';
    }
}
esp_err_t ota_drv_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}
