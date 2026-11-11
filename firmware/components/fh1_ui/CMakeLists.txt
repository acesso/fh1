idf_component_register(
    SRCS "ui.c" "ui_sound.c" "ui_look.c" "fh1_font8.c" "fh1_sym8.c" "lv_mem_psram.c" "post.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_core fh1_board esp_timer
)
# LVGL calls the allocator above (CONFIG_LV_USE_CUSTOM_MALLOC): let the linker find it from LVGL's library.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC ${COMPONENT_LIB})
