#!/usr/bin/env python3
"""FH1 3D models for art packs: simple solids (boxes, gable and hip roofs, cylinders, cones, flat panels for doors
and windows) rendered on the PC into the isometric view the games use, in a drawn style: flat colours by which way a
face looks, a dark outline wherever two faces meet and round the shape. A model is small and plain (JSON), so a
building of any shape is a few lines instead of a picture per shape; the pictures it makes go into the game's pack
(fh1art.py's "models").

  fh1model.py show MODEL.json OUT.png [--zoom N]      a model rendered (its palette, or grey), zoomed to look at
  fh1model.py sheet SPEC.json OUT.png                  a building spec's pictures (fh1art.py's "buildings"), one
                                                       colour, on a sheet

The view (game pixels): a cell's top face is TW x TH (48 x 28); a point (u, v, z) of the world (u, v in cells, z in
pixels up) lands at x = (u - v) * TW / 2, y = (u + v) * TH / 2 - z. Nearer the viewer is larger u + v along a line of
sight, so u + v is the depth.

A model (JSON):
  {"parts": [PART, ...], "palette": {MATERIAL: ["#light", "#dark"], ...}, "anchor": [u, v]}
  PARTS (u, v in cells, z in pixels; "mat" names a material, "cell" a colour group, below):
    {"box": [u0, v0, z0, u1, v1, z1]}                       a block
    {"gable": [u0, v0, u1, v1, z0, z1], "ridge": "u"|"v", "over": 0.05, "ends": MAT}
                                                            a roof from z0 (eaves) to z1 (ridge), its ridge along
                                                            u or v, overhanging by "over" cells; its ends (the
                                                            gables) in "ends"
    {"hip": [u0, v0, u1, v1, z0, z1], "over": 0.05}          a roof sloping on all four sides (a pyramid if square)
    {"cyl": [u, v, r, z0, z1], "seg": 24}                   an upright cylinder
    {"cone": [u, v, r, z0, z1], "seg": 24}                  a cone, its point at z1
    {"panel": [u, v, z0, z1, w], "face": "N"|"E"|"S"|"W"}  a flat rectangle on a wall facing that way, its middle
                                                            at (u, v), w cells wide: a door, a window
    {"poly": [[u, v, z], ...]}                              one flat face (a roof's odd piece)
    {"line": [[u, v, z], ...], "width": 1.5}                a line in the outline's colour drawn on the faces it
                                                            lies on (where nothing stands in front of it): a seam
  Colours: each material has a light tone (faces looking left-down on screen, +v, and tops) and a dark one (right-
  down, +u); roofs looking away from the viewer take the dark one; a third colour, if given, is for flat tops (a
  castle's walkway). The outline is OUTLINE.

Split by cells: render() also tells, for every pixel, which cell (whole u, v) its surface stands over, so a building
can be cut into one picture per cell (each in that cell's colours); the cut follows whole output pixels, so the
pieces fit edge to edge with no seam.
"""
import argparse
import json
import math
import sys

try:
    import numpy as np
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("fh1model.py needs numpy and Pillow (python3-numpy, python3-pil)")

TW, TH = 48, 28
SS = 4                                                   # supersampling: subpixels per pixel, each way
OUTLINE = "#2e222f"
EDGE = 5                                                 # the outline: this many subpixels each side of a boundary


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


# ── Solids into faces: (points [(u, v, z)], material, its tones' choice) ──

def _quad(a, b, c, d, mat):
    return [([a, b, c, d], mat)]


def box_faces(p, mat):
    u0, v0, z0, u1, v1, z1 = p
    P = lambda u, v, z: (u, v, z)
    return (_quad(P(u0, v0, z1), P(u1, v0, z1), P(u1, v1, z1), P(u0, v1, z1), mat) +      # top
            _quad(P(u0, v1, z0), P(u1, v1, z0), P(u1, v1, z1), P(u0, v1, z1), mat) +      # +v side
            _quad(P(u1, v0, z0), P(u1, v1, z0), P(u1, v1, z1), P(u1, v0, z1), mat) +      # +u side
            _quad(P(u0, v0, z0), P(u1, v0, z0), P(u1, v0, z1), P(u0, v0, z1), mat) +      # -v side
            _quad(P(u0, v0, z0), P(u0, v1, z0), P(u0, v1, z1), P(u0, v0, z1), mat))       # -u side


def gable_faces(p, ridge, over, mat, ends):
    u0, v0, u1, v1, z0, z1 = p
    if ridge == "u":                                     # slopes down to v0 and v1, gables at u0 and u1
        vm = (v0 + v1) / 2
        rise = (z1 - z0) / ((v1 - v0) / 2)
        e0, e1 = v0 - over, v1 + over                    # eaves, out over the walls and lower
        ze = z0 - over * rise
        a0, a1 = u0 - over, u1 + over
        return ([([(a0, e0, ze), (a1, e0, ze), (a1, vm, z1), (a0, vm, z1)], mat),
                 ([(a0, e1, ze), (a1, e1, ze), (a1, vm, z1), (a0, vm, z1)], mat),
                 ([(u0, v0, z0), (u0, v1, z0), (u0, vm, z1)], ends),
                 ([(u1, v0, z0), (u1, v1, z0), (u1, vm, z1)], ends)])
    um = (u0 + u1) / 2
    rise = (z1 - z0) / ((u1 - u0) / 2)
    e0, e1 = u0 - over, u1 + over
    ze = z0 - over * rise
    a0, a1 = v0 - over, v1 + over
    return ([([(e0, a0, ze), (e0, a1, ze), (um, a1, z1), (um, a0, z1)], mat),
             ([(e1, a0, ze), (e1, a1, ze), (um, a1, z1), (um, a0, z1)], mat),
             ([(u0, v0, z0), (u1, v0, z0), (um, v0, z1)], ends),
             ([(u0, v1, z0), (u1, v1, z0), (um, v1, z1)], ends)])


def hip_faces(p, over, mat):
    u0, v0, u1, v1, z0, z1 = p
    hu, hv = (u1 - u0) / 2, (v1 - v0) / 2
    h = min(hu, hv)
    rise = (z1 - z0) / h
    u0, v0, u1, v1 = u0 - over, v0 - over, u1 + over, v1 + over
    ze = z0 - over * rise
    um, vm = (u0 + u1) / 2, (v0 + v1) / 2
    k = min(hu, hv) + over                               # the ridge: what is left of the longer side
    r0, r1 = (u0 + k, u1 - k) if hu >= hv else (um, um)
    s0, s1 = (vm, vm) if hu >= hv else (v0 + k, v1 - k)
    A, B, C, D = (u0, v0, ze), (u1, v0, ze), (u1, v1, ze), (u0, v1, ze)
    R0, R1 = (r0, s0, z1), (r1, s1, z1)
    if hu >= hv:
        return [([A, B, R1, R0], mat), ([D, C, R1, R0], mat), ([A, D, R0], mat), ([B, C, R1], mat)]
    return [([A, D, R1, R0], mat), ([B, C, R1, R0], mat), ([A, B, R0], mat), ([D, C, R1], mat)]


def cyl_faces(p, seg, mat):
    u, v, r, z0, z1 = p
    pts = [(u + r * math.cos(2 * math.pi * i / seg), v + r * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
    out = [([(a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], z1), (a[0], a[1], z1)], mat + "~")
           for a, b in zip(pts, pts[1:] + pts[:1])]
    out.append(([(x, y, z1) for x, y in pts], mat))
    return out


def cone_faces(p, seg, mat):
    u, v, r, z0, z1 = p
    pts = [(u + r * math.cos(2 * math.pi * i / seg), v + r * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
    return [([(a[0], a[1], z0), (b[0], b[1], z0), (u, v, z1)], mat + "~") for a, b in zip(pts, pts[1:] + pts[:1])]


def panel_faces(p, face, mat):
    u, v, z0, z1, w = p
    d = 0.012                                            # a hair in front of its wall
    if face in ("N", "S"):
        vv = v + (d if face == "S" else -d)
        return [([(u - w / 2, vv, z0), (u + w / 2, vv, z0), (u + w / 2, vv, z1), (u - w / 2, vv, z1)], mat)]
    uu = u + (d if face == "E" else -d)
    return [([(uu, v - w / 2, z0), (uu, v + w / 2, z0), (uu, v + w / 2, z1), (uu, v - w / 2, z1)], mat)]


def faces_of(model):
    out = []
    for part in model["parts"]:
        mat, cell = part.get("mat", "wall"), part.get("cell")
        if "line" in part:
            continue
        if "box" in part:
            fs = box_faces(part["box"], mat)
        elif "gable" in part:
            fs = gable_faces(part["gable"], part.get("ridge", "u"), part.get("over", 0.05), mat, part.get("ends", "wall"))
        elif "hip" in part:
            fs = hip_faces(part["hip"], part.get("over", 0.05), mat)
        elif "cyl" in part:
            fs = cyl_faces(part["cyl"], part.get("seg", 24), mat)
        elif "cone" in part:
            fs = cone_faces(part["cone"], part.get("seg", 24), mat)
        elif "panel" in part:
            fs = panel_faces(part["panel"], part["face"], mat)
        elif "poly" in part:                                 # one flat face: [[u, v, z], ...]
            fs = [([tuple(q) for q in part["poly"]], mat)]
        elif "line" in part:                                 # drawn after, not a face
            continue
        else:
            sys.exit(f"unknown part: {part}")
        out += [(pts, m, cell) for pts, m in fs]
    return out


# ── Rendering ──

def _proj(p):
    u, v, z = p
    return ((u - v) * TW / 2, (u + v) * TH / 2 - z)


def _normal(pts):
    # Newell's method over the polygon (u, v in cells, z in pixels: z scaled to cells for the direction).
    nx = ny = nz = 0.0
    q = [(u, v, z / TH) for u, v, z in pts]
    for a, b in zip(q, q[1:] + q[:1]):
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])
    n = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
    if nx + ny + nz < 0:                                 # turned toward the viewer (+u, +v, up)
        n = -n
    return nx / n, ny / n, nz / n


def _tone(n, mat):
    """0 light, 1 dark. Walls: looking +v (left-down) light, +u (right-down) dark. Roofs (sloping up): light unless
    they look away (-u or -v). Round sides ("~"): by how much they look left or right."""
    nu, nv, nz = n
    if nz > 0.97:                                        # a flat top
        return 2
    if abs(nz) > 0.2:                                    # a roof slope
        return 1 if (nu < -0.15 or nv < -0.15) and nz < 0.95 else 0
    return 0 if nv >= nu else 1


def render(model, cells=None):
    """The model's buffers at SS x: per subpixel its face id, material, tone, cell index (or -1) and coverage.
    cells: [(cu, cv), ...] the cells to tell apart (else one). Returns a dict with the arrays and the origin
    (subpixel of world x = 0, y = 0)."""
    faces = faces_of(model)
    pts2 = [[_proj(p) for p in pts] for pts, _, _ in faces]
    xs = [x for f in pts2 for x, _ in f]
    ys = [y for f in pts2 for _, y in f]
    x0, y0 = math.floor(min(xs)) - 3, math.floor(min(ys)) - 3
    w, h = (math.ceil(max(xs)) + 3 - x0) * SS, (math.ceil(max(ys)) + 3 - y0) * SS
    depth = np.full((h, w), -1e9, np.float32)
    fid = np.full((h, w), -1, np.int32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    X = (xx + 0.5) / SS + x0
    Y = (yy + 0.5) / SS + y0
    planes = {}
    keys = []
    for i, ((pts, mat, cell), p2) in enumerate(zip(faces, pts2)):
        n = _normal(pts)
        # The depth (u + v) over the face as a plane in screen x, y: from its points.
        A = np.array([[x, y, 1.0] for x, y in p2])
        d = np.array([u + v for u, v, _ in pts])
        coef, *_ = np.linalg.lstsq(A, d, rcond=None)
        area = 0.5 * abs(sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(p2, p2[1:] + p2[:1])))
        if area < 0.05:
            keys.append(None)
            continue
        # The face's id for outlines: its plane and material (faces on one plane meet with no line between them).
        key = (round(n[0], 2), round(n[1], 2), round(n[2], 2), round(float(np.dot(n, pts[0][:2] + (pts[0][2] / TH,))), 2),
               mat.rstrip("~"))
        if mat.endswith("~"):
            key = ("round", mat.rstrip("~"))                 # a round side: one face, outlined only round it
        keys.append(key)
        planes.setdefault(key, len(planes))
        mask = Image.new("L", (w, h))
        ImageDraw.Draw(mask).polygon([((x - x0) * SS, (y - y0) * SS) for x, y in p2], fill=255)
        m = np.array(mask) > 0
        dep = (coef[0] * X + coef[1] * Y + coef[2]).astype(np.float32)
        win = m & (dep > depth + 1e-4)
        depth[win] = dep[win]
        fid[win] = i
    # Per subpixel: plane id, material, tone, and the cell its surface stands over.
    pid = np.full((h, w), -1, np.int32)
    mats = sorted({m.rstrip("~") for _, m, _ in faces})
    mid = np.full((h, w), -1, np.int32)
    tone = np.zeros((h, w), np.float32)
    for i, (pts, mat, cell) in enumerate(faces):
        sel = fid == i
        if not sel.any() or keys[i] is None:
            continue
        pid[sel] = planes[keys[i]]
        mid[sel] = mats.index(mat.rstrip("~"))
        n = _normal(pts)
        if mat.endswith("~"):                            # round: a soft turn from light to dark across it
            tone[sel] = 0.0 if n[1] - n[0] > -0.35 else 1.0
        else:
            tone[sel] = _tone(n, mat)
    cid = np.full((h, w), -1, np.int32)
    if cells:
        U = (depth + X / (TW / 2)) / 2
        V = (depth - X / (TW / 2)) / 2
        # A face on a cell's edge (a wall's side at u = 1) belongs to the cell it bounds: a hair back along its normal.
        nu = np.zeros((h, w), np.float32)
        nv = np.zeros((h, w), np.float32)
        for i, (pts, _, _) in enumerate(faces):
            sel = fid == i
            if sel.any():
                n = _normal(pts)
                nu[sel], nv[sel] = n[0], n[1]
        U = U - 2e-3 * nu
        V = V - 2e-3 * nv
        cu, cv = np.floor(U).astype(np.int32), np.floor(V).astype(np.int32)
        cid[fid >= 0] = -2                               # surface over another cell (a neighbour rendered round it)
        for k, (a, b) in enumerate(cells):
            cid[(fid >= 0) & (cu == a) & (cv == b)] = k
    # The outline: subpixels near a boundary between planes, or between the model and the empty.
    edge = np.zeros((h, w), bool)
    edge[:, 1:] |= pid[:, 1:] != pid[:, :-1]
    edge[:, :-1] |= pid[:, 1:] != pid[:, :-1]
    edge[1:, :] |= pid[1:, :] != pid[:-1, :]
    edge[:-1, :] |= pid[1:, :] != pid[:-1, :]
    edge = _dilate(edge, EDGE - 1)
    inside = pid >= 0
    # Lines: sampled along, each point drawn where the surface there is the nearest (its depth, u + v).
    for part in model["parts"]:
        if "line" not in part:
            continue
        rad = max(1, round(part.get("width", 1.5) * SS / 2))
        pts = part["line"]
        for a, b in zip(pts, pts[1:]):
            pa, pb = _proj(a), _proj(b)
            steps = max(2, int(math.hypot(pb[0] - pa[0], pb[1] - pa[1]) * SS * 2))
            for t in np.linspace(0, 1, steps):
                x = (pa[0] + (pb[0] - pa[0]) * t - x0) * SS
                y = (pa[1] + (pb[1] - pa[1]) * t - y0) * SS
                d = (a[0] + a[1]) + ((b[0] + b[1]) - (a[0] + a[1])) * t
                xi, yi = int(x), int(y)
                if not (0 <= xi < w and 0 <= yi < h) or depth[yi, xi] > d + 0.02:
                    continue
                edge[max(0, yi - rad):yi + rad, max(0, xi - rad):xi + rad] |= inside[max(0, yi - rad):yi + rad,
                                                                                   max(0, xi - rad):xi + rad]
    solid = inside | edge & _dilate(inside, EDGE)        # the outline reaches a little out of the shape
    if cells:
        cid = _fill_nearest(cid, solid, 3 * EDGE)        # the outline: to the surface beside it
    return {"mat": mid, "mats": mats, "tone": tone, "edge": edge, "solid": solid, "cell": cid,
            "origin": (-x0 * SS, -y0 * SS), "size": (w, h)}


def _dilate(m, r):
    out = m.copy()
    for _ in range(r):
        n = out.copy()
        n[1:, :] |= out[:-1, :]
        n[:-1, :] |= out[1:, :]
        n[:, 1:] |= out[:, :-1]
        n[:, :-1] |= out[:, 1:]
        out = n
    return out


def _fill_nearest(cid, where, steps):
    """Unknown cells (-1) where `where`, taken from a neighbour's (a cell, or -2: another's), step by step."""
    c = cid.copy()
    for _ in range(steps):
        miss = (c == -1) & where
        if not miss.any():
            break
        n = c.copy()
        for sl_to, sl_from in (((slice(1, None), slice(None)), (slice(None, -1), slice(None))),
                               ((slice(None, -1), slice(None)), (slice(1, None), slice(None))),
                               ((slice(None), slice(1, None)), (slice(None), slice(None, -1))),
                               ((slice(None), slice(None, -1)), (slice(None), slice(1, None)))):
            src = c[sl_from]
            dst = n[sl_to]
            take = (dst == -1) & (src != -1) & miss[sl_to]
            dst[take] = src[take]
        c = n
    return c


def colour(buf, palettes, default=("#b0b0b0", "#808080")):
    """The model's picture (RGBA, game pixels, soft edges): palettes[cell] (or palettes[0] for every cell) maps a
    material to its [light, dark] colours. Returns (image, origin in game pixels)."""
    w, h = buf["size"]
    out = np.zeros((h, w, 4), np.float32)
    line = rgb(OUTLINE)
    cells = buf["cell"]
    for k, pal in enumerate(palettes):
        sel_cell = (cells == k) if len(palettes) > 1 else np.ones((h, w), bool)
        for mi, name in enumerate(buf["mats"]):
            cs = [rgb(c) for c in pal.get(name, default)]
            for t, col in ((0, cs[0]), (1, cs[1]), (2, cs[2] if len(cs) > 2 else cs[0])):
                sel = sel_cell & (buf["mat"] == mi) & (buf["tone"] == t) & ~buf["edge"]
                out[sel, :3] = col
                out[sel, 3] = 255
    sel = buf["edge"] & buf["solid"]
    out[sel, :3] = line
    out[sel, 3] = 255
    # Down to game pixels: premultiplied, averaged over each SS x SS block; a pixel the model covers wholly takes
    # the colour most of its subpixels have (exact palette colours inside: a game can swap them as it draws), the
    # edge round it the average (soft, the outline's colour).
    pre = out.copy()
    pre[..., :3] *= pre[..., 3:4] / 255
    small = pre.reshape(h // SS, SS, w // SS, SS, 4).mean(axis=(1, 3))
    a = small[..., 3:4]
    small[..., :3] = np.where(a > 0, small[..., :3] * 255 / np.maximum(a, 1e-3), 0)
    key = (out[..., 0].astype(np.int64) << 16) | (out[..., 1].astype(np.int64) << 8) | out[..., 2].astype(np.int64)
    key[out[..., 3] == 0] = -1
    blocks = key.reshape(h // SS, SS, w // SS, SS).transpose(0, 2, 1, 3).reshape(h // SS, w // SS, SS * SS)
    best_n = np.zeros((h // SS, w // SS), np.int32)
    best = np.zeros((h // SS, w // SS), np.int64)
    for k in np.unique(key[key >= 0]):
        n = (blocks == k).sum(axis=2)
        take = n > best_n
        best[take] = k
        best_n[take] = n[take]
    whole = small[..., 3] >= 254.5
    small[whole, 0] = (best[whole] >> 16) & 255
    small[whole, 1] = (best[whole] >> 8) & 255
    small[whole, 2] = best[whole] & 255
    img = Image.fromarray(np.clip(small + 0.5, 0, 255).astype(np.uint8))
    return img, (buf["origin"][0] / SS, buf["origin"][1] / SS)


def cell_map(buf):
    """Which cell each game pixel belongs to: the one under its middle (every piece of a building rendered on its own
    agrees, so they share the pixels out exactly); at the soft edge, the most of its subpixels. -1 none, -2 another
    cell's (a neighbour rendered round it)."""
    w, h = buf["size"]
    c = buf["cell"].reshape(h // SS, SS, w // SS, SS).transpose(0, 2, 1, 3).reshape(h // SS, w // SS, SS * SS)
    out = c[:, :, (SS // 2) * SS + SS // 2].copy()
    edge = out == -1
    best = np.zeros((h // SS, w // SS), np.int32)
    for k in [-2] + list(range(int(buf["cell"].max()) + 1)):
        n = (c == k).sum(axis=2)
        take = edge & (n > best)
        out[take] = k
        best[take] = n[take]
    return out


def pieces(model, cells, palettes):
    """The model cut into one picture per cell, cell k in palettes[k]; each (image, origin) on the same canvas, so
    drawn at one place they make the whole model."""
    buf = render(model, cells)
    full, origin = colour(buf, palettes)
    cm = cell_map(buf)
    arr = np.array(full)
    out = []
    for k in range(len(cells)):
        a = arr.copy()
        a[cm != k] = 0
        out.append((Image.fromarray(a), origin))
    return out


# ── Buildings for a building game ──
#
# A joined building of any shape is drawn cell by cell, each cell's piece chosen from what is round it: its ridge's
# way ("u" along the screen's rows, "v" down its columns), which of its two sides along the ridge join (bit 0 the
# lower, bit 1 the higher), and what joins across (on the lower and the higher side: 0 nothing, 1 a parallel row,
# their roofs meeting in a valley, 2 a wing coming in, its roof crossing onto this one's). Each piece is rendered
# with a little of its neighbours round it (so no outline falls where the roof runs on) and cut to its own cell;
# drawn back to front they make the building. Doors, windows and a smithy's chimney are small pictures drawn on
# top. A 2 x 2 house of one kind is a building of its own (a bigger house, an upper storey on it), and so is a
# castle's 2 x 2 keep.

EXT = 0.25                                               # how far a piece is rendered into a joined neighbour


def _roof_u(alo, ahi, clo, chi, s):
    """A cell whose ridge runs along u, as parts (u, v in the cell, 0..1)."""
    i, j, o, E = s["inset"], 1 - s["inset"], s["over"], EXT
    ze, zr = s["wall"], s["wall"] + s["rise"]
    k = s["rise"] / (0.5 - i)                            # the slope: pixels up a cell across
    u0, u1 = (-E if alo else i), (1 + E if ahi else j)
    r0, r1 = (-E if alo else i - o), (1 + E if ahi else j + o)
    parts = [{"box": [u0, -E if clo == 1 else i, 0, u1, 1 + E if chi == 1 else j, ze], "mat": "wall"}]
    # The slopes: to an eave over the wall, or down to a valley at the edge and up the parallel row's slope.
    for side, c, edge, eave in ((0, clo, 0, i - o), (1, chi, 1, j + o)):
        if c == 1:
            parts.append({"poly": [[r0, 0.5, zr], [r1, 0.5, zr], [r1, edge, ze], [r0, edge, ze]], "mat": "roof"})
            far = -E if side == 0 else 1 + E
            parts.append({"poly": [[r0, edge, ze], [r1, edge, ze], [r1, far, ze + E * k], [r0, far, ze + E * k]],
                          "mat": "roof"})
        else:
            ze2 = ze - o * k
            parts.append({"poly": [[r0, 0.5, zr], [r1, 0.5, zr], [r1, eave, ze2], [r0, eave, ze2]], "mat": "roof"})
        if c == 2:                                       # a wing comes in: its walls and its roof to the ridge
            a, b = (-E, 0.5) if side == 0 else (0.5, 1 + E)
            parts.append({"box": [i, -E if side == 0 else j, 0, j, i if side == 0 else 1 + E, ze], "mat": "wall"})
            ze2 = ze - o * k
            parts.append({"poly": [[0.5, a, zr], [0.5, b, zr], [i - o, b, ze2], [i - o, a, ze2]], "mat": "roof"})
            parts.append({"poly": [[0.5, a, zr], [0.5, b, zr], [j + o, b, ze2], [j + o, a, ze2]], "mat": "roof"})
    # The gable ends where nothing joins along.
    vn, vs = (0 if clo == 1 else i), (1 if chi == 1 else j)
    if not alo:
        parts.append({"poly": [[i, vn, ze], [i, vs, ze], [i, 0.5, zr]], "mat": "wall"})
    if not ahi:
        parts.append({"poly": [[j, vn, ze], [j, vs, ze], [j, 0.5, zr]], "mat": "wall"})
    return parts


def _transpose(parts):
    """The same parts with u and v swapped (a ridge down the columns)."""
    out = []
    for p in parts:
        q = dict(p)
        if "box" in p:
            u0, v0, z0, u1, v1, z1 = p["box"]
            q["box"] = [v0, u0, z0, v1, u1, z1]
        if "poly" in p:
            q["poly"] = [[v, u, z] for u, v, z in p["poly"]]
        if "panel" in p:
            u, v, z0, z1, w = p["panel"]
            q["panel"] = [v, u, z0, z1, w]
            q["face"] = {"N": "W", "W": "N", "S": "E", "E": "S"}[p["face"]]
        out.append(q)
    return out


def cell_parts(kind, along, clo, chi, s):
    parts = _roof_u(along & 1, along >> 1, clo, chi, s)
    return parts if kind == "u" else _transpose(parts)


def big_house_parts(s, upper):
    """A 2 x 2 house of one kind: taller walls under a hip roof; upper: a storey on its middle, roofed again."""
    i, j = s["inset"], 2 - s["inset"]
    zw, zr = s["big_wall"], s["big_wall"] + s["big_rise"]
    parts = [{"box": [i, i, 0, j, j, zw], "mat": "wall"},
             {"hip": [i, i, j, j, zw, zr], "mat": "roof", "over": s["over"]},
             {"panel": [1.5, j, 0, 11, 0.24], "face": "S", "mat": "door"},
             {"panel": [0.5, j, 6, 12, 0.14], "face": "S", "mat": "window"},
             {"panel": [j, 0.5, 6, 12, 0.14], "face": "E", "mat": "window"},
             {"panel": [j, 1.5, 6, 12, 0.14], "face": "E", "mat": "window"}]
    if upper:
        a, b, z0, z1 = 0.62, 1.38, zw + 6, zw + 6 + s["wall"] + 4
        parts += [{"box": [a, a, z0, b, b, z1], "mat": "wall"},
                  {"hip": [a, a, b, b, z1, z1 + 12], "mat": "roof", "over": s["over"]},
                  {"panel": [1.0, b, z1 - 11, z1 - 4, 0.12], "face": "S", "mat": "window"},
                  {"panel": [b, 1.0, z1 - 11, z1 - 4, 0.12], "face": "E", "mat": "window"}]
    return parts


def keep_parts(s):
    """A castle's 2 x 2 keep: a block with a walkway inside its parapet, merlons round it."""
    h, p = s["keep_h"], 0.16
    parts = [{"box": [0, 0, 0, 2, 2, h], "mat": "castle"},
             {"box": [p, p, h, 2 - p, 2 - p, h + 0.01], "mat": "floor"}]
    n = 5
    for k in range(n):                                   # merlons along each side
        t = 0.06 + k * (2 - 0.12 - 0.22) / (n - 1)
        for u0, v0 in ((t, 0), (t, 2 - 0.2), (0, t), (2 - 0.2, t)):
            parts.append({"box": [u0, v0, h, u0 + 0.22 if v0 in (0, 2 - 0.2) else u0 + 0.2,
                                  v0 + 0.2 if v0 in (0, 2 - 0.2) else v0 + 0.22, h + 7], "mat": "castle"})
    parts.append({"panel": [1.5, 2, 0, 16, 0.34], "face": "S", "mat": "door"})
    return parts


# ── Castle walls and keeps: blocks the size of a cell joined edge to edge ──
#
# A wall cell's piece by the sides it joins on screen (mask: N 1, E 2, S 4, W 8): one block running on into its
# neighbours (no line where it joins), a parapet with merlons along its outer edges (the top level; a level under
# another has none), and where it joins to the right-down (E) or left-down (S) a seam drawn as a square wave on the
# face seen, like bricks laid into each other. A keep is four cells taller, its parapet round the outside; a wall
# against it is lower, so the keep's face runs on above the wall, the seam drawn up to the wall's height.

def _sq_wave(at, face, z0, z1, d, step):
    """A square wave up a face: on the S face (v = 1) near u = at, or the E face (u = 1) near v = at, stepping
    back by d (into the cell behind the seam) every step pixels."""
    pts, z, k = [], z0, 0
    while z < z1:
        a = at - (0.05 + d if k % 2 else 0.05)
        z2 = min(z1, z + step)
        if face == "S":
            pts += [[a, 0.999, z], [a, 0.999, z2]]
        else:
            pts += [[0.999, a, z], [0.999, a, z2]]
        z, k = z2, k + 1
    return {"line": pts, "width": 1.4}


def _merlons(edge, lo, hi, h, s):
    """Merlons along an edge ("N", "E", "S", "W") over [lo, hi] (cells along it), every half cell."""
    p, out = s["parapet"], []
    t = math.floor(lo * 2) / 2
    while t < hi:
        a, b = max(lo, t + 0.1), min(hi, t + 0.4)
        if b > a:
            if edge == "N": out.append({"box": [a, 0, h + 3, b, p, h + 9], "mat": "castle"})
            if edge == "S": out.append({"box": [a, 1 - p, h + 3, b, 1, h + 9], "mat": "castle"})
            if edge == "W": out.append({"box": [0, a, h + 3, p, b, h + 9], "mat": "castle"})
            if edge == "E": out.append({"box": [1 - p, a, h + 3, 1, b, h + 9], "mat": "castle"})
        t += 0.5
    return out


def castle_parts(join, lower, h, top, s):
    """A castle block's parts in its cell: join (N 1, E 2, S 4, W 8) the sides it runs on at its height, lower the
    sides a lower wall (height s["castle_h"]) stands against, h its height, top with its parapet."""
    E, p, hw = EXT, s["parapet"], s["castle_h"]
    u0, u1 = (-E if join & 8 else 0), (1 + E if join & 2 else 1)
    v0, v1 = (-E if join & 1 else 0), (1 + E if join & 4 else 1)
    parts = [{"box": [u0, v0, 0, u1, v1, h], "mat": "castle"}]
    for side, box in ((1, [0, -E, 0, 1, 0, hw]), (2, [1, 0, 0, 1 + E, 1, hw]), (4, [0, 1, 0, 1, 1 + E, hw]),
                      (8, [-E, 0, 0, 0, 1, hw])):
        if lower & side:
            parts.append({"box": box, "mat": "castle"})
    if top:                                              # the parapet and merlons along the outer edges
        for side, edge in ((1, "N"), (2, "E"), (4, "S"), (8, "W")):
            if join & side:
                continue
            # Along its own cell only: a neighbour may have no parapet on that line (an inner corner).
            if edge in ("N", "S"):
                lo, hi = 0, 1
                v_a, v_b = (0, p) if edge == "N" else (1 - p, 1)
                parts.append({"box": [lo, v_a, h, hi, v_b, h + 3], "mat": "castle"})
            else:
                lo, hi = 0, 1
                u_a, u_b = (0, p) if edge == "W" else (1 - p, 1)
                parts.append({"box": [u_a, lo, h, u_b, hi, h + 3], "mat": "castle"})
            parts += _merlons(edge, lo, hi, h, s)
    # Seams where it runs on to the front: up the face seen.
    zt = h + (3 if top else 0)
    if join & 2 and not join & 4:
        parts.append(_sq_wave(1.0, "S", 0, zt, 0.12, 5))
    if join & 4 and not join & 2:
        parts.append(_sq_wave(1.0, "E", 0, zt, 0.12, 5))
    if lower & 2 and not join & 4:                       # a lower wall against it, in front: up to its height
        parts.append(_sq_wave(1.0, "S", 0, hw, 0.12, 5))
    if lower & 4 and not join & 2:
        parts.append(_sq_wave(1.0, "E", 0, hw, 0.12, 5))
    return parts


def castle_set(spec):
    """A castle's pictures: [(name, image, anchor), ...] in the castle colours (spec["castle"]: light, dark, top).
      wl<mask>.<t|b>   a wall cell joined on mask's sides (screen: N 1, E 2, S 4, W 8), t its top level, b a level
                       under another
      kp<piece>.<ext>  a keep's cell (piece du + 2 dv in its 2 x 2 on screen), ext: lower walls against its two
                       outer sides (bit 0 its N or S side, bit 1 its W or E side)
      gS, gE, lS, lE   a gate's arch, a window slit on the S or E face seen; kdS the keep's door
      st<d>            stairs up to a wall's walkway, climbing toward side d (screen: 0 N, 1 E, 2 S, 3 W)"""
    out = []
    pal = {"castle": spec["castle"], "door": spec["door"], "window": spec["window"]}
    hw, hk = spec["castle_h"], spec["keep_h"]

    def one(name, parts):
        buf = render({"parts": parts}, [(0, 0)])
        img, _ = colour(buf, [pal])
        a = np.array(img)
        a[cell_map(buf) != 0] = 0
        out.append((name, Image.fromarray(a), _anchor_px(buf, (0, 0))))

    for mask in range(16):
        one(f"wl{mask}.t", castle_parts(mask, 0, hw, True, spec))
        one(f"wl{mask}.b", castle_parts(mask, 0, hw, False, spec))
    for piece in range(4):
        du, dv = piece & 1, piece >> 1
        inner = (2 if du == 0 else 8) | (4 if dv == 0 else 1)
        outer_a, outer_b = (1 if dv == 0 else 4), (8 if du == 0 else 2)
        for ext in range(4):
            lower = (outer_a if ext & 1 else 0) | (outer_b if ext & 2 else 0)
            one(f"kp{piece}.{ext}", castle_parts(inner, lower, hk, True, spec))
    # Stairs up to a wall's walkway, climbing toward one side (screen: 0 N, 1 E, 2 S, 3 W): steps across the
    # cell's middle, the highest against the wall.
    steps = 6
    for d in range(4):
        parts = []
        for k in range(steps):
            a0, z = k / steps, (k + 1) * hw / steps
            box = [a0, 0.18, 0, 1, 0.82, z]                  # climbing toward +u (E)
            if d == 3:
                box = [0, 0.18, 0, 1 - a0, 0.82, z]
            if d in (0, 2):                                  # down the columns: u and v swapped
                box = [box[1], box[0], 0, box[4], box[3], z] if d == 2 else [0.18, 0, 0, 0.82, 1 - a0, z]
            parts.append({"box": box, "mat": "castle"})
        one(f"st{d}", parts)
        if d == 3:                                       # the bar's icon: the steps seen climbing left
            img = out[-1][1]
            box = img.getchannel("A").getbbox()
            k = 26 / max(box[2] - box[0], box[3] - box[1])
            ic = img.crop(box).resize((max(1, round((box[2] - box[0]) * k)), max(1, round((box[3] - box[1]) * k))), Image.BOX)
            out.append(("ic.stair", ic, (ic.width / 2, ic.height / 2)))
    for name, part in {"gS": {"panel": [0.5, 1, 0, 15, 0.34], "face": "S", "mat": "door"},
                       "gE": {"panel": [1, 0.5, 0, 15, 0.34], "face": "E", "mat": "door"},
                       "lS": {"panel": [0.5, 1, 11, 18, 0.07], "face": "S", "mat": "window"},
                       "lE": {"panel": [1, 0.5, 11, 18, 0.07], "face": "E", "mat": "window"},
                       "kdS": {"panel": [0.5, 1, 0, 16, 0.32], "face": "S", "mat": "door"}}.items():
        buf = render({"parts": [part]})
        img, _ = colour(buf, [pal])
        out.append((name, img, _anchor_px(buf, (0, 0))))
    return out


# ── A flag: a pole and a cloth waving in the wind, frames to loop ──

def flag_parts(phase, s):
    """A flag's parts for one moment (phase 0..1 of its wave): a pole up from the cell's middle, the cloth from its
    top along u, rippling out of its plane more the further from the pole."""
    pole, w, hgt, n = s["pole"], s["width"], s["height"], 8
    parts = [{"box": [0.48, 0.48, 0, 0.52, 0.52, pole], "mat": "pole"}]
    pts = []
    for k in range(n + 1):
        t = k / n
        u = 0.52 + w * t
        v = 0.5 + s["wave"] * t * math.sin(2 * math.pi * (t * 1.2 - phase))
        z = -1.5 * t * t                                 # its tip droops a little
        pts.append((u, v, z))
    for (u0, v0, z0), (u1, v1, z1) in zip(pts, pts[1:]):
        parts.append({"poly": [[u0, v0, pole - hgt + z0], [u1, v1, pole - hgt + z1], [u1, v1, pole + z1],
                               [u0, v0, pole + z0]], "mat": "flag~"})   # one cloth: outlined only round it
    return parts


def flag_set(spec, name="flag"):
    """A waving flag's frames, name.0 .. name.<n-1> (anchored at the pole's foot), in reference colours, and name.key
    (two pixels: the cloth's light and dark) so a game draws it in any colours (fart_map, the engine's flag.h).
    spec: "frames", "pole", "width" (cells), "height", "wave" (cells), "cloth" ([light, dark] reference), "pole_c"."""
    out = []
    pal = {"flag": spec["cloth"], "pole": spec["pole_c"]}
    for k in range(spec["frames"]):
        buf = render({"parts": flag_parts(k / spec["frames"], spec)})
        img, _ = colour(buf, [pal])
        out.append((f"{name}.{k}", img, _anchor_px(buf, (0, 0))))
    key = Image.new("RGBA", (2, 1))
    for x, c in enumerate(spec["cloth"][:2]):
        key.putpixel((x, 0), rgb(c) + (255,))
    out.append((f"{name}.key", key, (0, 0)))
    return out


def _anchor_px(buf, front):
    x, y = _proj((front[0] + 0.5, front[1] + 0.5, 0))
    return (buf["origin"][0] / SS + x, buf["origin"][1] / SS + y)


def building_set(spec):
    """Every picture for a building game's houses, smithies and keeps: [(name, image, anchor), ...], each drawn once
    in the reference colours ("walls" "o" and "roofs" "b"); a game swaps those four (the walls' light and dark, the
    roof's) for a cell's own as it draws, from the palettes: pal.<walls><roof> and pal.key, pictures of four pixels
    in that order. spec: "walls" and "roofs" ({code: [light, dark]}), "door", "window", "stone", "castle" ([light,
    dark, top]), sizes ("inset", "over" in cells; "wall", "rise", "big_wall", "big_rise", "keep_h", "chimney" in
    pixels). Names:
      c<kind><along><across lo><across hi>   a cell's piece (anchored at the cell's middle)
      d<S|E>, w<S|E>, chimney                a door, a window on the wall seen that way; a chimney
      big<0|1>                               a 2 x 2 house (1: with its upper storey), at its front cell
      keep                                   a castle's 2 x 2 keep, at its front cell
    and ic.house.<walls>.<roof> icons (26 pixels, in their colours) when spec has "icons"."""
    out = []
    base = {"door": spec["door"], "window": spec["window"], "stone": spec["stone"], "castle": spec["castle"],
            "floor": spec["castle"][2:] * 2}
    ref = dict(base, wall=spec["walls"]["o"], roof=spec["roofs"]["b"])

    def one(name, parts, front, cells=None):
        buf = render({"parts": parts}, cells)
        img, _ = colour(buf, [ref])
        if cells:
            a = np.array(img)
            a[cell_map(buf) != 0] = 0
            img = Image.fromarray(a)
        out.append((name, img, _anchor_px(buf, front)))
        return buf

    for kind in ("u", "v"):
        for along in range(4):
            for clo in range(3):
                for chi in range(3):
                    one(f"c{kind}{along}{clo}{chi}", cell_parts(kind, along, clo, chi, spec), (0, 0), [(0, 0)])
    j = 1 - spec["inset"]
    for name, part in {"dS": {"panel": [0.5, j, 0, 9, 0.19], "face": "S", "mat": "door"},
                       "dE": {"panel": [j, 0.5, 0, 9, 0.19], "face": "E", "mat": "door"},
                       "wS": {"panel": [0.5, j, 4, 9, 0.13], "face": "S", "mat": "window"},
                       "wE": {"panel": [j, 0.5, 4, 9, 0.13], "face": "E", "mat": "window"},
                       "chimney": {"box": [0.4, 0.4, spec["wall"] + spec["rise"] - 4, 0.6, 0.6, spec["chimney"]],
                                   "mat": "stone"}}.items():
        one(name, [part], (0, 0))
    for upper in (0, 1):
        one(f"big{upper}", big_house_parts(spec, upper), (1, 1))
    for wc, wall in spec["walls"].items():                # the palettes: four pixels each
        for rc, roof in spec["roofs"].items():
            out.append((f"pal.{wc}{rc}", Image.new("RGBA", (4, 1)), (0, 0)))
            for x, c in enumerate(wall[:2] + roof[:2]):
                out[-1][1].putpixel((x, 0), rgb(c) + (255,))
    out.append(("pal.key", Image.new("RGBA", (4, 1)), (0, 0)))
    for x, c in enumerate(spec["walls"]["o"][:2] + spec["roofs"]["b"][:2]):
        out[-1][1].putpixel((x, 0), rgb(c) + (255,))
    if spec.get("icons"):                                 # the bar's houses, in their own colours
        names = {"o": "wood", "w": "white", "b": "blue", "g": "green", "r": "red"}
        buf = render({"parts": cell_parts("u", 0, 0, 0, spec) + [
            {"panel": [j, 0.5, 0, 9, 0.19], "face": "E", "mat": "door"}]}, [(0, 0)])
        for wc, wall in spec["walls"].items():
            for rc, roof in spec["roofs"].items():
                img, _ = colour(buf, [dict(base, wall=wall, roof=roof)])
                box = img.getchannel("A").getbbox()
                s = 26 / max(box[2] - box[0], box[3] - box[1])
                img = img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.BOX)
                out.append((f"ic.house.{names[wc]}.{names[rc]}", img, (img.width / 2, img.height / 2)))
                if wc == "o":
                    sb = render({"parts": cell_parts("u", 0, 0, 0, spec) + [
                        {"panel": [j, 0.5, 0, 9, 0.19], "face": "E", "mat": "door"},
                        {"box": [0.4, 0.4, spec["wall"] + spec["rise"] - 4, 0.6, 0.6, spec["chimney"]], "mat": "stone"}]},
                        [(0, 0)])
                    si, _ = colour(sb, [dict(base, wall=wall, roof=roof)])
                    bx = si.getchannel("A").getbbox()
                    t = 26 / max(bx[2] - bx[0], bx[3] - bx[1])
                    si = si.resize((max(1, round(si.width * t)), max(1, round(si.height * t))), Image.BOX)
                    out.append((f"ic.smith.{names[rc]}", si, (si.width / 2, si.height / 2)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show")
    s.add_argument("model")
    s.add_argument("out")
    s.add_argument("--zoom", type=int, default=4)
    s = sub.add_parser("sheet")
    s.add_argument("spec")
    s.add_argument("out")
    a = ap.parse_args()
    if a.cmd == "sheet":
        spec = json.load(open(a.spec))
        spec = spec.get("buildings", spec)
        pics = {n: (img, anc) for n, img, anc in building_set(spec)}
        names = sorted(n for n in pics if not n.startswith(("pal.", "ic.")))
        cw, chh, cols = 90, 100, 10
        sheet = Image.new("RGBA", (cols * cw, ((len(names) + cols - 1) // cols) * chh), (140, 190, 100, 255))
        for n, name in enumerate(names):
            img, anc = pics[name]
            x, y = (n % cols) * cw + cw // 2, (n // cols) * chh + chh - 30
            sheet.alpha_composite(img, (round(x - anc[0]), round(y - anc[1])))
            ImageDraw.Draw(sheet).text(((n % cols) * cw + 3, (n // cols) * chh + 2), name, fill=(0, 0, 0))
        sheet.save(a.out)
        print(f"{a.out}: {len(pics)} pictures")
        return
    model = json.load(open(a.model))
    img, _ = colour(render(model), [model.get("palette", {})])
    bg = Image.new("RGBA", img.size, (140, 190, 100, 255))
    bg.alpha_composite(img)
    bg.resize((img.width * a.zoom, img.height * a.zoom), Image.NEAREST).save(a.out)
    print(f"{a.out}: {img.width} x {img.height}")


if __name__ == "__main__":
    main()
