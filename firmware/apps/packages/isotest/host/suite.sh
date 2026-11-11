#!/bin/sh
# Táticas da Ilha's end-to-end tests, by group, on the PC (or on an FH1 with --device): the engine's showcase on the
# test field, and its stress test.
#
#   host/suite.sh [--device] [GROUP...]      from the package's folder; no group: quick; "all": every group
#
# Groups: views water scenery float effects spells overlays boss colors stress (host/tests/GROUP.script), and
# regress (the older host scripts). Sets: quick = views scenery float effects; engine = views water scenery float
# effects stress; game = spells overlays boss colors regress. Each group writes its screenshots and a contact sheet
# (sheet.png) to host/out/suite/GROUP/ and fails on a write outside the frame (guard), a command the game did not
# take, a layer that differs from a full build ("check: layer differs"), a layer check that never reported, or a
# crash.
# --device runs the group scripts on the FH1 (FH1_CODE, FH1_HOST; host/device.py), saving off, with the frame times
# and the bench lines from its log.
cd "$(dirname "$0")/.." || exit 1
device=0
[ "$1" = "--device" ] && device=1 && shift
groups="$*"
[ -z "$groups" ] && groups=quick
expand() {
    for g in "$@"; do
        case $g in
        all) echo views water scenery float effects spells overlays boss colors stress regress ;;
        quick) echo views scenery float effects ;;
        engine) echo views water scenery float effects stress ;;
        game) echo spells overlays boss colors regress ;;
        *) echo "$g" ;;
        esac
    done
}
groups=$(expand $groups)
[ $device = 0 ] && { make -C host -s >/dev/null || exit 1; }
failed=0
for g in $groups; do
    out=host/out/suite/$g
    rm -rf "$out"
    mkdir -p "$out"
    if [ "$g" = regress ]; then
        scripts="host/realms.script host/effects.script host/actions.script host/elemental.script host/flow.script host/target.script host/edge.script"
        [ $device = 1 ] && { echo "regress: PC only"; continue; }
    else
        scripts=host/tests/$g.script
        [ -f "$scripts" ] || { echo "$g: no such group"; failed=1; continue; }
    fi
    t0=$(date +%s)
    : > "$out/log.txt"
    for s in $scripts; do
        if [ $device = 1 ]; then
            { printf 'op stop\nsleep 1.5\nop start "" org.fh1.isotest\nsleep 2.5\ninfo\n'; cat "$s"; printf 'sleep 3\nop stop\n'; } > "$out/device.script"
            timeout 600 /usr/bin/python3 host/device.py "$out/device.script" "$out" >> "$out/log.txt" 2>&1
        else
            timeout 300 host/host "$s" "$out" >> "$out/log.txt" 2>&1
        fi
        echo "exit $?" >> "$out/log.txt"
    done
    bad=$(grep -E 'guard|outside the frame|^\? |not understood|layer differs|Traceback|did not start|exit [1-9]' "$out/log.txt" | head -3)
    checks=$(grep -c 'check: layer ok' "$out/log.txt")
    asked=0
    for s in $scripts; do asked=$((asked + $(grep -c '^checklayer' "$s"))); done
    [ "$checks" -lt "$asked" ] && [ -z "$bad" ] && bad="$((asked - checks)) of $asked layer checks never reported"
    bench=$(grep -o 'bench: .*' "$out/log.txt" | head -2 | tr '\n' ' ')
    /usr/bin/python3 - "$out" <<'PY' 2>/dev/null
import glob, os, sys
from PIL import Image, ImageDraw
d = sys.argv[1]
fs = sorted(f for f in glob.glob(d + "/*.bmp") + glob.glob(d + "/*.png") if not f.endswith("sheet.png"))
if fs:
    ims = [Image.open(f).convert("RGB") for f in fs]
    w, h = ims[0].size
    cols = 4
    s = Image.new("RGB", (cols * (w + 4), ((len(ims) + cols - 1) // cols) * (h + 14)))
    dr = ImageDraw.Draw(s)
    for i, (f, im) in enumerate(zip(fs, ims)):
        x, y = (i % cols) * (w + 4), (i // cols) * (h + 14)
        s.paste(im, (x, y + 12))
        dr.text((x + 2, y), os.path.basename(f), fill=(255, 220, 0))
    s.save(d + "/sheet.png")
PY
    if [ -n "$bad" ]; then
        echo "FAIL $g ($(( $(date +%s) - t0 )) s): $bad"
        failed=1
    else
        echo "ok   $g ($(( $(date +%s) - t0 )) s, $checks layer checks) $bench"
    fi
done
exit $failed
