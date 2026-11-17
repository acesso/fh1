#!/usr/bin/env python3
"""The screen's speed on an FH1 unit, over Wi-Fi through its dev console (any board).

  fh1bench.py [SCENARIO ...]        default: all; FH1_HOST and FH1_CODE as for fh1dev.py
  fh1bench.py --json [SCENARIO ...] one JSON line per scenario instead of the table

Scenarios:
  home      the home screen, still (10 s)
  menu      the Apps menu scrolling up and down (10 s)
  screen    the Teste de tela package's bench (apps/packages/screentest, installed first): motion (every pixel each
            frame), sprites (16 boxes), half (half the screen scrolling) and card (still), 10 s each
  construcao  Construção Legal walking in the player's own world (it saves the player's place on leaving)
  duelo     Duelo against the FH1, moving and striking
  taticas   Táticas da Ilha's title (music), then the heroes' showcase (effects, sounds); its saving off

For each: the frames a second the app reached and its drawing time (screen, from the app's own log), and from the
console's stats every 2 s: panel frames a second (sends of changed areas on a panel with its own memory, the fixed
refresh on one without), bands a second, the slowest band, late bands and odd frames (likely visible), the load of
each core, and the sound driver's health where it logs it (Audio%: the slowest 5 s's frames against the rate;
ADrop: samples dropped; AUnder: underruns, gaps you hear). The panel link itself is in docs/display-bench.md.
"""
import asyncio
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh1dev  # noqa: E402  (FH1_HOST, FH1_CODE, connect)

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# The sound driver's health line, every 5 s while sound plays (pcm5102a.c and the other audio drivers)
AUDIO = re.compile(r"(\d+) frames/s \(rate (\d+)\), (\d+) dropped, (\d+) underruns")


async def send(ws, msg):
    await ws.send(json.dumps(msg))


async def op(ws, name, path=None, id_=None):
    m = {"t": "app", "op": name}
    if path is not None:
        m["path"] = path
    if id_ is not None:
        m["id"] = id_
    await send(ws, m)


async def key(ws, k, down):
    await send(ws, {"t": "key", "k": k, "p": down})


async def tap(ws, k, hold=0.06, gap=0.15):
    await key(ws, k, True)
    await asyncio.sleep(hold)
    await key(ws, k, False)
    await asyncio.sleep(gap)


class Window:
    """Stats messages summed over a stretch of device time."""

    def __init__(self):
        self.first_up = None
        self.last_up = None
        self.frames = self.fills = self.late = self.odd = 0
        self.max_fill = 0
        self.cpu = []
        self.int_min = None
        self.audio = []                # (frames/s, rate, dropped, underruns) per 5-s audio window

    def add(self, m):
        if self.first_up is None:      # its numbers cover the time before: only its clock counts
            self.first_up = m["up"]
            return
        for line in ANSI.sub("", m.get("log", "")).splitlines():
            a = AUDIO.search(line)
            if a:
                self.audio.append(tuple(int(x) for x in a.groups()))
        d = m.get("disp", {})
        self.frames += d.get("frames", 0)
        self.fills += d.get("fills", 0)
        self.late += d.get("late", 0)
        self.odd += d.get("odd", 0)
        self.max_fill = max(self.max_fill, d.get("max_fill_us", 0))
        if "cpu" in m:
            self.cpu.append(m["cpu"])
        h = m.get("heap", {})
        if "int_min" in h:
            self.int_min = h["int_min"] if self.int_min is None else min(self.int_min, h["int_min"])
        self.last_up = m["up"]

    def result(self):
        span = (self.last_up or 0) - (self.first_up or 0)
        if span <= 0:
            return {}
        cpu0 = round(sum(c[0] for c in self.cpu) / len(self.cpu)) if self.cpu else None
        cpu1 = round(sum(c[1] for c in self.cpu) / len(self.cpu)) if self.cpu else None
        r = {"panel_fps": round(self.frames / span, 1), "bands_s": round(self.fills / span), "slowest_band_us":
             self.max_fill, "late": self.late, "odd": self.odd, "cpu0": cpu0, "cpu1": cpu1,
             "int_min_kb": (self.int_min or 0) // 1024, "span_s": span}
        if self.audio:                 # the slowest window's share of the rate, everything dropped or underrun
            r["audio_pct"] = min(round(100 * f / rate) for f, rate, _, _ in self.audio if rate) if any(x[1] for x in self.audio) else None
            r["audio_drop"] = sum(x[2] for x in self.audio)
            r["audio_under"] = sum(x[3] for x in self.audio)
        return r


async def drain(ws, secs=0.5):
    end = time.time() + secs
    while time.time() < end:
        try:
            await asyncio.wait_for(ws.recv(), 0.2)
        except asyncio.TimeoutError:
            pass


async def watch(ws, secs, driver=None):
    """A window of `secs` device seconds while `driver` plays keys."""
    task = asyncio.create_task(driver(ws)) if driver else None
    w = Window()
    try:
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 15))
            if m.get("t") != "stats":
                continue
            w.add(m)
            if w.first_up is not None and w.last_up is not None and w.last_up - w.first_up >= secs:
                break
    finally:
        if task:
            task.cancel()
    return w.result()


async def stop_app(ws):
    await op(ws, "stop")
    await asyncio.sleep(2)


async def home(ws):
    await stop_app(ws)
    await asyncio.sleep(3)
    await drain(ws)
    return [{"scenario": "home", **(await watch(ws, 10))}]


async def menu(ws):
    await stop_app(ws)
    await asyncio.sleep(2)

    async def scroll(ws):
        while True:
            for k in ["down"] * 4 + ["up"] * 4:
                await tap(ws, k)
    await drain(ws)
    return [{"scenario": "menu", **(await watch(ws, 10, scroll))}]


async def construcao(ws):
    await stop_app(ws)
    await op(ws, "start", "", "construcao-legal")
    await asyncio.sleep(12)                      # its art and world load
    await op(ws, "dev", "net host")
    await asyncio.sleep(6)
    await op(ws, "dev", "tp 12 70")              # open grass south of the main road: nothing to walk into
    await asyncio.sleep(2)

    async def walk(ws):
        while True:
            for k in ["right", "down", "left", "up"]:
                await key(ws, k, True)
                await asyncio.sleep(2.5)
                await key(ws, k, False)
                await asyncio.sleep(0.1)
    await drain(ws)
    r = await watch(ws, 12, walk)
    await stop_app(ws)
    return [{"scenario": "construcao", **r}]


async def screen(ws):
    """The screentest package's bench: one row per pattern, its window from the app's own log lines."""
    await stop_app(ws)
    await op(ws, "start", "", "screentest")
    await asyncio.sleep(4)
    await drain(ws)
    await op(ws, "dev", "bench")
    rows, w, start = [], Window(), time.time()
    while time.time() - start < 90:
        m = json.loads(await asyncio.wait_for(ws.recv(), 15))
        if m.get("t") != "stats":
            continue
        w.add(m)
        for line in ANSI.sub("", m.get("log", "")).splitlines():
            g = re.search(r"screentest: (\w+): ([\d.]+) fps, draw ([\d.]+) ms \(max (\d+)\), longest gap (\d+) ms, (\d+x\d+)",
                          line)
            if g:
                rows.append({"scenario": "screen:" + g[1], "app_fps": float(g[2]), "draw_ms": float(g[3]),
                             "draw_max_ms": int(g[4]), "gap_ms": int(g[5]), "size": g[6], **w.result()})
                w = Window()
                w.add(m)
            if "screentest: bench done" in line:
                await stop_app(ws)
                return rows
    await stop_app(ws)
    return rows + [{"scenario": "screen", "error": "no 'bench done' in 90 s (is screentest installed?)"}]


async def duelo(ws):
    """Duelo against the FH1 (no second controller): moving and striking both ways."""
    await stop_app(ws)
    await op(ws, "start", "", "duel")
    await asyncio.sleep(6)

    async def fight(ws):
        while True:
            await key(ws, "right", True)
            for k in ["a", "b", "a"]:
                await tap(ws, k, gap=0.3)
            await key(ws, "right", False)
            await key(ws, "left", True)
            for k in ["a", "x", "a"]:
                await tap(ws, k, gap=0.3)
            await key(ws, "left", False)
    await drain(ws)
    r = await watch(ws, 14, fight)
    await stop_app(ws)
    return [{"scenario": "duelo", **r}]


async def taticas(ws):
    """Táticas da Ilha: the title (its music), then the heroes' showcase (effects, sounds). Saving off."""
    await stop_app(ws)
    await op(ws, "start", "", "taticas-da-ilha")
    await asyncio.sleep(10)
    await op(ws, "dev", "nosave")
    await drain(ws)
    rows = [{"scenario": "taticas:title", **(await watch(ws, 12))}]
    await op(ws, "dev", "show")
    await asyncio.sleep(6)
    await drain(ws)
    rows.append({"scenario": "taticas:show", **(await watch(ws, 14))})
    await stop_app(ws)
    return rows


SCENARIOS = {"home": home, "menu": menu, "screen": screen, "construcao": construcao, "duelo": duelo, "taticas": taticas}
COLS = [("scenario", "Scenario", 18), ("app_fps", "App fps", 8), ("draw_ms", "Draw ms", 8), ("gap_ms", "Gap ms", 7),
        ("panel_fps", "Panel fps", 10), ("bands_s", "Bands/s", 8), ("slowest_band_us", "Band us", 8),
        ("late", "Late", 5), ("odd", "Odd", 4), ("cpu0", "CPU0%", 6), ("cpu1", "CPU1%", 6), ("int_min_kb", "IntMinKB", 9),
        ("audio_pct", "Audio%", 7), ("audio_drop", "ADrop", 6), ("audio_under", "AUnder", 7)]


async def main(argv):
    as_json = "--json" in argv
    names = [a for a in argv if not a.startswith("--")] or list(SCENARIOS)
    ws = await fh1dev.connect()
    rows = []
    try:
        for n in names:
            for r in await SCENARIOS[n](ws):
                rows.append(r)
                if as_json:
                    print(json.dumps(r), flush=True)
    finally:
        await ws.close()
    if not as_json:
        print(f"{fh1dev.HOST}")
        print(" ".join(h.ljust(w) for _, h, w in COLS))
        for r in rows:
            print(" ".join(str(r.get(k, "-") if r.get(k) is not None else "-").ljust(w) for k, _, w in COLS))
            if "error" in r:
                print("  ", r["error"])


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1:]))
