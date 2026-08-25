set(srcs
        POWER.c
        battery.c)

set(include_dirs
        .)

set(requires
        LCD esp_adc esp_timer esp_hw_support)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
