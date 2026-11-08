idf_component_register(
    SRCS "apps.c" "apps_games.c" "apps_music.c" "music_deck.c" "apps_bridges.c" "apps_io.c" "apps_users.c" "apps_net.c" "pkg.c" "run.c" "stress.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_users fh1_avatar fh1_core fh1_ui fh1_board fh1_storage fh1_devconsole fh1_ota fh1_gb fh1_nes fh1_sms fh1_sdk fh1_player fh1_bridge esp_app_format esp_timer mbedtls spi_flash esp_psram esp_hw_support
)
# fh1_sdk.h: the firmware has its own declarations of keys, capabilities and audio.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FH1_FIRMWARE)
# Labels are fixed-size buffers: cutting long text short is intended.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
