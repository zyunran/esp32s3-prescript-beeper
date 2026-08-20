set(srcs
        SOUND.c snd_data.c)

set(include_dirs
        .)

set(requires
        driver esp_driver_i2s freertos)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
