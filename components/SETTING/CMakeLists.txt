set(srcs
        SETTING.c
        ORACLE.c)

set(include_dirs
        .)

set(requires
        UI INSTRUCTION AUDIO MPU6050 GACHA NET nvs_flash esp_hw_support)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
