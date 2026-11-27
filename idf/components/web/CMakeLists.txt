idf_component_register(
    SRCS "src/web_server.c" "src/web_ui.c" "src/web_json.c" "src/web_at.c"
         "src/web_sms_api.c" "src/web_query_api.c" "src/web_modem_api.c"
         "src/web_sys_api.c" "src/web_ota_api.c" "src/web_esim_api.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES core modem notify net sched ota esim esp_http_server mbedtls
    EMBED_TXTFILES "web/index.html"
)
