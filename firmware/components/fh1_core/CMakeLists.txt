idf_component_register(
    SRCS "devices.c" "settings.c" "input.c" "audio.c" "i18n.c" "events.c" "mem.c" "text.c" "app.c" "gamepad.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event nvs_flash esp_timer esp_hw_support heap esp_system
)
