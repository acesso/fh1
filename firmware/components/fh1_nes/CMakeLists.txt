file(GLOB NOFRENDO_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/nofrendo/nes/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/nofrendo/mappers/*.c")
list(APPEND NOFRENDO_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/nofrendo/nofrendo.c")
idf_component_register(
    SRCS "nes.c" ${NOFRENDO_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "nofrendo" "nofrendo/nes"
    REQUIRES fh1_core fh1_board fh1_storage esp_timer heap
)
# nofrendo: speed first; its code trips a few warnings. Its allocations go through nes_alloc.h, so the
# emulated machine's hot data lands in internal RAM when there is room (fh1_mem.h).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error -Wno-array-bounds -Wno-format -Wno-unused-function
                       -Wno-unused-variable -Wno-implicit-fallthrough -Wno-format-truncation)
set_source_files_properties(${NOFRENDO_SRCS} PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/nes_alloc.h")
