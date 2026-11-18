file(GLOB PRBOOM_SRCS "prboom/*.c")
idf_component_register(SRCS "fh1_doom.c" "wadsrc.c" "doom_mem.c" "fh1_arena.c" ${PRBOOM_SRCS}
                       INCLUDE_DIRS "." "prboom"
                       REQUIRES fh1_sdk heap)
# PrBoom's own warnings are many and harmless; it is built as its authors do.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -O2 -DHAVE_CONFIG_H -DFH1_DOOM
    -include ${CMAKE_CURRENT_SOURCE_DIR}/doom_mem.h
    -Wno-error -Wno-address -Wno-misleading-indentation -Wno-format-overflow -Wno-char-subscripts
    -Wno-missing-field-initializers -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
    -Wno-format-truncation -Wno-implicit-fallthrough -Wno-pointer-sign -Wno-parentheses -Wno-dangling-else
    -Wno-maybe-uninitialized -Wno-unused-const-variable)
