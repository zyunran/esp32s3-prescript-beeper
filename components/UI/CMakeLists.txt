set(srcs
        UI.c)

set(include_dirs
        .)

set(requires
        driver LCD KEY nvs_flash)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
