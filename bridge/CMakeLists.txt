# The FH1 extension board (Bluetooth bridge) on an ESP32-WROOM-32: Bluepad32 for the controllers.
# Bluepad32 is not in this repository (yet): a checkout of its release, with its BTstack patch applied, is
# expected at BLUEPAD32_ROOT (default: ../../FH1-ext/bluepad32, next to the FH1 repository). See README.md.
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED BLUEPAD32_ROOT)
    set(BLUEPAD32_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../FH1-ext/bluepad32)
endif()
get_filename_component(BLUEPAD32_ROOT ${BLUEPAD32_ROOT} ABSOLUTE)
if(NOT EXISTS ${BLUEPAD32_ROOT}/src/components/bluepad32)
    message(FATAL_ERROR "Bluepad32 not found at ${BLUEPAD32_ROOT}: see README.md")
endif()
set(ENV{BTSTACK_ROOT} ${BLUEPAD32_ROOT}/external/btstack)
set(EXTRA_COMPONENT_DIRS ${BLUEPAD32_ROOT}/src/components)
# mDNS (the bridge's name on the network): the copy the FH1's firmware already has from the component
# registry (firmware/managed_components, espressif/mdns 1.15.0; build the FH1 once first).
set(FH1_MDNS ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/managed_components/espressif__mdns)
if(EXISTS ${FH1_MDNS})
    list(APPEND EXTRA_COMPONENT_DIRS ${FH1_MDNS})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{BTSTACK_ROOT}/port/esp32/components/btstack/cmake/BTstack_project.cmake)

# The board, for its name in the bridge's info: WROOM32 (the spare ESP32-WROOM-32) or A1S (an ESP32-A1S
# Audio Kit). Build each in its own directory: idf.py -B build-a1s -DSDKCONFIG=build-a1s/sdkconfig -DBRIDGE_BOARD=A1S build
set(BRIDGE_BOARD "WROOM32" CACHE STRING "Bridge board: WROOM32 or A1S")

# espressif/mdns trips -Werror=stringop-truncation with this compiler (the FH1's build does not make it an error).
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)

project(fh1_bridge)
