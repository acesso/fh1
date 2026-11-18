# Doom on the FH1: PrBoom (GPL-2.0-or-later, main/prboom, from retro-go's prboom-go) with the FH1's platform layer
# (main/fh1_doom.c) and WADs from a file or over the network (main/wadsrc.c). Built into a relocatable ELF and packed
# with tools/fpk.py, like every package.
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS ../../../components/fh1_sdk)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doom)
include(elf_loader)
project_elf(doom)
