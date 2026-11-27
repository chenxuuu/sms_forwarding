idf_component_register(
    SRCS "src/esim.c" "src/esim_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES core modem esp_timer
)
