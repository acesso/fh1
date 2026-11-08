cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# -O2 (CONFIG_COMPILER_OPTIMIZATION_PERF) finds possible strncpy truncations in the managed mDNS component;
# keep them as warnings instead of errors.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
project(fh1)
