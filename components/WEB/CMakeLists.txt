idf_component_register(SRCS "web.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server json LCD INSTRUCTION ALARM NET SETTING GACHA TODO ANSWER BATTERY nvs_flash esp_timer)
