idf_component_register(
    SRCS "src/sched.c"
    INCLUDE_DIRS "include"
    REQUIRES core modem notify nvs_flash esp_timer
)
