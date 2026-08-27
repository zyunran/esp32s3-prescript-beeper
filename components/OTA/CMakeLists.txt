idf_component_register(
    SRCS "ota_drv.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_http_client app_update esp_partition esp_timer mbedtls)
