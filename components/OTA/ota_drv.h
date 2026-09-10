#ifndef __OTA_DRV_H
#define __OTA_DRV_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OTA: 无内置 URL。可空 SHA 则跳过校验(后续应强制)。启动后 mark_valid 防误回滚。 */

#define OTA_URL_MAX    256
#define OTA_SHA256_MAX 65

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_DONE,
    OTA_STATE_FAIL,
} ota_state_t;

/* 初始化：加载 NVS 配置 */
esp_err_t ota_drv_init(void);
/* 配置升级地址/SHA256(可只配 url；sha 为空则跳过校验) */
esp_err_t ota_drv_set_url(const char *url);
esp_err_t ota_drv_set_sha256(const char *sha256_hex);
const char *ota_drv_url(void);
const char *ota_drv_sha256(void);
uint8_t    ota_drv_configured(void);

/* 开始升级（非阻塞，内部创建后台任务） */
esp_err_t ota_drv_start(void);

/* 查询状态 */
uint8_t  ota_drv_busy(void);
uint8_t  ota_drv_success(void);
uint8_t  ota_drv_failed(void);
int      ota_drv_progress(void);          /* 0-100，未知返回 -1 */
void     ota_drv_status(char *buf, size_t n);

/* 启动后标记当前固件有效，防止误回滚 */
esp_err_t ota_drv_mark_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_DRV_H */
