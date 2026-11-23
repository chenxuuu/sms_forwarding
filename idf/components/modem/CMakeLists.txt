idf_component_register(
    SRCS
        "src/at_client.c"
        "src/pdu.c"
        "src/sms_assembler.c"
        "src/modem_hw.c"
        "src/netmon.c"
        "src/modem.c"
    INCLUDE_DIRS "include"
    REQUIRES core esp_driver_uart esp_driver_gpio esp_timer
)
