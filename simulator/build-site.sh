#!/bin/sh
# Assembles the GitHub Pages site: the FH1 landing page (simulator/site/) plus esp32sim's emulator page
# booting the FH1 firmware.
#   simulator/build-site.sh ESP32SIM_DIR FLASH.bin OUT_DIR
# ESP32SIM_DIR is an esp32sim checkout at the commit in simulator/esp32sim.pin, with simulator/esp32sim.patch
# applied and its WebAssembly module built (tools/wasm-build.sh). FLASH.bin comes from build-flash.sh.
set -e
[ $# -eq 3 ] || { echo "usage: $0 ESP32SIM_DIR FLASH.bin OUT_DIR" >&2; exit 2; }
sim=$(cd "$1" && pwd); flash=$2; out=$3
here=$(cd "$(dirname "$0")" && pwd); repo=$(dirname "$here")
[ -f "$sim/web/wasm/esp32sim.wasm" ] || { echo "$sim/web/wasm/esp32sim.wasm: build it first (tools/wasm-build.sh)" >&2; exit 1; }

# xterm.js (the Terminal tab) and the mask ROM ELFs, both pinned by esp32sim's own scripts.
[ -f "$sim/web/vendor/xterm/xterm.js" ] || (cd "$sim" && tools/fetch-web-vendor.sh)
(cd "$sim" && tools/fetch-rom-elfs.sh web/wasm/fw)

rm -rf "$out"; mkdir -p "$out"
# The emulator page, without esp32sim's own landing page and demo firmware.
(cd "$sim/web" && tar cf - --exclude=./wasm/fw --exclude=./index.html .) | (cd "$out" && tar xf -)
mkdir -p "$out/wasm/fw" "$out/img"
cp "$sim/web/wasm/fw/esp32s3_rev0_rom.elf" "$out/wasm/fw/"
cp "$flash" "$out/wasm/fw/fh1.bin"
cat > "$out/wasm/fw/fh1.json" <<'EOF'
{ "board": "fh1", "flash_mb": 16, "psram_mb": 8,
  "files": { "rom": "esp32s3_rev0_rom.elf", "flash": "fh1.bin" } }
EOF
cat > "$out/wasm/fw/demos.json" <<'EOF'
[ { "fw": "fh1", "title": "FH1 handheld", "note": "the FH1 firmware built for the FH1 board, with Táticas da Ilha and Duelo installed" } ]
EOF
cp "$sim/LICENSE" "$out/esp32sim-LICENSE.txt"

# The landing page and its pictures.
cp -r "$here/site/." "$out/"
cp "$repo/hardware/layout/concept-v1-front.png" "$out/img/"
printf '{ "firmware": "%s", "commit": "%s", "date": "%s" }\n' \
  "$(git -C "$repo" describe --tags --always)" "$(git -C "$repo" rev-parse HEAD)" "$(date -u +%Y-%m-%d)" > "$out/build.json"
touch "$out/.nojekyll"
du -sh "$out"
