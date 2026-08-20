set(srcs
        SETTING.c)

set(include_dirs
        .)

set(requires
        UI INSTRUCTION SOUND MPU6050 ORACLE GACHA nvs_flash)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
