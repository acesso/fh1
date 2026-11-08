file(GLOB SMSPLUS_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/smsplus/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/smsplus/cpu/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/smsplus/sound/*.c")
idf_component_register(
    SRCS "sms.c" ${SMSPLUS_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "smsplus" "smsplus/cpu" "smsplus/sound"
    REQUIRES fh1_core fh1_board fh1_storage esp_timer heap esp_rom
)
# SMS Plus: speed first; its code trips a few warnings. Its allocations go through sms_alloc.h, so the
# emulated machine's hot data lands in internal RAM when there is room (fh1_mem.h).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error -Wno-array-bounds -Wno-format -Wno-unused-function
                       -Wno-unused-variable -Wno-implicit-fallthrough -Wno-format-truncation -Wno-unused-but-set-variable)
set_source_files_properties(${SMSPLUS_SRCS} PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/sms_alloc.h")
