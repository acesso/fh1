# The audio player engine and its decoders (vendored single-file libraries and Tremor; see VENDORED.md).
file(GLOB TREMOR_SRCS "${CMAKE_CURRENT_LIST_DIR}/vendor/tremor/*.c")
idf_component_register(
    SRCS "src/player.c" "src/dec_mp3.c" "src/dec_flac.c" "src/dec_wav.c" "src/dec_ogg.c" "src/dec_mod.c" ${TREMOR_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "vendor/minimp3" "vendor/dr_libs" "vendor/pocketmod" "vendor/tremor"
    REQUIRES fh1_core
    PRIV_REQUIRES heap esp_hw_support
)
# The decoders allocate in PSRAM first (shim/pl_alloc.h; our own sources call pl_* directly).
set_source_files_properties(${TREMOR_SRCS} PROPERTIES COMPILE_OPTIONS
    "-include;${CMAKE_CURRENT_LIST_DIR}/shim/pl_alloc.h;-Wno-error;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-misleading-indentation;-Wno-implicit-fallthrough;-Wno-char-subscripts;-Wno-shift-negative-value;-Wno-maybe-uninitialized;-Wno-type-limits;-Wno-unused-label")
# Speed over size: decoding runs all the time a track plays. The vendored headers trip a few warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-implicit-fallthrough)
