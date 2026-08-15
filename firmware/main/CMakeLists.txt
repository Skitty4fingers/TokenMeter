idf_component_register(
    SRCS
        "main.c"
        "app_state.c"
        "board/lcd.c"
        "board/led.c"
        "board/sdcard.c"
        "net/wifi_mgr.c"
        "net/webui.c"
        "net/sntp_sync.c"
        "providers/claude.c"
        "providers/openai.c"
        "providers/http_util.c"
        "ui/theme.c"
        "ui/ui_dashboard.c"
        "ui/ui_provision.c"
        "storage/config.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client
        esp-tls mbedtls esp_driver_spi esp_driver_gpio esp_driver_ledc esp_lcd
        fatfs sdmmc esp_timer dns_server esp_driver_sdspi vfs esp_app_format app_update
    EMBED_TXTFILES "www/index.html"
)

# snprintf into sized buffers is intentional truncation everywhere in this app
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
