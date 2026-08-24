idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES KEY LCD SPI UI INSTRUCTION GACHA NET SOUND SETTING TIMER ALARM TODO BATTERY ORACLE ANSWER LOOM POMODORO WEB MPU6050 DS1302 driver esp_timer esp_hw_support nvs_flash)
