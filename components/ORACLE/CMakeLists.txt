set(srcs
        ORACLE.c)

set(include_dirs
        .)

set(requires
        SETTING NET esp_hw_support nvs_flash)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
