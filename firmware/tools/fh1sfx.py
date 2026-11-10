#!/usr/bin/env python3
"""Sound effects for the FH1: WAV files into a sound bank (.fsfx), by rules chosen from folder and file names.

  fh1sfx.py build <recipe.json> [-o out.fsfx]     the bank, and a preview WAV of every sound
  fh1sfx.py show <file.fsfx>                       its sounds

Recipe (paths relative to it):
  {
    "out": "assets/rpg.fsfx",
    "previews": "build/sfx-preview",
    "sources": ["../../packs/rpg-essentials-sfx"],       folders searched for *.wav
    "rate": 44100,                                         every sound at this rate, 16-bit mono
    "trim_db": -48, "fade_ms": 15, "peak_db": -3,
    "gain": {"12_Player_Movement_SFX": -9, "10_UI_Menu_SFX": -6, "8_Buffs_Heals_SFX": -3}
  }

Every sound: both channels mixed to one; the silence before and after cut (below trim_db); a fade at the end;
the peak set to peak_db, then the gain of the first rule whose text is in its path. Its name is the file's,
lower case, without the number in front: "22_Slash_04.wav" is "slash_04".

The bank (little-endian): "FSFX" u16 version(1) u16 sounds; per sound: name[32] u32 rate u32 frames u32 offset
u32 0; then the samples (16-bit mono), each sound's at its offset. A game reads the table, then only the sounds
it needs.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("fh1sfx.py needs numpy (run it with /usr/bin/python3)")


def load(path):
    w = wave.open(path)
    ch, sr, sw, n = w.getnchannels(), w.getframerate(), w.getsampwidth(), w.getnframes()
    raw = w.readframes(n)
    if sw == 3:
        a = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = a[:, 0].astype(np.int32) | (a[:, 1].astype(np.int32) << 8) | (a[:, 2].astype(np.int32) << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    elif sw == 2:
        v = np.frombuffer(raw, dtype="<i2") / 32768.0
    elif sw == 1:
        v = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128) / 128.0
    else:
        v = np.frombuffer(raw, dtype="<i4") / float(1 << 31)
    return v.reshape(-1, ch).mean(axis=1), sr


def resample(x, sr, to):
    if sr == to:
        return x
    cut = 0.45 * min(1.0, to / sr)
    k = np.arange(63) - 31
    h = 2 * cut * np.sinc(2 * cut * k) * np.hanning(63)
    y = np.convolve(x, h / h.sum(), mode="same")
    t = np.arange(0, len(y) - 1, sr / to)
    i = t.astype(int)
    f = t - i
    return y[i] * (1 - f) + y[i + 1] * f


def name_of(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    return re.sub(r"^\d+_", "", stem).lower()[:31]


def build(recipe_path, out=None):
    root = os.path.dirname(os.path.abspath(recipe_path))
    r = json.load(open(recipe_path))
    rate = r.get("rate", 44100)
    files = []
    for s in r["sources"]:
        files += sorted(glob.glob(os.path.join(root, s, "**", "*.wav"), recursive=True))
    if not files:
        sys.exit("no WAV files in the sources")
    sounds, names = [], set()
    for f in files:
        x, sr = load(f)
        x = resample(x, sr, rate)
        thr = 10 ** (r.get("trim_db", -48) / 20)
        loud = np.nonzero(np.abs(x) > thr)[0]
        if not len(loud):
            print(f"{f}: silent, skipped")
            continue
        x = x[max(0, loud[0] - int(0.002 * rate)):loud[-1] + 1].copy()
        fade = min(len(x), int(r.get("fade_ms", 15) * rate / 1000))
        x[len(x) - fade:] *= np.linspace(1, 0, fade)
        gain = r.get("peak_db", -3)
        for key, g in r.get("gain", {}).items():
            if key in f:
                gain += g
                break
        x = x / np.abs(x).max() * 10 ** (gain / 20)
        pcm = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
        name = name_of(f)
        if name in names:
            sys.exit(f"two sounds named {name}")
        names.add(name)
        sounds.append((name, pcm, os.path.relpath(f, root)))
    head = struct.pack("<4sHH", b"FSFX", 1, len(sounds))
    table, data = bytearray(), bytearray()
    base = len(head) + len(sounds) * 48
    for name, pcm, _ in sounds:
        table += struct.pack("<32sIIII", name.encode(), rate, len(pcm), base + len(data), 0)
        data += pcm.tobytes()
    out = out or os.path.join(root, r.get("out", "assets/sounds.fsfx"))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "wb").write(head + table + data)
    secs = sum(len(p) for _, p, _ in sounds) / rate
    print(f"{out}: {len(sounds)} sounds, {secs:.1f} s, {len(head) + len(table) + len(data)} bytes")
    if r.get("previews"):
        d = os.path.join(root, r["previews"])
        os.makedirs(d, exist_ok=True)
        for name, pcm, _ in sounds:
            w = wave.open(os.path.join(d, name + ".wav"), "wb")
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm.tobytes())
            w.close()
        print(f"previews: {d}/")


def show(path):
    b = open(path, "rb").read()
    magic, ver, n = struct.unpack_from("<4sHH", b, 0)
    print(f"{path}: version {ver}, {n} sounds, {len(b)} bytes")
    for i in range(n):
        name, rate, frames, off, _ = struct.unpack_from("<32sIIII", b, 8 + 48 * i)
        print(f"  {name.rstrip(b'\0').decode():32s} {frames / rate:5.2f} s  {frames * 2:7d} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("build"); s.add_argument("recipe"); s.add_argument("-o", "--out")
    s = sub.add_parser("show"); s.add_argument("file")
    a = ap.parse_args()
    build(a.recipe, a.out) if a.cmd == "build" else show(a.file)


if __name__ == "__main__":
    main()
