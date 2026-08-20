set(srcs
        NET.c)

set(include_dirs
        .)

set(requires
        esp_wifi esp_netif esp_event lwip nvs_flash log esp_timer
        esp_http_client esp-tls json DS1302)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
