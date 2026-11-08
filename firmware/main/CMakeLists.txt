idf_component_register(
    SRCS "main.c" "serial_keys.c"
    INCLUDE_DIRS "."
    REQUIRES fh1_users fh1_core fh1_board fh1_storage fh1_ui fh1_apps fh1_devconsole fh1_ota fh1_bridge esp_timer esp_driver_uart esp_psram spi_flash esp_app_format
)
