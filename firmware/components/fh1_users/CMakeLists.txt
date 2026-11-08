idf_component_register(
    SRCS "users.c" "users_social.c" "vendor/monocypher/monocypher.c" "vendor/monocypher/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor/monocypher"
    REQUIRES fh1_core fh1_storage
    PRIV_REQUIRES mbedtls esp_hw_support esp_timer heap
)
# Paths and labels are fixed-size buffers sized for their contents (ids of 16, names of 16).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
