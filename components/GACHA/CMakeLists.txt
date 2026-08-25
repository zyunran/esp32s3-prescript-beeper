set(srcs
        GACHA.c gacha_data.c gacha_coin_img.c)

set(include_dirs
        .)

set(requires
        LCD UI INSTRUCTION AUDIO esp_hw_support esp_timer nvs_flash)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
