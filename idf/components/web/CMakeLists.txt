idf_component_register(
    SRCS "src/web_server.c" "src/web_api.c" "src/web_ui.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES core modem notify net sched esp_http_server mbedtls
    EMBED_TXTFILES "web/index.html"
)
