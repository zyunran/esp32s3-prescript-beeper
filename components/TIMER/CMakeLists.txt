set(srcs
        TIMER.c)

set(include_dirs
        .)

set(requires
        UI INSTRUCTION esp_timer)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
