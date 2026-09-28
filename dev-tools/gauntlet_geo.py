# SPDX-License-Identifier: Apache-2.0
#
# Gauntlet geometry generator: writes ASCII PLY meshes (Z-up, vertex
# normals) for the scenes/gauntlet benchmark suite. Run from repo root:
#
#   python3.13 dev-tools/gauntlet_geo.py
#
# Regenerates scenes/gauntlet/mesh/*.ply deterministically.

import math
from pathlib import Path

import numpy as np

MESH_DIR = Path(__file__).resolve().parent.parent / "scenes/gauntlet/mesh"


def write_ply(path, verts, normals, faces):
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\n")
        f.write(f"element vertex {len(verts)}\n")
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("property float nx\nproperty float ny\nproperty float nz\n")
        f.write(f"element face {len(faces)}\n")
        f.write("property list uchar uint vertex_indices\n")
        f.write("end_header\n")
        for v, n in zip(verts, normals):
            f.write(f"{v[0]:.6f} {v[1]:.6f} {v[2]:.6f} "
                    f"{n[0]:.6f} {n[1]:.6f} {n[2]:.6f}\n")
        for fc in faces:
            f.write(f"{len(fc)} " + " ".join(str(i) for i in fc) + "\n")
    print(f"wrote {path} ({len(verts)}v {len(faces)}f)")


def triangulate(faces):
    out = []
    for fc in faces:
        for i in range(1, len(fc) - 1):
            out.append((fc[0], fc[i], fc[i + 1]))
    return out


def smooth_normals(verts, faces):
    n = np.zeros_like(verts)
    for a, b, c in faces:
        fn = np.cross(verts[b] - verts[a], verts[c] - verts[a])
        n[a] += fn; n[b] += fn; n[c] += fn
    l = np.linalg.norm(n, axis=1)
    l[l == 0] = 1.0
    return n / l[:, None]


def flat_mesh(verts, faces):
    """Duplicate vertices per face -> faceted look."""
    verts = np.asarray(verts, float)
    fv, fn, ff = [], [], []
    for fc in triangulate(faces):
        a, b, c = (verts[i] for i in fc)
        n = np.cross(b - a, c - a)
        n /= max(np.linalg.norm(n), 1e-12)
        base = len(fv)
        fv += [tuple(a), tuple(b), tuple(c)]
        fn += [tuple(n)] * 3
        ff.append((base, base + 1, base + 2))
    return fv, fn, ff


def uv_sphere(r, nu=64, nv=32, center=(0, 0, 0)):
    verts, faces = [], []
    for j in range(nv + 1):
        ph = math.pi * j / nv
        for i in range(nu):
            th = 2 * math.pi * i / nu
            verts.append((center[0] + r * math.sin(ph) * math.cos(th),
                          center[1] + r * math.sin(ph) * math.sin(th),
                          center[2] + r * math.cos(ph)))
    for j in range(nv):
        for i in range(nu):
            a = j * nu + i
            b = j * nu + (i + 1) % nu
            faces.append((a, b, b + nu, a + nu))
    verts = np.asarray(verts)
    return verts, smooth_normals(verts, triangulate(faces)), faces


def torus(R, r, nu=96, nv=24, center=(0, 0, 0)):
    verts, faces = [], []
    for j in range(nv):
        v = 2 * math.pi * j / nv
        for i in range(nu):
            u = 2 * math.pi * i / nu
            verts.append((center[0] + (R + r * math.cos(v)) * math.cos(u),
                          center[1] + (R + r * math.cos(v)) * math.sin(u),
                          center[2] + r * math.sin(v)))
    for j in range(nv):
        for i in range(nu):
            a = j * nu + i
            b = j * nu + (i + 1) % nu
            c = ((j + 1) % nv) * nu + (i + 1) % nu
            d = ((j + 1) % nv) * nu + i
            faces.append((a, b, c, d))
    verts = np.asarray(verts)
    return verts, smooth_normals(verts, triangulate(faces)), faces


def prism(r, h, center=(0, 0, 0), sides=3):
    """Regular polygonal prism standing on z (flat-shaded)."""
    verts = []
    for z in (0.0, h):
        for i in range(sides):
            a = 2 * math.pi * i / sides + math.pi / sides
            verts.append((center[0] + r * math.cos(a),
                          center[1] + r * math.sin(a),
                          center[2] + z))
    faces = [tuple(range(sides - 1, -1, -1)),
             tuple(range(sides, 2 * sides))]
    for i in range(sides):
        j = (i + 1) % sides
        faces.append((i, j, j + sides, i + sides))
    return flat_mesh(verts, faces)


def diamond(r, h, center=(0, 0, 0), sides=8):
    """Faceted gem: two stacked pyramids (pavilion+crown)."""
    verts = [(center[0], center[1], center[2] - h * 0.55),
             (center[0], center[1], center[2] + h * 0.45)]
    for i in range(sides):
        a = 2 * math.pi * i / sides
        verts.append((center[0] + r * math.cos(a),
                      center[1] + r * math.sin(a), center[2]))
    faces = []
    for i in range(sides):
        j = (i + 1) % sides
        faces.append((0, 2 + j, 2 + i))
        faces.append((1, 2 + i, 2 + j))
    return flat_mesh(verts, faces)


def box(mn, mx, flip=False):
    """Axis-aligned box; flip=True gives inward-facing walls (room shell)."""
    x0, y0, z0 = mn; x1, y1, z1 = mx
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
         (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    if flip:
        f = [tuple(reversed(fc)) for fc in f]
    fv, fn, ff = flat_mesh(v, f)
    return np.asarray(fv), np.asarray(fn), ff


def room_with_windows(mn, mx, windows, win_wall="x0"):
    """Room shell with rectangular window holes on one wall.

    windows: list of (y0, y1, z0, z1) on the wall plane.
    Simplified: builds the perforated wall as a quad strip grid.
    """
    x0, y0, z0 = mn; x1, y1, z1 = mx
    verts, faces = [], []

    def add(quad):
        b = len(verts)
        verts.extend(quad)
        faces.append((b, b + 1, b + 2, b + 3))

    # inward-facing walls; floor is a SEPARATE mesh (own material)
    add([(x0, y0, z1), (x0, y1, z1), (x1, y1, z1), (x1, y0, z1)])  # ceil -z
    add([(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)])  # +x wall -x
    add([(x0, y0, z0), (x0, y0, z1), (x1, y0, z1), (x1, y0, z0)])  # y0 wall +y
    add([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)])  # y1 wall -y
    # left wall (x0) with window holes: build per-window segments
    # wall is the x=x0 plane spanning y0..y1, z0..z1
    segs_y = sorted([y0] + [w[0] for w in windows] +
                    [w[1] for w in windows] + [y1])
    # vertical strips between window bands
    yc = y0
    spans = []
    for w in sorted(windows):
        if w[0] > yc:
            spans.append((yc, w[0], z0, z1))
        spans.append((w[0], w[1], w[2], w[3]))  # hole region (filled below)
        yc = max(yc, w[1])
    if yc < y1:
        spans.append((yc, y1, z0, z1))
    for sy0, sy1, sz0, sz1 in spans:
        hole = (sz0 > z0) or (sz1 < z1)
        if not hole:
            add([(x0, sy0, z0), (x0, sy1, z0),
                 (x0, sy1, z1), (x0, sy0, z1)])  # solid strip (+x normal)
        else:
            if sz0 > z0:  # sill below window
                add([(x0, sy0, z0), (x0, sy1, z0),
                     (x0, sy1, sz0), (x0, sy0, sz0)])
            if sz1 < z1:  # lintel above window
                add([(x0, sy0, sz1), (x0, sy1, sz1),
                     (x0, sy1, z1), (x0, sy0, z1)])
    fv, fn, ff = flat_mesh(verts, faces)
    return np.asarray(fv), np.asarray(fn), ff


def main():
    MESH_DIR.mkdir(parents=True, exist_ok=True)

    # Gallery room 14(x) x 16(y) x 7(z), camera near y=0 looking +Y.
    # Two tall windows on the left wall (x=x0).
    v, n, f = room_with_windows((-7, -1, 0), (7, 15, 7),
                                windows=[(3.0, 5.0, 1.0, 6.2),
                                         (8.0, 10.0, 1.0, 6.2)])
    write_ply(MESH_DIR / "gauntlet_room.ply", v, n, f)

    # Solid sun-blocking wall segment is part of room; outside sun free.

    v, n, f = prism(1.1, 3.4)
    write_ply(MESH_DIR / "prism_a.ply", v, n, f)
    v, n, f = prism(0.85, 2.6)
    write_ply(MESH_DIR / "prism_b.ply", v, n, f)

    v, n, f = uv_sphere(1.05)
    write_ply(MESH_DIR / "orb.ply", v, n, f)
    v, n, f = uv_sphere(0.6)
    write_ply(MESH_DIR / "orb_small.ply", v, n, f)

    v, n, f = diamond(0.9, 1.6, sides=8)
    write_ply(MESH_DIR / "gem.ply", v, n, f)

    v, n, f = torus(1.3, 0.42)
    write_ply(MESH_DIR / "ring.ply", v, n, f)

    v, n, f = box((-0.9, -0.9, 0), (0.9, 0.9, 1.2))
    write_ply(MESH_DIR / "pedestal.ply", v, n, f)
    v, n, f = box((-0.7, -0.7, 0), (0.7, 0.7, 0.9))
    write_ply(MESH_DIR / "pedestal_s.ply", v, n, f)

    # thin vertical hanging plate for the suspended gem (string)
    v, n, f = box((-0.02, -0.02, 0), (0.02, 0.02, 1.0))
    write_ply(MESH_DIR / "rod.ply", v, n, f)

    # room floor plane (separate material from walls)
    v, n, f = flat_mesh([(-7, -1, 0), (7, -1, 0), (7, 15, 0), (-7, 15, 0)],
                        [(0, 1, 2, 3)])
    write_ply(MESH_DIR / "floor_plane.ply", v, n, f)

    # interior fog volume box (slightly inside the room shell)
    v, n, f = box((-6.9, -0.9, 0.01), (6.9, 14.9, 6.9))
    write_ply(MESH_DIR / "fogbox.ply", v, n, f)


if __name__ == "__main__":
    main()
