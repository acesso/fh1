# A packaged FH1 app: an ESP-IDF project built into a relocatable ELF (build/isotest.app.elf),
# then packed with tools/fpk.py. It links against the SDK header only.
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS ../../../components/fh1_sdk)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(isotest)
include(elf_loader)
project_elf(isotest)
