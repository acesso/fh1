idf_component_register(
    SRCS "bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_core bt
    PRIV_REQUIRES esp_timer heap nvs_flash lwip esp_netif
)
