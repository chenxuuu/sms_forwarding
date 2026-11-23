idf_component_register(
    SRCS "src/ota.c"
    INCLUDE_DIRS "include"
    REQUIRES core app_update bootloader_support esp_app_format esp_http_client
             mbedtls nvs_flash esp_timer net notify
)
