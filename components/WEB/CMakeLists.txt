idf_component_register(SRCS "web.c"
                    "web_page.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server json LCD INSTRUCTION AUDIO TIMER NET SETTING GACHA TODO POWER OTA CLOUD nvs_flash esp_timer)
