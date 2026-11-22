idf_component_register(
    SRCS "src/wifi.c" "src/time_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES core esp_wifi esp_netif esp_event lwip
)
