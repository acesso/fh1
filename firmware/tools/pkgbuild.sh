#!/bin/sh
# Builds a packaged app from scratch and packs it: tools/pkgbuild.sh apps/packages/NAME
# elf_loader's project_elf compiles with make's "/fast" targets, which skip the dependency check: a changed header
# does not rebuild the .c files that include it, and the app ends up with two ideas of one struct (Táticas da Ilha
# crashed on the FH1 that way). So the objects are always deleted first. Needs ESP-IDF's environment (export.sh).
set -e
dir=$(cd "$1" && pwd)
tools=$(cd "$(dirname "$0")" && pwd)
cd "$dir"
# fh1art.py needs Pillow: the system Python has it, the ESP-IDF one may not.
# FH1_PYTHON: another Python with Pillow (CI, when the system's packages cannot be installed).
if [ -f art.json ]; then "${FH1_PYTHON:-/usr/bin/python3}" "$tools/fh1art.py" build art.json; fi
[ -f build/esp-idf/main/CMakeFiles/__idf_main.dir/build.make ] || idf.py reconfigure
find build/esp-idf/main -name "*.obj" -delete; rm -f build/esp-idf/main/libmain.a
idf.py elf
python3 "$tools/elfcheck.py" build/*.app.elf
rm -f ./*.fpk
python3 "$tools/fpk.py" pack .
