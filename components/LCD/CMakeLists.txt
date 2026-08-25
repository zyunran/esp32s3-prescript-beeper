set(srcs
        LCD.c
        spi.c lcdzh.c
        lcd_font_data.c)

set(include_dirs
        .)

set(requires
        driver COMMON)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format -Wno-missing-braces)
