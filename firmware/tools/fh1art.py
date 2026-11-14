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
       "grid": [64, 64], "anchor": [32, 50], "pick": [0, 2], "recolor": {"hue": [285, 45], "to": 8}},
      {"name": "knight.down", "aseprite": "....aseprite", "tags": {"se": "Se"}, "kneel": true, "body": {...}},
                                                     a knock-out made from each tag's first frame (kneel_frames)
      {"name": "fx.circle", "png": "....png", "grid": [64, 64], "rows": {"se": 1}, "anchor": [32, 32],
       "scale": 0.75, "squash": 0.5}                 an effect laid flat on the ground; "frames": every one if left out
    ],
    "tile_anims": [                                 a tile's frames: images above, or made from one of them
      {"name": "water.deep", "from": "tile.94", "frames": ["tile.94", "tile.95", "tile.96", "tile.95"], "ms": 260},
      {"name": "tile.90", "from": "tile.90", "ripple": {"frames": 6, "ms": 170, "period": 12, "keep": 2}}
    ]
  }

"frames" names images already in the pack (a tileset's own steps of a swell or a glint), in order; an image may
repeat to hold it longer. A "ripple" (water, lava) groups the image's colours into brightness levels and moves a wave through the levels
above the darkest `keep` (outlines and sides stay), along the screen's diagonal, one `period` (pixels) per loop.
Every frame is a step of the wave (none is the still image, so the loop never pauses); cells naming the still
image show the frames instead (iso_tile_anim). The animation has the south-east facing only. A ripple's "rows":
[y0, y1] keeps it to those rows of the image (a fire's flames, not its logs). A "turn": {"part": png, "plane": [c,
k], "frames": 12, "turn": 360, "ms": 90} finds the part in the image and turns it about its middle in its tilted
plane (screen x = c * p, y = q + k * p), pixel art kept crisp. A "whirl": {"part": png, "plane": [c, k], "frames":
12, "arms": 2, "twist": 2.5, "rim": 2, "ms": 80} keeps the part's rim and winds spiral arms round its middle in
its own colours, dark at the middle: a portal's vortex. On an image, "cut": {"part": png, "keep": "rest"|"front"}
takes a part out of it (the rest: what is round it and what stands in front of it, the light it casts on them
greyed; front: only what stands in front; glow: only that light, in the part's colours), and
"crop": [x, y, w, h] takes one piece of a sheet laid out by hand (anchor default: its top-left), and
"anchor_in": {"png": whole, "anchor": [x, y]} anchors a part as it sits in a whole picture (drawn at the whole's
anchor, it lands in its place).

Every image is trimmed to its visible pixels and keeps its anchor (the point that stands on the ground).
A recipe with "smooth": true holds drawn (not pixel) art: every "scale" filters it (a box filter, premultiplied) and
cuts its edge at half opacity; with "aa": true as well, the soft edge stays (an edge list the engine blends) on every
image but ground blocks ("seamless" ones; "aa" on an entry says otherwise). "seamless": [cx, cy, hw, hh] on a block repaints the band along its top face (that
diamond, in source pixels) with the colour inward, so flat ground drawn block by block has no grid. "over":
[[png, x, y], ...] on an image puts other pictures on top of it first (at x, y of it; the canvas grows to hold them).
"buildings": {...} renders a building game's houses, smithies and keeps from simple 3D models (fh1model.py's
building_set: a piece per cell for every way a cell can be joined, in reference colours a game swaps as it draws,
and the palettes), with their soft edges.
"spin": {"hub": [x, y], "plane": [c, k], "frames": 8, "turn": 90} makes frames NAME.0.. of the picture turned
about its hub in a tilted plane (screen x = c * p, y = q + k * p): a windmill's sails. "tint":
[[y0, y1, "#rrggbb"], ...] on a sheet animation colours a white template by height bands (skin, shirt...). A
"scale" below 1 shrinks pixel art: integer factors (1/2, 1/3) keep the most frequent colour of each block;
other factors average by area and snap each pixel to the source's nearest colour; whole numbers above 1 repeat
pixels. "files" gives a facing per PNG (a row of frames each; "frames" or as many as fit), "pick" keeps some
frames, "recolor" turns the hues within [centre, half width] (degrees) to "to", keeping light and saturation
("sat" and "light" multiply saturation and lightness): a red slime from a purple one, its golden crown kept.

The pack (little-endian), read by the engine:
  header  "FART" u16 version(3) u16 images u16 anims u16 0
  images  per image: name[24] i16 ox i16 oy (top-left from the anchor) u16 w u16 h u32 offset (spans)
          i16 hx i16 hy (the hand, from the anchor; -32768: none) u32 parts (offset of a part map, 0: none:
          2 bits a pixel, row by row over w x h: 0 none, 1 torso, 2 arms, 3 legs) u32 edge (offset of a soft edge,
          0: none)
  anims   per anim: name[24] u8 dirs u8 frames u16 ms; then dirs x frames u16 image indexes
          (direction order: se, sw, nw, ne, then s, w, n, e when the anim has any of those (dirs 8, else 4);
          0xffff where missing: the engine mirrors se<->sw, ne<->nw, e<->w, and shows a cardinal facing it
          lacks as the diagonal next to it)
  spans   per image row: u16 runs, then per run u16 skip, u16 len, len x u16 RGB565 (all 16-bit: the S3
          cannot read a u16 at an odd address)
  edge    per image row: u16 n, then n x (u16 x, u16 RGB565, u16 alpha 0..255): the pixels the engine blends over
          what is under them (the recipe's "aa": smooth art keeps the soft edge its scaling made; alpha from 224 is
          in the spans, below 24 dropped)
"""
import argparse
import hashlib
import json
import math
import os
import struct
import sys
import zlib
from collections import Counter

try:
    import numpy as np
except ImportError:
    np = None

try:
    from PIL import Image
except ImportError:
    sys.exit("fh1art.py needs Pillow (run it with /usr/bin/python3 or install python3-pillow)")

DIRS = ["se", "sw", "nw", "ne", "s", "w", "n", "e"]   # the first four always; the rest only if present
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


# A knock-out made from a standing frame (no such art in the pack): the knees bend (the legs squash toward the feet,
# the rest sinks), then the head drops and tips the way the figure faces. Per step: how far the body sinks, how far
# the head drops below it and how far it tips forward.
KNEEL = [(0, 0, 0), (2, 0, 0), (4, 1, 0), (6, 2, 1), (6, 3, 1)]
FORWARD = {"se": 1, "e": 1, "ne": 1, "sw": -1, "w": -1, "nw": -1, "s": 0, "n": 0}


def kneel_frames(ase, f, body, facing):
    """The KNEEL steps of frame f: [(image, hand, parts)], cut along the base body's lines: below the hip (the
    torso's bottom) the legs, above the neck (the torso's top) the head, the arms with the body."""
    legs = ase.frame(f, body["legs"])
    torso = ase.frame(f, body["torso"])
    arms = ase.frame(f, body["arms"]).getchannel("A").load()
    tb = torso.getbbox()
    lb = legs.getbbox()
    if not tb or not lb:
        return []
    neck, hip, feet = tb[1], tb[3] - 1, lb[3]
    fwd = FORWARD.get(facing, 0)

    def bend(img, d, e, t):
        src = img.load()
        w, h = img.size
        out = Image.new("RGBA", (w, h))
        leg = Image.new("RGBA", (w, max(1, feet - hip)))
        lp = leg.load()
        rest = Image.new("RGBA", (w, h))
        head = Image.new("RGBA", (w, h))
        rp, hp = rest.load(), head.load()
        for y in range(h):
            for x in range(w):
                p = src[x, y]
                if p[3] < 128:
                    continue
                if y >= hip and arms[x, y] < 128:
                    if y < feet:
                        lp[x, y - hip] = p
                elif y < neck and arms[x, y] < 128:
                    hp[x, y] = p
                else:
                    rp[x, y] = p
        n = max(1, feet - hip - d)
        out.alpha_composite(leg.resize((w, n), Image.NEAREST), (0, feet - n))
        out.alpha_composite(rest.transform((w, h), Image.AFFINE, (1, 0, 0, 0, 1, -d)), (0, 0))
        out.alpha_composite(head.transform((w, h), Image.AFFINE, (1, 0, -t * fwd, 0, 1, -(d + e))), (0, 0))
        return out

    frames = []
    for d, e, t in KNEEL:
        img = bend(ase.frame(f), d, e, t)
        hand, parts = body_info(ase, f, body)
        parts = {k: bend(m, d, e, t) for k, m in parts.items()}
        arm = bend(ase.frame(f, [body["hand"]]), d, e, t)
        best, hand, px = -1, None, arm.load()
        for y in range(arm.height):
            for x in range(arm.width):
                if px[x, y][3] >= 128 and y > best:
                    best, hand = y, (x, y)
        frames.append((img, hand, parts))
    return frames


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

SMOOTH = False                                               # the recipe's "smooth": vector-like art, not pixel art
AA = False                                                   # the recipe's "aa": soft edges kept (an edge list per image)
EDGE_LO, EDGE_HI = 24, 224                                   # alpha: below, dropped; from EDGE_HI, opaque; between, edge


def _find_part(b, q):
    """Where part q (RGBA array) sits in base b: the offset where its pixels match best."""
    pa = q[:, :, 3] > 0
    best = None
    for y in range(b.shape[0] - q.shape[0] + 1):
        for x in range(b.shape[1] - q.shape[1] + 1):
            d = np.abs(b[y:y + q.shape[0], x:x + q.shape[1], :3] - q[:, :, :3]).sum(axis=2)[pa].sum()
            if best is None or d < best[0]:
                best = (d, x, y)
    return best[1], best[2]


def cut_part(base, part, keep):
    """base with part (found in it) taken out. keep "rest": what is round it and what stands in front of it (the
    pixels over the part's shape that are not the part's colours), its glow (pixels tinted like the part, the light
    it casts on what is round it) turned to plain grey of the same brightness; "front": only what stands in front of
    it; "glow": only that glow, in the part's colours nearest in brightness (so a game swapping the part's colours
    swaps its light too)."""
    b = np.array(base.convert("RGBA")).astype(int)
    q = np.array(part.convert("RGBA")).astype(int)
    ox, oy = _find_part(b, q)
    h, w = q.shape[:2]
    lum = lambda c: (c[0] * 3 + c[1] * 6 + c[2]) / 10
    pcols = sorted({tuple(q[y, x, :3]) for y in range(h) for x in range(w) if q[y, x, 3]}, key=lum)
    pr, pg, pb = np.mean([c[0] for c in pcols]), np.mean([c[1] for c in pcols]), np.mean([c[2] for c in pcols])
    def tinted(c):                                           # leaning to the part's hue, clearly
        r, g, bl = c[:3]
        return (pr > pg and pr > pb and r > g + 40 and r > bl + 40) or (pg > pr and pg > pb and g > r + 40 and g > bl + 40) \
            or (pb > pr and pb > pg and bl > r + 40 and bl > g + 40)
    inside = np.zeros(b.shape[:2], bool)
    out = b.copy() if keep == "rest" else np.zeros_like(b)
    for y in range(h):
        for x in range(w):
            if q[y, x, 3] == 0:
                continue
            inside[oy + y, ox + x] = True
            # In front: not the part's colour there, and not tinted like it (the part's own edge drawn a little
            # differently in the whole picture is still the part).
            front = abs(b[oy + y, ox + x, :3] - q[y, x, :3]).sum() > 30 and not tinted(b[oy + y, ox + x])
            if keep == "rest" and not front:
                out[oy + y, ox + x] = 0
            if keep == "front" and front:
                out[oy + y, ox + x] = b[oy + y, ox + x]
    if keep in ("rest", "glow"):
        for y in range(b.shape[0]):
            for x in range(b.shape[1]):
                c = b[y, x]
                if c[3] == 0 or inside[y, x] or not tinted(c):
                    continue
                if keep == "rest":
                    g = int(lum(c))
                    out[y, x] = (g, g, g, c[3])
                else:
                    out[y, x] = min(pcols, key=lambda pc: abs(lum(pc) - lum(c))) + (255,)
    return Image.fromarray(out.astype(np.uint8))


def whirl_part(base, part, plane, frames, arms=2, twist=2.5, rim=2):
    """Frames of base (pixel art) with part, found in it, whirling: inside its rim (rim pixels from its edge, kept),
    spiral arms wind round its middle in its tilted plane (screen x = c * p, y = q + k * p), drawn in the part's own
    colours from dark (the middle) to light: a portal's vortex."""
    import math
    b = np.array(base.convert("RGBA")).astype(int)
    q = np.array(part.convert("RGBA")).astype(int)
    pa = q[:, :, 3] > 0
    ox, oy = _find_part(b, q)
    h, w = pa.shape
    inside = pa.copy()                                       # the part less its rim
    for _ in range(rim):
        n = inside.copy()
        n[1:, :] &= inside[:-1, :]
        n[:-1, :] &= inside[1:, :]
        n[:, 1:] &= inside[:, :-1]
        n[:, :-1] &= inside[:, 1:]
        inside = n
    lum = lambda c: c[0] * 3 + c[1] * 6 + c[2]
    cols = sorted({tuple(q[y, x]) for y in range(h) for x in range(w) if inside[y, x]}, key=lum)
    c, k = plane
    cx, cy = (w - 1) / 2, (h - 1) / 2
    R = max(1.0, h / 2 - rim)
    out = []
    for f in range(frames):
        fr = b.copy()
        for y in range(h):
            for x in range(w):
                if not inside[y, x]:
                    continue
                pp = (x - cx) / c                            # back into the plane: a circle
                qq = (y - cy) - k * pp
                rho = min(1.0, math.hypot(pp, qq) / R)
                phi = math.atan2(qq, pp)
                arm = 0.5 + 0.5 * math.sin(arms * phi + twist * 2 * math.pi * rho - 2 * math.pi * f / frames)
                v = 0.15 + 0.5 * rho + 0.35 * arm * (0.4 + 0.6 * rho)
                fr[oy + y, ox + x] = cols[max(0, min(len(cols) - 1, int(v * len(cols))))]
        out.append(Image.fromarray(fr.astype(np.uint8)))
    return out


def turn_part(base, part, plane, frames, turn):
    """Frames of base (pixel art) with part, found in it, turned about its middle in a tilted plane (screen x = c * p,
    y = q + k * p): a portal's vortex. Turned 4x up and back by the most frequent colour of each block, so the
    pixels stay crisp; only within the part's own shape."""
    import math
    b = np.array(base.convert("RGBA")).astype(int)
    q = np.array(part.convert("RGBA")).astype(int)
    pa = q[:, :, 3] > 0
    best = None                                              # where the part sits in the base: the best match
    for y in range(b.shape[0] - q.shape[0] + 1):
        for x in range(b.shape[1] - q.shape[1] + 1):
            d = np.abs(b[y:y + q.shape[0], x:x + q.shape[1], :3] - q[:, :, :3]).sum(axis=2)[pa].sum()
            if best is None or d < best[0]:
                best = (d, x, y)
    _, ox, oy = best
    c, k = plane
    ss = 4
    big = part.convert("RGBA").resize((part.width * ss, part.height * ss), Image.NEAREST)
    cx, cy = part.width * ss / 2, part.height * ss / 2
    out = []
    for i in range(frames):
        th = -math.radians(turn) * i / frames
        R = [[math.cos(th), -math.sin(th)], [math.sin(th), math.cos(th)]]
        S, Si = [[c, 0], [k, 1]], [[1 / c, 0], [-k / c, 1]]
        SR = [[sum(S[a][m] * R[m][j] for m in range(2)) for j in range(2)] for a in range(2)]
        (a, bb), (d, e) = [[sum(SR[a][m] * Si[m][j] for m in range(2)) for j in range(2)] for a in range(2)]
        t = big.transform(big.size, Image.AFFINE, (a, bb, cx - a * cx - bb * cy, d, e, cy - d * cx - e * cy),
                          resample=Image.NEAREST)
        arr = np.array(t).reshape(part.height, ss, part.width, ss, 4).transpose(0, 2, 1, 3, 4).reshape(
            part.height, part.width, ss * ss, 4)
        fr = b.copy()
        for y in range(part.height):
            for x in range(part.width):
                if not pa[y, x]:
                    continue
                cols = [tuple(v) for v in arr[y, x] if v[3] > 0]
                col = max(set(cols), key=cols.count) if cols else tuple(q[y, x])
                fr[oy + y, ox + x] = col
        out.append(Image.fromarray(fr.astype(np.uint8)))
    return out


def spin_frames(img, hub, plane, frames, turn):
    """A picture turned about hub (source pixels) in a tilted plane, `frames` steps over `turn` degrees: the plane is
    seen as screen x = c * p, y = q + k * p (plane = [c, k]: a windmill's sails on a wall turned 45 degrees), so each
    frame is S R S^-1. Returns (frames, hub in the frames): square canvases round the hub, 4x supersampled."""
    c, k = plane
    ss = 4
    r = max(math.hypot(x - hub[0], y - hub[1]) for x in (0, img.width) for y in (0, img.height))
    size = int(math.ceil(r * 1.6)) * 2
    big = img.resize((img.width * ss, img.height * ss), Image.LANCZOS)
    S = [[c, 0.0], [k, 1.0]]
    Si = [[1 / c, 0.0], [-k / c, 1.0]]
    out = []
    for i in range(frames):
        th = -math.radians(turn) * i / frames                  # the inverse turn: PIL maps output to input
        R = [[math.cos(th), -math.sin(th)], [math.sin(th), math.cos(th)]]
        SR = [[sum(S[a][m] * R[m][b] for m in range(2)) for b in range(2)] for a in range(2)]
        (a, b), (d, e) = [[sum(SR[i2][m] * Si[m][j] for m in range(2)) for j in range(2)] for i2 in range(2)]
        cx = cy = size * ss / 2
        im = big.transform((size * ss, size * ss), Image.AFFINE,
                           (a, b, hub[0] * ss - a * cx - b * cy, d, e, hub[1] * ss - d * cx - e * cy),
                           resample=Image.BICUBIC)
        out.append(im.resize((size, size), Image.LANCZOS))
    return out, (size / 2, size / 2)


def scale_image(img, s, keep_alpha=False):
    if s == 1:
        return img
    if SMOOTH:                                               # filtered (an area average), then a hard edge at half opacity
        # Premultiplied while filtering: the transparent pixels round a shape must not darken its edge. A box filter,
        # not Lanczos: Lanczos rings (light and dark lines) along a block's edge, a grid over the ground.
        out = img.convert("RGBa").resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.BOX)
        out = out.convert("RGBA")
        if not keep_alpha:
            out.putalpha(out.getchannel("A").point(lambda v: 255 if v >= 128 else 0))
        return out
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


def seamless(img, face):
    """A block's top face without its border outline: the band along the diamond (centre cx, cy; half widths hw,
    hh) repainted with the colour found toward the centre, so flat ground drawn block by block reads as one surface
    (a road or river crossing the edge keeps going: the colour inward is its own)."""
    if not face:
        return img
    cx, cy, hw, hh = face
    img = img.copy()
    px = img.load()
    src = img.copy().load()
    dark = lambda p: p[3] < 200 or p[0] + p[1] + p[2] < 200
    for y in range(img.height):
        for x in range(img.width):
            d = abs(x + 0.5 - cx) / hw + abs(y + 0.5 - cy) / hh
            if not 0.8 <= d <= 1.04:
                continue
            for k in range(1, 14):                           # toward the centre, to the first face colour inside
                t = 1 - k * 0.025
                sx, sy = int(cx + (x + 0.5 - cx) * t), int(cy + (y + 0.5 - cy) * t)
                if 0 <= sx < img.width and 0 <= sy < img.height and not dark(src[sx, sy]) \
                        and abs(sx + 0.5 - cx) / hw + abs(sy + 0.5 - cy) / hh < 0.8:
                    px[x, y] = src[sx, sy][:3] + (255,)
                    break
    return img


def flat_top(img, face):
    """A block's top face in one colour (its most common one): no strokes repeating tile after tile on plain ground."""
    from collections import Counter
    cx, cy, hw, hh = face
    img = img.copy()
    px = img.load()
    inside = [(x, y) for y in range(img.height) for x in range(img.width)
              if abs(x + 0.5 - cx) / hw + abs(y + 0.5 - cy) / hh <= 1.04]
    main = Counter(px[x, y][:3] for x, y in inside if px[x, y][3] >= 128).most_common(1)[0][0]
    for x, y in inside:
        px[x, y] = main + (255,)
    return img


def paint_paths(base, face, kind, spec):
    """Road, river and bridge tiles painted on a block's top face, one per set of neighbours: bands in the cell's own
    coordinates (u along +c, v along +r, 0..1) from the centre to each joined edge's midpoint, all of one width, so
    neighbouring pieces always meet; ringed by the outline. Sampled 4 x 4 a pixel (smooth edges). Returns
    {name: image}: "<kind>.<mask>" (mask bits N 1, E 2, S 4, W 8), or for a bridge "bridge.ew" / "bridge.ns" (a
    plank deck one way over water the other)."""
    import math
    cx, cy, hw, hh = face
    hex_ = lambda c: tuple(int(c[i:i + 2], 16) for i in (1, 3, 5))
    out_c = hex_(spec.get("outline", "#2e222f"))
    mids = {1: (0.5, 0.0), 2: (1.0, 0.5), 4: (0.5, 1.0), 8: (0.0, 0.5)}

    def seg(pu, pv, a, b):
        ax, ay = a; bx, by = b
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((pu - ax) * dx + (pv - ay) * dy) / (dx * dx + dy * dy or 1)))
        return math.hypot(pu - ax - t * dx, pv - ay - t * dy)

    def band(pu, pv, mask, w):                                 # signed distance to the band (negative inside)
        d = math.hypot(pu - 0.5, pv - 0.5) - w
        for bit, m in mids.items():
            if mask & bit:
                d = min(d, seg(pu, pv, (0.5, 0.5), m) - w)
        return d

    def render(layers):
        img = base.copy()
        px = img.load()
        for y in range(img.height):
            for x in range(img.width):
                if abs(x + 0.5 - cx) / hw + abs(y + 0.5 - cy) / hh > 1.04:   # as far as the seamless band reaches
                    continue
                acc = [0, 0, 0]
                for sy in range(4):
                    for sx in range(4):
                        fx = (x + (sx + 0.5) / 4 - cx) / hw
                        fy = (y + (sy + 0.5) / 4 - (cy - hh)) / hh
                        pu, pv = (fx + fy) / 2, (fy - fx) / 2
                        col = px[x, y][:3]
                        for fn in layers:                      # later layers on top
                            c = fn(pu, pv)
                            if c:
                                col = c
                        for k in range(3):
                            acc[k] += col[k]
                px[x, y] = (acc[0] // 16, acc[1] // 16, acc[2] // 16, 255)
        return img

    def layer(mask, w, fill, line=0.035, stripes=None):
        fill = hex_(fill)
        def fn(pu, pv):
            d = band(pu, pv, mask, w)
            if d > line:
                return None
            if d > 0:
                return out_c
            if stripes:                                        # planks across the deck
                axis, step, col = stripes
                if (pu if axis == "u" else pv) % step < 0.03:
                    return hex_(col)
            return fill
        return fn

    found = {}
    if kind == "bridge":
        water = layer(1 | 4, spec.get("water_width", 0.27), spec.get("water", "#96d0f3"))
        found["bridge.ew"] = render([water, layer(2 | 8, spec.get("width", 0.2), spec.get("deck", "#cd693d"),
                                                  stripes=("u", 0.14, "#9f463a"))])
        water = layer(2 | 8, spec.get("water_width", 0.27), spec.get("water", "#96d0f3"))
        found["bridge.ns"] = render([water, layer(1 | 4, spec.get("width", 0.2), spec.get("deck", "#cd693d"),
                                                  stripes=("v", 0.14, "#9f463a"))])
        return found
    for mask in range(16):
        found[f"{kind}.{mask}"] = render([layer(mask, spec.get("width", 0.18), spec["fill"])])
    return found


def tint(img, bands):
    """A white template coloured by height bands ([y0, y1, "#rrggbb"], source pixels): its light pixels take the band's
    colour, keeping their shading (an outline and dark details stay)."""
    if not bands:
        return img
    img = img.copy()
    px = img.load()
    cols = [(y0, y1, tuple(int(c[i:i + 2], 16) for i in (1, 3, 5))) for y0, y1, c in bands]
    for y in range(img.height):
        band = next((c for y0, y1, c in cols if y0 <= y < y1), None)
        if not band:
            continue
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a >= 128 and min(r, g, b) > 140:
                k = (r + g + b) / 765
                px[x, y] = (round(band[0] * k), round(band[1] * k), round(band[2] * k), a)
    return img


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


def ripple(img, frames, period, keep, amp=1.0, rows=None):
    """Frames of a wave running through an image's brightness levels, each a step of the same loop (rows: only
    those [y0, y1) of the image: a fire's flames, not its logs)."""
    import math
    img = img.convert("RGBA")
    px = img.load()
    lum = lambda c: c[0] * 3 + c[1] * 6 + c[2]
    cols = sorted({px[x, y] for y in range(img.height) for x in range(img.width) if px[x, y][3] >= 128}, key=lum)
    levels = []
    for c in cols:                                           # colours closer than this in brightness: one level
        if levels and lum(c) - lum(levels[-1][-1]) < 40:
            levels[-1].append(c)
        else:
            levels.append([c])
    lev = {c: i for i, l in enumerate(levels) for c in l}
    rep = [l[len(l) // 2] for l in levels]
    out = []
    for k in range(frames):
        im = img.copy()
        q = im.load()
        for y in range(img.height):
            for x in range(img.width):
                c = px[x, y]
                if c[3] < 128 or lev[c] < keep or (rows and not rows[0] <= y < rows[1]):
                    continue
                i = max(keep, min(len(levels) - 1, lev[c] + round(amp * math.sin(2 * math.pi * (k / frames + (x + 2 * y) / period)))))
                if i != lev[c]:
                    q[x, y] = rep[i]
        out.append(im)
    return out


def rgb565(p):
    return ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3)


def spans(img, aa=False):
    """Rows of runs of opaque pixels: u16 runs, then (u16 skip, u16 len, len x u16 RGB565). With soft edges (aa),
    only the pixels from EDGE_HI on; the rest of the edge goes in edges()."""
    lo = EDGE_HI if aa else 128
    out = bytearray()
    px = img.load()
    for y in range(img.height):
        runs, x, last = [], 0, 0
        while x < img.width:
            if px[x, y][3] < lo:
                x += 1
                continue
            start = x
            while x < img.width and px[x, y][3] >= lo:
                x += 1
            runs.append((start - last, [rgb565(px[i, y]) for i in range(start, x)]))
            last = x
        out += struct.pack("<H", len(runs))
        for skip, run in runs:
            out += struct.pack(f"<HH{len(run)}H", skip, len(run), *run)
    return bytes(out)


def edges(img):
    """A soft edge's pixels, blended over what is under them: per row u16 n, then n x (u16 x, u16 RGB565, u16 alpha)."""
    out = bytearray()
    px = img.load()
    for y in range(img.height):
        row = [(x, rgb565(px[x, y]), px[x, y][3]) for x in range(img.width) if EDGE_LO <= px[x, y][3] < EDGE_HI]
        out += struct.pack("<H", len(row))
        for x, c, a in row:
            out += struct.pack("<HHH", x, c, a)
    return bytes(out)


class Pack:
    def __init__(self):
        self.images, self.anims, self.index = [], [], {}

    def add_image(self, name, img, anchor, scale=1, share=False, hand=None, parts=None, aa=False, soft=False):
        """hand: (x, y) in the image; parts: {1: torso, 2: arms, 3: legs} masks (RGBA, the image's size); aa: keep a
        soft edge (smooth art scaled down)."""
        aa = aa and AA and (soft or SMOOTH and scale != 1)    # soft: drawn already with its soft edge (models)
        img = scale_image(img.convert("RGBA"), scale, keep_alpha=aa)
        ax, ay = round(anchor[0] * scale), round(anchor[1] * scale)
        box = img.getchannel("A").point(lambda v: 255 if v >= (EDGE_LO if aa else 128) else 0).getbbox()
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
                            "parts": pmap, "aa": aa})
        self.index[key] = len(self.images) - 1
        return len(self.images) - 1

    def write(self, path):
        head = struct.pack("<4sHHHH", b"FART", 3, len(self.images), len(self.anims), 0)
        table_size = len(self.images) * 48
        anims = bytearray()
        for a in self.anims:
            dirs = DIRS if any(d in a["dirs"] for d in DIRS[4:]) else DIRS[:4]
            anims += struct.pack("<24sBBH", a["name"].encode()[:23], len(dirs), a["frames"], a["ms"])
            for d in dirs:
                frames = a["dirs"].get(d, [NONE] * a["frames"])
                anims += struct.pack(f"<{a['frames']}H", *frames)
        base = len(head) + table_size + len(anims)
        table, data = bytearray(), bytearray()
        for im in self.images:
            sp = base + len(data)
            data += spans(im["img"], im["aa"])
            po = 0
            if im["parts"]:
                if len(data) % 2:
                    data += b"\0"
                po = base + len(data)
                data += im["parts"]
                if len(data) % 2:
                    data += b"\0"
            eo = 0
            if im["aa"]:
                if len(data) % 2:
                    data += b"\0"
                eo = base + len(data)
                data += edges(im["img"])
            table += struct.pack("<24shhHHIhhII", im["name"].encode()[:23], im["ox"], im["oy"], im["img"].width,
                                 im["img"].height, sp, im["hx"], im["hy"], po, eo)
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

    global SMOOTH, AA
    SMOOTH = bool(r.get("smooth"))
    AA = bool(r.get("aa"))
    for e in r.get("painted", []):                           # roads, rivers, bridges painted on a block
        base = seamless(Image.open(src(e["base"])).convert("RGBA"), e["face"])
        if e.get("flat"):
            base = flat_top(base, e["face"])
        for name, im in paint_paths(base, e["face"], e["kind"], e).items():
            p.add_image(name, im, e["anchor"], e.get("scale", 1))
    if "buildings" in r:                                     # houses and smithies from 3D models (fh1model.py)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import fh1model
        for name, im, anchor in fh1model.building_set(r["buildings"]):
            p.add_image(name, im, anchor, 1, aa=not name.startswith("pal."), soft=True)
        for fl in r["buildings"].get("flags", []):          # waving flags (the engine's flag.h)
            for name, im, anchor in fh1model.flag_set(fl, fl.get("name", "flag")):
                p.add_image(name, im, anchor, 1, aa=not name.endswith(".key"), soft=True)
        if "castle_h" in r["buildings"]:                     # castle walls and keeps, joined edge to edge
            for name, im, anchor in fh1model.castle_set(r["buildings"]):
                p.add_image(name, im, anchor, 1, aa=True, soft=True)
    for e in r.get("images", []):
        sheet = seamless(Image.open(src(e["png"])).convert("RGBA"), e.get("seamless"))
        if "crop" in e:                                     # [x, y, w, h]: one piece of a sheet laid out by hand
            x, y, w, h = e["crop"]
            sheet = sheet.crop((x, y, x + w, y + h))
        anchor = e.get("anchor", [0, 0])
        for o in e.get("over", []):                          # [png, x, y]: another picture on top (a mill's sails)
            top = Image.open(src(o[0])).convert("RGBA")
            x0, y0 = min(0, o[1]), min(0, o[2])
            canvas = Image.new("RGBA", (max(sheet.width, o[1] + top.width) - x0,
                                        max(sheet.height, o[2] + top.height) - y0))
            canvas.alpha_composite(sheet, (-x0, -y0))
            canvas.alpha_composite(top, (o[1] - x0, o[2] - y0))
            sheet, anchor = canvas, [anchor[0] - x0, anchor[1] - y0]
        if "cut" in e:                                      # a part taken out of it (a portal's vortex)
            sheet = cut_part(sheet, Image.open(src(e["cut"]["part"])), e["cut"].get("keep", "rest"))
        if "anchor_in" in e:                                # a part placed as it sits in a whole picture
            ai = e["anchor_in"]
            ox, oy = _find_part(np.array(Image.open(src(ai["png"])).convert("RGBA")).astype(int),
                                np.array(sheet).astype(int))
            anchor = [ai["anchor"][0] - ox, ai["anchor"][1] - oy]
        scale = e.get("scale", 1)
        soft = e.get("aa", not e.get("seamless"))           # soft edges, not on ground blocks (they tile)
        if "spin" in e:                                     # frames name.0..n-1, turned about the hub
            sp = e["spin"]
            ims, h = spin_frames(sheet, sp["hub"], sp["plane"], sp.get("frames", 8), sp.get("turn", 90))
            at = [h[0] + anchor[0] - sp["hub"][0], h[1] + anchor[1] - sp["hub"][1]]
            for i, im in enumerate(ims):
                p.add_image(f"{e['name']}.{i}", im, at, scale, aa=soft)
        elif "grid" in e:
            gw, gh = e["grid"]
            cols = sheet.width // gw
            cells = e.get("cells", [[0, 0]])
            if cells == "all":                               # every cell, numbered row by row (empty ones too)
                cells = [[i % cols, i // cols] for i in range(cols * (sheet.height // gh))]
            for cx, cy in cells:
                name = e["name"] if len(cells) == 1 else f"{e['name']}.{cy * cols + cx}"
                p.add_image(name, sheet.crop((cx * gw, cy * gh, cx * gw + gw, cy * gh + gh)), anchor, scale,
                            aa=soft)
        else:
            p.add_image(e["name"], sheet, anchor, scale, aa=soft)

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
            n = e.get("frames", sheet.width // gw)
            sq = e.get("squash", 1)                          # flattened (a circle on the ground): height times sq
            anchor = [e["anchor"][0], e["anchor"][1] * sq]
            def cell(i, row):
                im = tint(sheet.crop((i * gw, row * gh, i * gw + gw, row * gh + gh)), e.get("tint"))
                return im if sq == 1 else im.resize((gw, max(1, round(gh * sq))), Image.NEAREST)
            for d, row in e["rows"].items():
                a["dirs"][d] = [p.add_image(f"{e['name']}.{d}.{i}", cell(i, row), anchor, scale, share=True,
                                            aa=e.get("aa", True)) for i in range(n)]
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
                anchor = spec.get("anchor", e.get("anchor"))
                body = e.get("body")                         # the base body's layers: hand and part map
                if e.get("kneel"):                           # a knock-out made from the tag's first frame
                    made = kneel_frames(ase, frames[0], body, d)
                    imgs = [m[0] for m in made]
                    extras = [(m[1], m[2]) for m in made]
                    n = len(imgs) if n is None else min(n, len(imgs))
                else:
                    imgs = [ase.frame(f, spec.get("layers", e.get("layers"))) for f in frames]
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

    names = {im["name"]: i for i, im in enumerate(p.images)}
    for e in r.get("tile_anims", []):
        if e["from"] not in names:
            sys.exit(f"{e['name']}: no image {e['from']}")
        if "frames" in e:                                   # the art's own frames, by image name
            missing = [f for f in e["frames"] if f not in names]
            if missing:
                sys.exit(f"{e['name']}: no image {', '.join(missing)}")
            idx = [names[f] for f in e["frames"]]
            p.anims.append({"name": e["name"], "dirs": {"se": idx}, "ms": e.get("ms", 200), "frames": len(idx)})
            continue
        i = names[e["from"]]
        base = p.images[i]
        if "whirl" in e:                                    # a part of it whirling (a portal's vortex)
            wh = e["whirl"]
            frames = whirl_part(base["img"], Image.open(src(wh["part"])), wh["plane"], wh.get("frames", 12),
                                wh.get("arms", 2), wh.get("twist", 2.5), wh.get("rim", 2))
            anchor = (-base["ox"], -base["oy"])
            idx = [p.add_image(f"{e['name']}~{k}", im, anchor) for k, im in enumerate(frames)]
            p.anims.append({"name": e["name"], "dirs": {"se": idx}, "ms": wh.get("ms", 80), "frames": len(idx)})
            continue
        if "turn" in e:                                     # a part of it turning (a portal's vortex)
            tn = e["turn"]
            frames = turn_part(base["img"], Image.open(src(tn["part"])), tn["plane"], tn.get("frames", 12),
                               tn.get("turn", 360))
            anchor = (-base["ox"], -base["oy"])
            idx = [p.add_image(f"{e['name']}~{k}", im, anchor) for k, im in enumerate(frames)]
            p.anims.append({"name": e["name"], "dirs": {"se": idx}, "ms": tn.get("ms", 90), "frames": len(idx)})
            continue
        rp = e["ripple"]
        frames = ripple(base["img"], rp.get("frames", 6), rp.get("period", 12), rp.get("keep", 2), rp.get("amp", 1.0),
                        rp.get("rows"))
        anchor = (-base["ox"], -base["oy"])                 # the image is already trimmed: same box, same anchor
        idx = [p.add_image(f"{e['name']}~{k}", im, anchor) for k, im in enumerate(frames)]
        p.anims.append({"name": e["name"], "dirs": {"se": idx}, "ms": rp.get("ms", 250), "frames": len(idx)})

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
        o += 48 if ver >= 3 else 44 if ver >= 2 else 36
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
