idf_component_register(
    SRCS "guition_4848s040.c" "pcm5102a.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_core fh1_storage esp_lcd esp_timer esp_hw_support esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_i2s
)
