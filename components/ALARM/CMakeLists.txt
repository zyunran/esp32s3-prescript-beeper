set(srcs
        ALARM.c)

set(include_dirs
        .)

set(requires
        UI INSTRUCTION NET nvs_flash esp_timer esp_hw_support)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
