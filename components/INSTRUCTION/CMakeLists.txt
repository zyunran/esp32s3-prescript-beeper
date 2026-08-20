set(srcs
        INSTRUCTION.c)

set(include_dirs
        .)

set(requires
        driver LCD UI SOUND TODO esp_timer esp_hw_support nvs_flash)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
