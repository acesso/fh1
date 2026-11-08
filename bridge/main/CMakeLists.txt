# fh1_bridge_proto.h, the protocol, lives with the FH1's side of it (firmware/components/fh1_bridge).
# The board's own hardware: board_a1s.c on the ESP32-A1S Audio Kit, board_none.c elsewhere.
if(BRIDGE_BOARD STREQUAL "A1S")
    set(board_src "board_a1s.c" "audio_rx.c")
else()
    set(board_src "board_none.c" "audio_rx.c")
endif()
idf_component_register(SRCS "main.c" "bridge.c" "wifi.c" "udp.c" "ota.c" ${board_src}
                       INCLUDE_DIRS "." "../../firmware/components/fh1_bridge/include"
                       REQUIRES "bluepad32" "btstack" "esp_timer" "app_update" "bootloader_support" "spi_flash" "nvs_flash" "esp_wifi" "esp_netif" "esp_event" "lwip" "esp_driver_i2c" "esp_driver_gpio" "esp_driver_i2s" "esp_driver_sdmmc" "sdmmc" "esp_http_server" "espressif__mdns")
if(BRIDGE_BOARD STREQUAL "A1S")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BRIDGE_BOARD_NAME="ESP32-A1S")
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BRIDGE_BOARD_NAME="ESP32-WROOM-32")
endif()
