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
mkdir -p "$out/wasm/fw"
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
# The 3D view of the V1 build, with its own copy of three.js r160 (no CDN at view time; pinned by SHA-256) and
# the static renders it falls back to without WebGL.
three="$out/vendor/three"; mkdir -p "$three"
while read -r sha path; do
  f="$three/$(basename "$path")"
  curl -fsSL --retry 3 "https://cdn.jsdelivr.net/npm/three@0.160.0/$path" -o "$f"
  echo "$sha  $f" | sha256sum -c --quiet || { echo "three.js: $path: wrong SHA-256" >&2; exit 1; }
done <<'PINS'
76dea8151bc9352aef3528b4262e249b2604f62543828328db978d060d61a495 build/three.module.js
e21f41b7ef2016f2984a5d27850b42dffa797a6a194a138e288b06576d765a7b examples/jsm/environments/RoomEnvironment.js
d20aeaf7077c459a8d952d215738d7b44fba028440bb862d84b622a34f0c80bb examples/jsm/geometries/RoundedBoxGeometry.js
5a44a9e86a2a0fb11933eed69bc2cd33c76a496854c1aed6ed776efa87d7b064 examples/jsm/controls/OrbitControls.js
PINS
sed -i "s|from 'three';|from './three.module.js';|" "$three/RoomEnvironment.js" "$three/RoundedBoxGeometry.js" "$three/OrbitControls.js"
sed 's|<meta charset="utf-8">|<meta charset="utf-8">\n<meta name="three" content="./vendor/three">|' \
  "$repo/hardware/layout/concept-render.html" > "$out/hardware-3d.html"
cp "$repo"/hardware/layout/concept-v1-*.png "$out/"
printf '{ "firmware": "%s", "commit": "%s", "date": "%s" }\n' \
  "$(git -C "$repo" describe --tags --always)" "$(git -C "$repo" rev-parse HEAD)" "$(date -u +%Y-%m-%d)" > "$out/build.json"
touch "$out/.nojekyll"
du -sh "$out"
