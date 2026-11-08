# Duelo: a two-player sword duel for the FH1, a packaged app (SDK 0.2: gamepads with motion, the canvas).
# Built into build/duel.app.elf, then packed with tools/fpk.py (firmware/README.md, "Writing an app").
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS ../../../components/fh1_sdk)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(duel)
include(elf_loader)
project_elf(duel)
