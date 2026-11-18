#!/usr/bin/env python3
"""Doom's timed demos on an FH1 unit, watched: stops as soon as the game stalls, fails or the unit restarts.

  fh1doombench.py [--wad URL] [--install doom_X.fpk] [--stall S] [DEMO:TICS ...]   default: demo1:2000
  FH1_HOST and FH1_CODE as for fh1dev.py

For each demo: Doom's "timedemo DEMO TICS" (its first TICS drawn as fast as they go), reported as frames a second
with the game's own split of a frame (the fps dev command) and its network reads. --wad gives the WAD when the
unit has none (http://<this computer>:8077/freedoom1.wad, from tools/fh1wadserve.py).

Watched all along, on one console connection:
- progress: a frame-rate line comes every 5 s while Doom draws; none for --stall seconds (default 30) is a stall,
  and three in a row under --min-fps (default 1) are as bad;
- Doom's own errors ("doom: error: ...");
- the unit restarting (its uptime goes back, or the console drops), and Doom leaving;
- the lowest free internal RAM and PSRAM, reported at the end.
The whole log goes to doombench.log. On a failure: the last log lines, a screenshot (doombench-shot.bmp) and the unit's core dump if it made one
(doombench.core: esp-coredump info_corefile -t raw -c doombench.core build-<board>/fh1.elf). Exit status 1.
"""
import argparse
import asyncio
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

import websockets

HOST = os.environ.get("FH1_HOST", "fh1.local")
CODE = os.environ.get("FH1_CODE", "")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


class Unit:
    def __init__(self, ws):
        self.ws = ws
        self.lines = []          # every log line seen, in order
        self.seen = set()
        self.up = None
        self.restarted = False
        self.dropped = False
        self.last_stats = time.time()
        self.int_low = self.ps_low = None   # the lowest free internal RAM and PSRAM seen

    async def reader(self):
        try:
            async for raw in self.ws:
                m = json.loads(raw)
                if m.get("t") != "stats":
                    continue
                self.last_stats = time.time()
                heap = m.get("heap", {})
                for key, attr in (("int_free", "int_low"), ("ps_free", "ps_low")):
                    v = heap.get(key)
                    if isinstance(v, int) and (getattr(self, attr) is None or v < getattr(self, attr)):
                        setattr(self, attr, v)
                up = m.get("up")
                if isinstance(up, int):
                    if self.up is not None and up + 5 < self.up:
                        self.restarted = True
                    self.up = up
                for line in ANSI.sub("", m.get("log", "")).splitlines():
                    if line and line not in self.seen:
                        self.seen.add(line)
                        self.lines.append(line)
        except websockets.ConnectionClosed:
            self.dropped = True

    async def op(self, op, path="", id_=None):
        msg = {"t": "app", "op": op, "path": path}
        if id_ is not None:
            msg["id"] = id_
        await self.ws.send(json.dumps(msg))

    def doom(self, start):
        return [l for l in self.lines[start:] if "[doom]" in l]


def http(path, data=None, timeout=60):
    req = urllib.request.Request(f"http://{HOST}{path}", data=data, method="POST" if data is not None else "GET",
                                 headers={"X-FH1-Code": CODE})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def evidence(unit, why):
    print(f"FAILED: {why}")
    for l in unit.lines:
        if "run: starting doom" in l:     # where its code is: crash addresses map from it (docs/sdk.md)
            print("  " + l)
    print("last log lines:")
    for l in unit.lines[-25:]:
        print("  " + l[:220])
    try:
        open("doombench-shot.bmp", "wb").write(http(f"/shot.bmp?code={CODE}", timeout=20))
        print("screenshot: doombench-shot.bmp")
    except Exception as e:
        print(f"screenshot: {e}")
    try:
        open("doombench.core", "wb").write(http(f"/coredump?code={CODE}", timeout=30))
        print("core dump: doombench.core")
    except Exception:
        print("core dump: none")


MIN_FPS = 1.0


async def watch(unit, start, done, stall, total, what):
    """Until a line matches done (returns it); fails on a stall, an error, a restart, Doom leaving, or three frame-rate
    lines in a row under MIN_FPS (thrashing: as good as stuck)."""
    t0 = last = time.time()
    progress_seen = len(unit.lines)
    slow_checked = start
    while True:
        await asyncio.sleep(1)
        new = unit.doom(start)
        for l in new:
            if done.search(l):
                return l
        for l in new:
            if "doom: error:" in l:
                raise RuntimeError(f"{what}: Doom stopped: {l.split('doom: error:', 1)[1].strip()}")
        # Leaving: after this run's start only (the last run's own lines come with a later stats push)
        began = max((i for i, l in enumerate(unit.lines) if "run: starting doom" in l), default=len(unit.lines))
        if any("doom unloaded" in l or "doom: closed" in l for l in unit.lines[began:]):
            raise RuntimeError(f"{what}: Doom left")
        if unit.restarted or unit.dropped:
            raise RuntimeError(f"{what}: the unit restarted (or its console dropped)")
        if time.time() - unit.last_stats > 15:
            raise RuntimeError(f"{what}: no stats from the unit for 15 s (hung?)")
        if len(unit.doom(progress_seen)) > 0:
            progress_seen = len(unit.lines)
            last = time.time()
        rates = [float(m.group(1)) for l in unit.doom(slow_checked)
                 for m in [re.search(r"doom: ([\d.]+) fps,", l)] if m]
        if len(rates) >= 3 and all(r < MIN_FPS for r in rates[-3:]):
            raise RuntimeError(f"{what}: under {MIN_FPS} fps for 15 s ({', '.join(map(str, rates[-3:]))})")
        if time.time() - last > stall:
            raise RuntimeError(f"{what}: no progress for {stall} s (stuck)")
        if time.time() - t0 > total:
            raise RuntimeError(f"{what}: not done in {total} s")


async def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("demos", nargs="*", default=["demo1:2000"])
    a.add_argument("--wad")
    a.add_argument("--install")
    a.add_argument("--stall", type=int, default=30)
    a.add_argument("--min-fps", type=float, default=1.0)
    args = a.parse_args()
    global MIN_FPS
    MIN_FPS = args.min_fps
    ws = await websockets.connect(f"ws://{HOST}/ws", open_timeout=60, ping_interval=None, max_size=None)
    await ws.send(json.dumps({"t": "auth", "token": CODE}))
    unit = Unit(ws)
    reader = asyncio.create_task(unit.reader())
    ok = True
    try:
        await unit.op("stop")
        await asyncio.sleep(3)
        if args.install:               # (after the stop: an app running refuses it)
            try:
                print(http("/fh1/install", open(args.install, "rb").read(), timeout=300).decode().strip())
            except urllib.error.HTTPError as e:
                raise RuntimeError(f"install: {e.code} {e.read().decode(errors='replace').strip()}")
        start = len(unit.lines)
        await unit.op("start", "", "doom")
        await asyncio.sleep(2)
        if args.wad:                   # (ignored when Doom already has one: wad.txt or the wads folder)
            await unit.op("dev", f"wad {args.wad}")
        # Without a WAD, Doom only says so on its screen: no log, so this stalls (see the screenshot)
        await watch(unit, start, re.compile(r"doom: ready"), args.stall, 120, "loading the WAD")
        await unit.op("dev", "fps")
        results = []
        for d in args.demos:
            demo, _, tics = d.partition(":")
            start = len(unit.lines)
            await unit.op("dev", f"timedemo {demo} {tics or 2000}")
            line = await watch(unit, start, re.compile(r"doom: timedemo: |Timed \d+ gametics"), args.stall, 900, demo)
            fps = re.search(r"= ([\d.]+) f", line)
            steps = [l for l in unit.doom(start) if "ms/10 a frame: step" in l]
            reads = [l for l in unit.doom(start) if "WAD reads" in l]
            results.append((demo, tics, fps.group(1) if fps else "?", steps[-1] if steps else "", reads[-1] if reads else ""))
            print(f"{demo} {tics} tics: {fps.group(1) if fps else line} fps")
        start = len(unit.lines)
        await unit.op("dev", "net")
        await unit.op("dev", "mem")
        await asyncio.sleep(4)
        print("\n| Demo | Tics | fps |\n|---|---|---|")
        for demo, tics, fps, _, _ in results:
            print(f"| {demo} | {tics} | {fps} |")
        for demo, _, _, step, reads in results:
            print(f"{demo}: " + step.split("ms/10 a frame: ", 1)[-1])
            print(f"{demo}: " + reads.split("ms/10 a frame: ", 1)[-1])
        for l in unit.doom(start):
            if "requests" in l or "zone" in l or "all " in l:
                print(l.split("doom: ", 1)[-1][:200])
        print(f"lowest free while it ran: internal RAM {unit.int_low // 1024 if unit.int_low else '?'} KB, "
              f"PSRAM {unit.ps_low // 1024 if unit.ps_low else '?'} KB")
    except RuntimeError as e:
        evidence(unit, str(e))
        ok = False
    finally:
        reader.cancel()
        await ws.close()
        with open("doombench.log", "w") as f:
            f.write("\n".join(unit.lines) + "\n")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    asyncio.run(main())
