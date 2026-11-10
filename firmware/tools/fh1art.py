#!/usr/bin/env python3
"""Turn external art (PNG sheets, separate PNGs, Aseprite files) into an FH1 art pack (.fart), when a game's
package is built. A game commits a recipe (JSON) naming its sources under the repository's assets/ folder.

  fh1art.py info <file.aseprite|file.png>          what a file holds: size, layers, tags, frames
  fh1art.py build <recipe.json> [-o out.fart]       build the pack (and previews next to it, *.png)
  fh1art.py show <file.fart>                        list a pack's contents

Recipe (paths are relative to the recipe):
  {
    "out": "assets/art.fart",
    "previews": "build/art-preview",
    "images": [                                     one image (or a grid of them) per entry
      {"name": "grass", "png": "../../../assets/packs/x/grass.png", "anchor": [16, 24]},
      {"name": "rock", "png": "...", "grid": [32, 32], "cells": [[0, 0], [1, 0]], "anchor": [16, 24]},
      {"name": "tile", "png": "sheet.png", "grid": [32, 32], "cells": "all", "anchor": [16, 16]}   tile.0, tile.1...
    ],
    "anims": [                                      animations: rows of a sheet or Aseprite tags
      {"name": "knight.walk", "png": "....png", "grid": [64, 64], "rows": {"se": 0, "ne": 2},
       "frames": 8, "ms": 100, "anchor": [32, 52], "scale": 1},
      {"name": "knight.walk", "aseprite": "....aseprite", "tags": {"se": "SouthEast"}, "layers": ["Body"],
       "anchor": [32, 52]},
      {"name": "wolf.idle", "aseprite": "....aseprite", "tags": {"se": {"tag": "idle", "layers": ["S-East"],
       "anchor": [32, 108]}}},                        facings kept in separate layers and places
      {"name": "slime.fire.idle", "files": {"sw": "...-Idle Down.png", "ne": "...-Idle Up.png"},
       "grid": [64, 64], "anchor": [32, 50], "pick": [0, 2], "recolor": {"hue": [285, 45], "to": 8}}
    ]
  }

Every image is trimmed to its visible pixels and keeps its anchor (the point that stands on the ground). A
"scale" below 1 shrinks pixel art: integer factors (1/2, 1/3) keep the most frequent colour of each block;
other factors average by area and snap each pixel to the source's nearest colour; whole numbers above 1 repeat
pixels. "files" gives a facing per PNG (a row of frames each; "frames" or as many as fit), "pick" keeps some
frames, "recolor" turns the hues within [centre, half width] (degrees) to "to", keeping light and saturation
("sat" and "light" multiply saturation and lightness): a red slime from a purple one, its golden crown kept.

The pack (little-endian), read by the engine:
  header  "FART" u16 version(2) u16 images u16 anims u16 0
  images  per image: name[24] i16 ox i16 oy (top-left from the anchor) u16 w u16 h u32 offset (spans)
          i16 hx i16 hy (the hand, from the anchor; -32768: none) u32 parts (offset of a part map, 0: none:
          2 bits a pixel, row by row over w x h: 0 none, 1 torso, 2 arms, 3 legs)
  anims   per anim: name[24] u8 dirs u8 frames u16 ms; then dirs x frames u16 image indexes
          (direction order: se, sw, nw, ne; 0xffff where missing: the engine mirrors se<->sw, ne<->nw)
  spans   per image row: u16 runs, then per run u16 skip, u16 len, len x u16 RGB565 (all 16-bit: the S3
          cannot read a u16 at an odd address)
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from collections import Counter

try:
    from PIL import Image
except ImportError:
    sys.exit("fh1art.py needs Pillow (run it with /usr/bin/python3 or install python3-pillow)")

DIRS = ["se", "sw", "nw", "ne"]
NONE = 0xFFFF


# ── Aseprite files (https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md) ──

class Ase:
    def __init__(self, path):
        data = open(path, "rb").read()
        (size, magic, nframes, self.w, self.h, self.depth) = struct.unpack_from("<IHHHHH", data, 0)
        if magic != 0xA5E0:
            raise ValueError(f"{path}: not an Aseprite file")
        self.transparent = data[28]
        self.layers, self.tags, self.durations = [], [], []
        self.cels = {}                                       # (frame, layer) -> (x, y, opacity, RGBA image)
        self.palette = [(0, 0, 0, 0)] * 256
        pos = 128
        for f in range(nframes):
            fbytes, fmagic, old, dur, _, new = struct.unpack_from("<IHHH2sI", data, pos)
            if fmagic != 0xF1FA:
                raise ValueError(f"{path}: bad frame {f}")
            self.durations.append(dur)
            n = new if new else old
            p = pos + 16
            for _ in range(n):
                csize, ctype = struct.unpack_from("<IH", data, p)
                self._chunk(ctype, data[p + 6:p + csize], f)
                p += csize
            pos += fbytes

    @staticmethod
    def _str(b, o):
        n = struct.unpack_from("<H", b, o)[0]
        return b[o + 2:o + 2 + n].decode("utf-8", "replace"), o + 2 + n

    def _chunk(self, t, b, frame):
        if t == 0x2004:                                      # layer
            flags, ltype, level = struct.unpack_from("<HHH", b, 0)
            opacity = b[12]
            name, _ = self._str(b, 16)
            self.layers.append({"name": name, "visible": bool(flags & 1), "type": ltype, "level": level,
                                "opacity": opacity})
        elif t == 0x2005:                                    # cel
            layer, x, y, opacity, ctype = struct.unpack_from("<HhhBH", b, 0)
            body = b[16:]
            if ctype == 1:                                   # linked to another frame's cel
                self.cels[(frame, layer)] = ("link", struct.unpack_from("<H", body, 0)[0])
                return
            if ctype not in (0, 2):
                return                                       # tilemaps: not used
            w, h = struct.unpack_from("<HH", body, 0)
            px = body[4:] if ctype == 0 else zlib.decompress(body[4:])
            self.cels[(frame, layer)] = (x, y, opacity, self._image(px, w, h))
        elif t == 0x2018:                                    # tags
            n = struct.unpack_from("<H", b, 0)[0]
            o = 10
            for _ in range(n):
                fr, to, direction = struct.unpack_from("<HHB", b, o)
                name, o = self._str(b, o + 17)
                self.tags.append({"name": name, "from": fr, "to": to, "dir": direction})
        elif t == 0x2019:                                    # palette
            size, first, last = struct.unpack_from("<III", b, 0)
            o = 20
            for i in range(first, last + 1):
                flags = struct.unpack_from("<H", b, o)[0]
                r, g, bl, a = b[o + 2:o + 6]
                self.palette[i] = (r, g, bl, a)
                o += 6
                if flags & 1:
                    _, o = self._str(b, o)

    def _image(self, px, w, h):
        if self.depth == 32:
            return Image.frombytes("RGBA", (w, h), bytes(px))
        if self.depth == 16:
            img = Image.new("RGBA", (w, h))
            img.putdata([(px[i], px[i], px[i], px[i + 1]) for i in range(0, len(px), 2)])
            return img
        img = Image.new("RGBA", (w, h))                      # indexed: resolved with the palette at the end
        img.info["indexed"] = bytes(px)
        return img

    def cel(self, frame, layer):
        c = self.cels.get((frame, layer))
        if c and c[0] == "link":
            c = self.cels.get((c[1], layer))
        if c and self.depth == 8 and "indexed" in c[3].info:
            idx = c[3].info["indexed"]
            img = Image.new("RGBA", c[3].size)
            img.putdata([(0, 0, 0, 0) if i == self.transparent else self.palette[i] for i in idx])
            c = (c[0], c[1], c[2], img)
        return c

    def frame(self, f, layers=None):
        """The frame flattened (visible image layers, or the named ones), RGBA, the sprite's full size."""
        out = Image.new("RGBA", (self.w, self.h))
        shown, parents = [], []                              # a layer shows when it and its groups are visible
        for l in self.layers:
            parents = parents[:l["level"]]
            shown.append(l["visible"] and all(parents))
            parents.append(l["visible"])
        for li, l in enumerate(self.layers):
            if l["type"] != 0 or (layers is None and not shown[li]) or (layers is not None and l["name"] not in layers):
                continue
            c = self.cel(f, li)
            if not c:
                continue
            x, y, opacity, img = c
            if opacity < 255 or l["opacity"] < 255:
                a = img.getchannel("A").point(lambda v: v * opacity * l["opacity"] // 65025)
                img = img.copy()
                img.putalpha(a)
            out.alpha_composite(img, (max(x, 0), max(y, 0)), (max(-x, 0), max(-y, 0)))
        return out


def body_info(ase, f, body):
    """The hand (the far end of the front arm) and the part masks, from the base body's hidden layers.
    body: {"hand": "frontArm", "torso": ["body"], "arms": [...], "legs": [...]}"""
    arm = ase.frame(f, [body["hand"]])
    hand = None
    px = arm.load()
    best = -1
    for y in range(arm.height):
        for x in range(arm.width):
            if px[x, y][3] >= 128 and y > best:      # the lowest pixel of the arm: the fist
                best, hand = y, (x, y)
    parts = {}
    for k, key in ((1, "torso"), (2, "arms"), (3, "legs")):
        if body.get(key):
            parts[k] = ase.frame(f, body[key])
    return hand, parts


def rotations(img, grip, base_deg, n):
    """n rotated copies of an icon around its grip (the tip pointing at base_deg in the image): copy k points
    at k * 360 / n degrees (0 right, 90 up). Pixel art kept crisp: 4x up, rotate, then the most frequent
    colour of each 4 x 4 block. Returns [(image, (gx, gy))]."""
    import math
    out = []
    big = img.convert("RGBA").resize((img.width * 4, img.height * 4), Image.NEAREST)
    pad = max(big.size) * 2
    canvas = Image.new("RGBA", (pad, pad))
    gx, gy = grip[0] * 4 + 2, grip[1] * 4 + 2
    canvas.alpha_composite(big, (pad // 2 - gx, pad // 2 - gy))
    for k in range(n):
        deg = k * 360.0 / n - base_deg                     # PIL turns counter-clockwise for positive angles
        rot = canvas.rotate(deg, resample=Image.NEAREST, center=(pad // 2, pad // 2))
        small = scale_image(rot, 0.25)
        out.append((small, (pad // 8, pad // 8)))
    return out


# ── Images: trimming, scaling, spans ──

def scale_image(img, s):
    if s == 1:
        return img
    if s > 1 and s == int(s):                                # whole numbers: each pixel repeated
        return img.resize((img.width * int(s), img.height * int(s)), Image.NEAREST)
    inv = round(1 / s)
    w, h = max(1, round(img.width * s)), max(1, round(img.height * s))
    if abs(1 / inv - s) < 1e-6:                              # integer factor: the most frequent colour per block
        src = img.load()
        out = Image.new("RGBA", (w, h))
        dst = out.load()
        for y in range(h):
            for x in range(w):
                block = [src[x * inv + i, y * inv + j] for j in range(inv) for i in range(inv)
                         if x * inv + i < img.width and y * inv + j < img.height]
                opaque = [p for p in block if p[3] >= 128]
                dst[x, y] = Counter(opaque).most_common(1)[0][0] if len(opaque) * 2 >= len(block) else (0, 0, 0, 0)
        return out
    colours = list({p for p in img.getdata() if p[3] >= 128})
    out = img.resize((w, h), Image.BOX)
    px = out.load()
    for y in range(h):
        for x in range(w):
            p = px[x, y]
            if p[3] < 128:
                px[x, y] = (0, 0, 0, 0)
            elif colours:
                px[x, y] = min(colours, key=lambda c: (c[0] - p[0]) ** 2 + (c[1] - p[1]) ** 2 + (c[2] - p[2]) ** 2)
    return out


def recolor(img, spec):
    """Hues within spec["hue"] = [centre, half width] (degrees) turned to spec["to"]; light and saturation kept."""
    import colorsys
    if not spec:
        return img
    centre, half = spec["hue"]
    to = spec["to"] / 360.0
    out = img.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            if a < 128:
                continue
            h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
            d = abs((h * 360 - centre + 180) % 360 - 180)
            if s < 0.15 or d > half:
                continue
            r2, g2, b2 = colorsys.hls_to_rgb(to, l * spec.get("light", 1.0), min(1.0, s * spec.get("sat", 1.0)))
            px[x, y] = (round(r2 * 255), round(g2 * 255), round(b2 * 255), a)
    return out


def rgb565(p):
    return ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3)


def spans(img):
    """Rows of runs of opaque pixels: u16 runs, then (u16 skip, u16 len, len x u16 RGB565)."""
    out = bytearray()
    px = img.load()
    for y in range(img.height):
        runs, x, last = [], 0, 0
        while x < img.width:
            if px[x, y][3] < 128:
                x += 1
                continue
            start = x
            while x < img.width and px[x, y][3] >= 128:
                x += 1
            runs.append((start - last, [rgb565(px[i, y]) for i in range(start, x)]))
            last = x
        out += struct.pack("<H", len(runs))
        for skip, run in runs:
            out += struct.pack(f"<HH{len(run)}H", skip, len(run), *run)
    return bytes(out)


class Pack:
    def __init__(self):
        self.images, self.anims, self.index = [], [], {}

    def add_image(self, name, img, anchor, scale=1, share=False, hand=None, parts=None):
        """hand: (x, y) in the image; parts: {1: torso, 2: arms, 3: legs} masks (RGBA, the image's size)."""
        img = scale_image(img.convert("RGBA"), scale)
        ax, ay = round(anchor[0] * scale), round(anchor[1] * scale)
        box = img.getchannel("A").point(lambda v: 255 if v >= 128 else 0).getbbox()
        if not box:
            box = (0, 0, 1, 1)
        full = img
        img = img.crop(box)
        pmap = None
        if parts:
            pmap = bytearray((img.width * img.height + 3) // 4)
            loaded = {k: scale_image(m.convert("RGBA"), scale).crop(box).getchannel("A").load() for k, m in parts.items()}
            ip = img.load()
            for y in range(img.height):
                for x in range(img.width):
                    if ip[x, y][3] < 128:
                        continue
                    for k in (1, 2, 3):                       # torso first: it wins where parts overlap
                        if k in loaded and loaded[k][x, y] >= 128:
                            i = y * img.width + x
                            pmap[i >> 2] |= k << ((i & 3) * 2)
                            break
        hx = hy = -32768
        if hand:
            hx, hy = round(hand[0] * scale) - ax, round(hand[1] * scale) - ay
        key = (img.tobytes(), box[0] - ax, box[1] - ay, hx, hy, bytes(pmap) if pmap else None)
        if share and key in self.index:                      # an animation's frame seen before: stored once
            return self.index[key]
        self.images.append({"name": name, "img": img, "ox": box[0] - ax, "oy": box[1] - ay, "hx": hx, "hy": hy,
                            "parts": pmap})
        self.index[key] = len(self.images) - 1
        return len(self.images) - 1

    def write(self, path):
        head = struct.pack("<4sHHHH", b"FART", 2, len(self.images), len(self.anims), 0)
        table_size = len(self.images) * 44
        anims = bytearray()
        for a in self.anims:
            anims += struct.pack("<24sBBH", a["name"].encode()[:23], len(DIRS), a["frames"], a["ms"])
            for d in DIRS:
                frames = a["dirs"].get(d, [NONE] * a["frames"])
                anims += struct.pack(f"<{a['frames']}H", *frames)
        base = len(head) + table_size + len(anims)
        table, data = bytearray(), bytearray()
        for im in self.images:
            sp = base + len(data)
            data += spans(im["img"])
            po = 0
            if im["parts"]:
                if len(data) % 2:
                    data += b"\0"
                po = base + len(data)
                data += im["parts"]
                if len(data) % 2:
                    data += b"\0"
            table += struct.pack("<24shhHHIhhI", im["name"].encode()[:23], im["ox"], im["oy"], im["img"].width,
                                 im["img"].height, sp, im["hx"], im["hy"], po)
        blob = head + table + anims + data
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        open(path, "wb").write(blob)
        return len(blob)


def build(recipe_path, out=None):
    root = os.path.dirname(os.path.abspath(recipe_path))
    r = json.load(open(recipe_path))
    p = Pack()
    sources = {}

    def src(rel):
        path = os.path.normpath(os.path.join(root, rel))
        if not os.path.exists(path):
            sys.exit(f"missing source: {path}")
        sources[rel] = hashlib.sha256(open(path, "rb").read()).hexdigest()
        return path

    for e in r.get("images", []):
        sheet = Image.open(src(e["png"])).convert("RGBA")
        scale = e.get("scale", 1)
        if "grid" in e:
            gw, gh = e["grid"]
            cols = sheet.width // gw
            cells = e.get("cells", [[0, 0]])
            if cells == "all":                               # every cell, numbered row by row (empty ones too)
                cells = [[i % cols, i // cols] for i in range(cols * (sheet.height // gh))]
            for cx, cy in cells:
                name = e["name"] if len(cells) == 1 else f"{e['name']}.{cy * cols + cx}"
                p.add_image(name, sheet.crop((cx * gw, cy * gh, cx * gw + gw, cy * gh + gh)), e["anchor"], scale)
        else:
            p.add_image(e["name"], sheet, e["anchor"], scale)

    for e in r.get("rotated", []):                           # {"name", "png", "grid", "cell", "grip", "angle", "n"}
        sheet = Image.open(src(e["png"])).convert("RGBA")
        gw, gh = e["grid"]
        cx, cy = e["cell"]
        icon = sheet.crop((cx * gw, cy * gh, cx * gw + gw, cy * gh + gh))
        for k, (im, grip) in enumerate(rotations(icon, e["grip"], e.get("angle", 45), e.get("n", 16))):
            p.add_image(f"{e['name']}.{k}", im, grip)

    for e in r.get("anims", []):
        scale = e.get("scale", 1)
        a = {"name": e["name"], "dirs": {}, "ms": e.get("ms", 100)}
        if "files" in e:                                     # a PNG per facing, a row of frames each
            gw, gh = e["grid"]
            for d, f in e["files"].items():
                sheet = Image.open(src(f)).convert("RGBA")
                idx = e.get("pick", list(range(e.get("frames", sheet.width // gw))))
                a["dirs"][d] = [p.add_image(f"{e['name']}.{d}.{k}",
                                            recolor(sheet.crop((i * gw, 0, i * gw + gw, gh)), e.get("recolor")),
                                            e["anchor"], scale, share=True) for k, i in enumerate(idx)]
        elif "png" in e:
            sheet = Image.open(src(e["png"])).convert("RGBA")
            gw, gh = e["grid"]
            n = e["frames"]
            for d, row in e["rows"].items():
                a["dirs"][d] = [p.add_image(f"{e['name']}.{d}.{i}", sheet.crop((i * gw, row * gh, i * gw + gw, row * gh + gh)),
                                            e["anchor"], scale, share=True) for i in range(n)]
        else:
            ase = Ase(src(e["aseprite"]))
            tags = {t["name"]: t for t in ase.tags}
            n = None
            for d, tag in e["tags"].items():
                # A facing is a tag name, or {"tag", "layers", "anchor"} when the file keeps facings apart
                # (each in its own layers, in its own part of the canvas).
                spec = tag if isinstance(tag, dict) else {"tag": tag}
                t = tags[spec["tag"]]
                frames = list(range(t["from"], t["to"] + 1))
                if "pick" in e:                              # some of the tag's frames (a long idle, thinned)
                    frames = [frames[i] for i in e["pick"] if i < len(frames)]
                n = len(frames) if n is None else min(n, len(frames))
                a["ms"] = e.get("ms", ase.durations[t["from"]])
                imgs = [ase.frame(f, spec.get("layers", e.get("layers"))) for f in frames]
                anchor = spec.get("anchor", e.get("anchor"))
                body = e.get("body")                         # the base body's layers: hand and part map
                extras = [body_info(ase, f, body) if body else (None, None) for f in frames]
                a["dirs"][d] = [p.add_image(f"{e['name']}.{d}.{i}", im, anchor, scale, share=True,
                                            hand=extras[i][0], parts=extras[i][1])
                                if im.getchannel("A").getbbox() else NONE for i, im in enumerate(imgs)]
            a["dirs"] = {d: v for d, v in a["dirs"].items() if any(i != NONE for i in v)}
            if not a["dirs"]:
                print(f"{e['name']}: every frame is empty, skipped")
                continue
            a["dirs"] = {d: v[:n] for d, v in a["dirs"].items()}
        if not a["dirs"]:
            continue
        a["frames"] = min(len(v) for v in a["dirs"].values())
        a["dirs"] = {d: v[:a["frames"]] for d, v in a["dirs"].items()}
        p.anims.append(a)

    out = out or os.path.join(root, r.get("out", "assets/art.fart"))
    size = p.write(out)
    print(f"{out}: {len(p.images)} images, {len(p.anims)} animations, {size} bytes")
    if r.get("previews"):
        preview(p, os.path.join(root, r["previews"]))
    json.dump(sources, open(out + ".sources.json", "w"), indent=1, sort_keys=True)


def preview(p, prefix):
    """Every image on a grid (magnified 2x), to look at what came out."""
    if not p.images:
        return
    cw = max(im["img"].width for im in p.images) + 4
    ch = max(im["img"].height for im in p.images) + 4
    cols = max(1, min(16, 1024 // (cw * 2)))
    rows = (len(p.images) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cw, rows * ch), (40, 44, 60, 255))
    for i, im in enumerate(p.images):
        sheet.alpha_composite(im["img"], ((i % cols) * cw + 2, (i // cols) * ch + 2))
    os.makedirs(os.path.dirname(os.path.abspath(prefix)), exist_ok=True)
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(prefix + ".png")
    print(f"preview: {prefix}.png")


def show(path):
    b = open(path, "rb").read()
    magic, ver, ni, na, _ = struct.unpack_from("<4sHHHH", b, 0)
    print(f"{path}: version {ver}, {ni} images, {na} animations, {len(b)} bytes")
    o = 12
    for i in range(ni):
        o += 44 if ver >= 2 else 36
    for i in range(na):
        name, dirs, frames, ms = struct.unpack_from("<24sBBH", b, o)
        idx = struct.unpack_from(f"<{dirs * frames}H", b, o + 28)
        o += 28 + dirs * frames * 2
        have = [DIRS[d] for d in range(dirs) if idx[d * frames] != NONE]
        print(f"  {name.rstrip(b'\\0').decode()}: {frames} frames, {ms} ms, facings {' '.join(have)}")


def info(path):
    if path.endswith((".ase", ".aseprite")):
        a = Ase(path)
        print(f"{path}: {a.w} x {a.h}, {a.depth}-bit, {len(a.durations)} frames ({sorted(set(a.durations))} ms)")
        for i, l in enumerate(a.layers):
            kind = {0: "image", 1: "group", 2: "tilemap"}.get(l["type"], "?")
            print(f"  layer {i}: {'  ' * l['level']}{l['name']} ({kind}{'' if l['visible'] else ', hidden'})")
        for t in a.tags:
            print(f"  tag {t['name']}: frames {t['from']}-{t['to']}")
    else:
        img = Image.open(path)
        print(f"{path}: {img.width} x {img.height}, {img.mode}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("info"); s.add_argument("file")
    s = sub.add_parser("build"); s.add_argument("recipe"); s.add_argument("-o", "--out")
    s = sub.add_parser("show"); s.add_argument("file")
    a = ap.parse_args()
    if a.cmd == "info":
        info(a.file)
    elif a.cmd == "build":
        build(a.recipe, a.out)
    else:
        show(a.file)


if __name__ == "__main__":
    main()
