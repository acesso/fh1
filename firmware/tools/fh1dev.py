#!/usr/bin/env python3
"""Drive the device from the PC through its dev console (keys, screenshots, stats, console ops).

  fh1dev.py keys A DOWN MENU ...       tap keys (names: up down left right a b x y start select menu)
  fh1dev.py shot out.png [KEY ...]     tap keys, then save a screenshot (PNG needs Pillow; else .bmp)
  fh1dev.py stats [LINES]              uptime, memory, display health, the last log lines
  fh1dev.py watch SECONDS [KEY ...]    hold keys down and print stats every 2 s (games: watch for 'late'/'odd')
  fh1dev.py op OP [PATH] [ID]          a console app op, e.g. op delete_rom /sd/roms/x.nes, op rescan

Environment: FH1_HOST (default fh1.local; use the IP if another device answers that name), FH1_CODE (the
6-digit console code from the boot log or the Settings window). Needs: pip install websockets [pillow].
"""
import asyncio
import json
import os
import re
import sys
import time
import urllib.request

import websockets

HOST = os.environ.get("FH1_HOST", "fh1.local")
CODE = os.environ.get("FH1_CODE", "")


async def connect():
    ws = await websockets.connect(f"ws://{HOST}/ws", open_timeout=10)
    await ws.send(json.dumps({"t": "auth", "token": CODE}))
    await asyncio.sleep(1.0)            # the device answers the login slowly while it is busy
    return ws


async def tap(ws, keys):
    for k in keys:
        await ws.send(json.dumps({"t": "key", "k": k.lower(), "p": True}))
        await asyncio.sleep(0.06)
        await ws.send(json.dumps({"t": "key", "k": k.lower(), "p": False}))
        await asyncio.sleep(0.4)


def print_stats(m, lines=0):
    h, d = m["heap"], m.get("disp", {})
    print(f"fw {m.get('fw')} slot {m.get('slot')} up {m.get('up')} s | internal free {h['int_free'] // 1024} KB "
          f"min {h['int_min'] // 1024} KB big {h['int_big'] // 1024} KB | disp late {d.get('late')} odd {d.get('odd')} "
          f"late_starts {d.get('late_starts')}")
    log = re.sub(r"\x1b\[[0-9;]*m", "", m.get("log", "")).splitlines()
    for l in log[-lines:] if lines else []:
        print("   ", l)


async def main(argv):
    cmd = argv[0] if argv else "stats"
    ws = await connect()
    try:
        if cmd == "keys":
            await tap(ws, argv[1:])
        elif cmd == "shot":
            await tap(ws, argv[2:])
            await asyncio.sleep(0.6)
            data = urllib.request.urlopen(f"http://{HOST}/shot.bmp?code={CODE}", timeout=60).read()
            out = argv[1]
            try:
                from PIL import Image
                import io
                Image.open(io.BytesIO(data)).convert("RGB").save(out)
            except ImportError:
                out = os.path.splitext(out)[0] + ".bmp"
                open(out, "wb").write(data)
            print(out)
        elif cmd in ("stats", "watch"):
            secs = float(argv[1]) if cmd == "watch" and len(argv) > 1 else 0
            keys = argv[2:] if cmd == "watch" else []
            for k in keys:
                await ws.send(json.dumps({"t": "key", "k": k.lower(), "p": True}))
            end = time.time() + secs
            while True:
                m = json.loads(await asyncio.wait_for(ws.recv(), 10))
                if m["t"] != "stats":
                    continue
                print_stats(m, int(argv[1]) if cmd == "stats" and len(argv) > 1 else (40 if cmd == "watch" else 0))
                if time.time() >= end:
                    break
            for k in keys:
                await ws.send(json.dumps({"t": "key", "k": k.lower(), "p": False}))
        elif cmd == "op":
            msg = {"t": "app", "op": argv[1]}
            if len(argv) > 2:
                msg["path"] = argv[2]
            if len(argv) > 3:
                msg["id"] = argv[3]
            await ws.send(json.dumps(msg))
            while True:
                m = json.loads(await asyncio.wait_for(ws.recv(), 10))
                if m["t"] == "reply":
                    print(m["msg"])
                    break
        else:
            print(__doc__)
    finally:
        await ws.close()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1:]))
