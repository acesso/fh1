idf_component_register(
    SRCS "gb.c" "minigb_apu.c"
    INCLUDE_DIRS "include"
    REQUIRES fh1_core fh1_board fh1_storage esp_timer heap
)
# Peanut-GB: speed matters more than size here, and its code trips a few warnings.
target_compile_definitions(${COMPONENT_LIB} PRIVATE AUDIO_SAMPLE_RATE=32000)
# minigb_apu's functions under other names: gb.c answers Peanut-GB's audio_read/audio_write itself and feeds
# the APU from a task on core 0 (gb.c, "Sound").
set_source_files_properties(minigb_apu.c PROPERTIES COMPILE_DEFINITIONS
    "audio_read=apu_core_read;audio_write=apu_core_write;audio_callback=apu_core_callback;audio_init=apu_core_init;audio_registers=apu_core_registers")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-unused-variable -Wno-implicit-fallthrough -Wno-format-truncation -Wno-error)
