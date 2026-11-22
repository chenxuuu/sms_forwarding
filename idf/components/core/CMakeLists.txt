idf_component_register(
    SRCS "src/log.c" "src/sbuf.c" "src/util.c" "src/config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash mbedtls esp_timer
)
