set(srcs
        UI.c
        KEY.c
        LOOM.c)

set(include_dirs
        .)

set(requires
        driver LCD nvs_flash COMMON esp_timer)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
