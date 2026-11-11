# The board is chosen at build time (Kconfig "FH1 board"): the dev board by default, the FH1 with
# sdkconfig.defaults.fh1 (see firmware/README.md, "Build and flash").
if(CONFIG_FH1_BOARD_FH1)
    set(board_src "fh1.c")
else()
    set(board_src "guition_4848s040.c")
endif()
idf_component_register(
    SRCS ${board_src} "pcm5102a.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_core fh1_storage esp_lcd esp_timer esp_hw_support esp_driver_gpio esp_driver_ledc esp_driver_i2c
             esp_driver_i2s esp_driver_spi esp_adc
)
