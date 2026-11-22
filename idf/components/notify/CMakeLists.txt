idf_component_register(
    SRCS "src/notify.c" "src/email.c" "src/push.c"
    INCLUDE_DIRS "include"
    REQUIRES core esp_http_client esp-tls mbedtls lwip
)
