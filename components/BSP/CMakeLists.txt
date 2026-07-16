set(src_dirs
                "LED_Driver"
                "I2S"
                "AI_client"
                "Wavecode"
                "WIFI")

set(include_dirs
                "LED_Driver"
                "I2S"
                "AI_client"
                "Wavecode"
                "WIFI")

set(requires
                nvs_flash
                esp_netif
                esp_wifi
                esp_event
                esp_http_client
                esp_http_server
                driver
                esp_timer
                freertos
                log
                mbedtls)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
