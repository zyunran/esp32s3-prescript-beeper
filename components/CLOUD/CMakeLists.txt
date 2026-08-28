idf_component_register(SRCS "cloud.c"
                    "cloud_onenet.c"
                    "cloud_token.c"
                    INCLUDE_DIRS "."
                    REQUIRES mqtt json esp_wifi esp_app_format esp_timer nvs_flash NET POWER)
