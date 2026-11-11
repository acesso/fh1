#!/bin/sh
# Builds the packages that ship with the firmware and copies the .fpk files to OUT_DIR:
#   tools/build-packages.sh OUT_DIR [PACKAGE...]
# PACKAGE names a folder in apps/packages (default: isotest duel). Every asset pool in assets/pools with a
# sound recipe (sfx.json) is built and packed too. Needs ESP-IDF's environment (export.sh), and Pillow and
# numpy for /usr/bin/python3 (tools/fh1art.py, tools/fh1sfx.py), or for the Python FH1_PYTHON names.
set -e
out=$(mkdir -p "$1" && cd "$1" && pwd); shift
fw=$(cd "$(dirname "$0")/.." && pwd); repo=$(dirname "$fw")
[ $# -gt 0 ] || set -- isotest duel
# tools/elfcheck.py checks each app against the firmware's own elf_loader (firmware/managed_components): fetch the
# firmware's managed components first when this checkout has not built the firmware.
if [ ! -d "$fw/managed_components/espressif__elf_loader" ]; then
  idf.py -C "$fw" -B "$fw/build-deps" -D SDKCONFIG="$fw/build-deps/sdkconfig" reconfigure >/dev/null
fi
for p in "$@"; do
  "$fw/tools/pkgbuild.sh" "$fw/apps/packages/$p"
  cp "$fw/apps/packages/$p"/*.fpk "$out/"
done
for recipe in "$repo"/assets/pools/*/sfx.json; do
  pool=$(dirname "$recipe")
  (cd "$pool" && "${FH1_PYTHON:-/usr/bin/python3}" "$fw/tools/fh1sfx.py" build sfx.json && rm -f ./*.fpk && python "$fw/tools/fpk.py" pack .)
  cp "$pool"/*.fpk "$out/"
done
ls -l "$out"
