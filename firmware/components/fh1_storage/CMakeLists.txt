idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_core esp_timer fatfs sdmmc esp_driver_sdspi esp_driver_spi vfs
)
