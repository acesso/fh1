#!/bin/sh
# Builds the 16 MB flash image the simulator boots: the firmware (bootloader, partition table, OTA data,
# app) plus the "apps" LittleFS partition at 0x820000 with packages already installed, laid out as the
# firmware installs them (/int/apps/<id>/, /int/assets/<id>/).
#   simulator/build-flash.sh BUILD_DIR OUT.bin PACKAGE.fpk...
# BUILD_DIR is an ESP-IDF build of the firmware (firmware/build-fh1 for the FH1 board). Needs esptool and
# littlefs-python (pip install esptool littlefs-python==0.15.0); PYTHON picks the interpreter.
set -e
[ $# -ge 2 ] || { echo "usage: $0 BUILD_DIR OUT.bin PACKAGE.fpk..." >&2; exit 2; }
build=$(cd "$1" && pwd); out=$2; shift 2
PY=${PYTHON:-python3}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/int/apps" "$tmp/int/assets"
for f in "$@"; do
  "$PY" - "$f" "$tmp/int" <<'EOF'
import json, sys, tarfile, os
fpk, root = sys.argv[1], sys.argv[2]
with tarfile.open(fpk) as t:
    m = json.load(t.extractfile("manifest.json"))
    dest = os.path.join(root, "assets" if m.get("kind") == "assets" else "apps", m["id"])
    t.extractall(dest, filter="data")
print(f"  {m['id']} {m.get('version', '')} -> {os.path.relpath(dest, root)}")
EOF
done
# The partition table's "apps" partition: 0x820000, 0x7d0000 bytes (firmware/partitions.csv).
"$PY" -m littlefs create "$tmp/int" "$tmp/apps.bin" --fs-size=0x7d0000 --name-max=64 --block-size=4096
"$PY" -m esptool --chip esp32s3 merge-bin -o "$out" --flash-mode dio --flash-freq 80m --flash-size 16MB \
  --pad-to-size 16MB \
  0x0 "$build/bootloader/bootloader.bin" 0x8000 "$build/partition_table/partition-table.bin" \
  0xf000 "$build/ota_data_initial.bin" 0x20000 "$build/fh1.bin" 0x820000 "$tmp/apps.bin"
ls -l "$out"
