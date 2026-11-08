idf_component_register(
    SRCS "console.c" "wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_core fh1_bridge fh1_storage fh1_board fh1_ota app_update esp_wifi esp_netif esp_http_server esp_timer esp_driver_tsens
    EMBED_TXTFILES "web/index.html"
)
