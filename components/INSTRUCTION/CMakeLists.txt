set(srcs
        INSTRUCTION.c
        ANSWER.c)

set(include_dirs
        .)

set(requires
        driver LCD UI AUDIO TODO esp_timer esp_hw_support nvs_flash)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
