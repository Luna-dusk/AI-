set(src_dirs
                "LED_Driver"
                "I2S"
                "AI_client"
                "Wavecode"
                "WIFI"
                "AFE_SR"
                "KEY"
                "MultiNet"
                "LCD")

set(include_dirs
                "LED_Driver"
                "I2S"
                "AI_client"
                "Wavecode"
                "WIFI"
                "AFE_SR"
                "KEY"
                "MultiNet"
                "LCD")

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
                mbedtls
                esp-sr
                esp_lcd
                lvgl__lvgl)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
