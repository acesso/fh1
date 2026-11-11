#!/bin/sh
# Builds the firmware for one board and collects what to flash:
#   tools/build-board.sh guition|fh1 OUT_DIR [psram120]
# OUT_DIR gets fh1-firmware-<board>.bin (one image, written at 0x0) and the parts in ESP-IDF's layout
# (bootloader/bootloader.bin, partition_table/partition-table.bin, ota_data_initial.bin, fh1.bin), which
# simulator/build-flash.sh takes as its BUILD_DIR. psram120: PSRAM and flash at 120 MHz (sdkconfig.defaults.psram120,
# experimental; the dev unit only), in a build folder of its own. Needs ESP-IDF's environment (export.sh).
set -e
board=$1; out=$(mkdir -p "$2" && cd "$2" && pwd)
case "$board" in
  guition) defaults="sdkconfig.defaults" ;;
  fh1) defaults="sdkconfig.defaults;sdkconfig.defaults.fh1" ;;
  *) echo "usage: $0 guition|fh1 OUT_DIR [psram120]" >&2; exit 2 ;;
esac
cd "$(dirname "$0")/.."
b="build-$board"
if [ "$3" = psram120 ]; then defaults="$defaults;sdkconfig.defaults.psram120"; b="$b-psram120"; fi
idf.py -B "$b" -D SDKCONFIG="$b/sdkconfig" -D SDKCONFIG_DEFAULTS="$defaults" build
mkdir -p "$out/bootloader" "$out/partition_table"
cp "$b/bootloader/bootloader.bin" "$out/bootloader/"
cp "$b/partition_table/partition-table.bin" "$out/partition_table/"
cp "$b/ota_data_initial.bin" "$b/fh1.bin" "$out/"
(cd "$b" && python -m esptool --chip esp32s3 merge-bin -o "$out/fh1-firmware-$board.bin" @flash_args)
ls -l "$out/fh1-firmware-$board.bin"
