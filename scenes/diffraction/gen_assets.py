# SPDX-License-Identifier: Apache-2.0
#
# Generate the test assets for scenes/diffraction:
#   cd-annulus.ply  - flat annulus (CD) with +Z normals and radial UVs
#   light-ball.ply  - small emissive sphere used as the finite light source
#
# Run from the repo root:
#   python3 scenes/diffraction/gen_assets.py

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def write_ply(path, verts, uvs, faces):
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\n")
        f.write(f"element vertex {len(verts)}\n")
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("property float nx\nproperty float ny\nproperty float nz\n")
        f.write("property float s\nproperty float t\n")
        f.write(f"element face {len(faces)}\n")
        f.write("property list uchar int vertex_indices\n")
        f.write("end_header\n")
        for (v, uv) in zip(verts, uvs):
            f.write(f"{v[0]:.8f} {v[1]:.8f} {v[2]:.8f} 0 0 1 {uv[0]:.8f} {uv[1]:.8f}\n")
        for fa in faces:
            f.write(f"{len(fa)} " + " ".join(str(i) for i in fa) + "\n")


def gen_annulus():
    # Real CD: outer radius 60mm, hub hole 7.5mm; units are meters
    n_seg, inner, outer = 128, 0.0075, 0.06
    verts, uvs, faces = [], [], []
    for i in range(n_seg):
        a = 2 * math.pi * i / n_seg
        ca, sa = math.cos(a), math.sin(a)
        verts.append((outer * ca, outer * sa, 0.0))
        verts.append((inner * ca, inner * sa, 0.0))
        # UV maps the disc bounding box to [0.05, 0.95]
        uvs.append((0.5 + ca * 0.45, 0.5 + sa * 0.45))
        uvs.append((0.5 + ca * 0.45 * inner / outer, 0.5 + sa * 0.45 * inner / outer))
    for i in range(n_seg):
        j = (i + 1) % n_seg
        o0, i0, o1, i1 = 2 * i, 2 * i + 1, 2 * j, 2 * j + 1
        faces.append((o0, o1, i1))
        faces.append((o0, i1, i0))
    write_ply(os.path.join(HERE, "cd-annulus.ply"), verts, uvs, faces)


def gen_ball():
    # Small UV sphere (radius set to 0.008m) for the emissive light
    n_seg, n_ring, r = 32, 16, 0.008
    verts, uvs, faces = [], [], []
    for ri in range(n_ring + 1):
        phi = math.pi * ri / n_ring
        sp, cp = math.sin(phi), math.cos(phi)
        for si in range(n_seg):
            th = 2 * math.pi * si / n_seg
            verts.append((r * sp * math.cos(th), r * sp * math.sin(th), r * cp))
            uvs.append((si / n_seg, ri / n_ring))
    for ri in range(n_ring):
        for si in range(n_seg):
            sj = (si + 1) % n_seg
            a = ri * n_seg + si
            b = ri * n_seg + sj
            c = (ri + 1) * n_seg + sj
            d = (ri + 1) * n_seg + si
            faces.append((a, c, d))
            faces.append((a, b, c))
    # Per-vertex normals are written as +Z by write_ply; override below
    path = os.path.join(HERE, "light-ball.ply")
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\n")
        f.write(f"element vertex {len(verts)}\n")
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("property float nx\nproperty float ny\nproperty float nz\n")
        f.write("property float s\nproperty float t\n")
        f.write(f"element face {len(faces)}\n")
        f.write("property list uchar int vertex_indices\n")
        f.write("end_header\n")
        for (v, uv) in zip(verts, uvs):
            n = (v[0] / r, v[1] / r, v[2] / r)
            f.write(f"{v[0]:.8f} {v[1]:.8f} {v[2]:.8f} {n[0]:.6f} {n[1]:.6f} {n[2]:.6f} {uv[0]:.8f} {uv[1]:.8f}\n")
        for fa in faces:
            f.write(f"{len(fa)} " + " ".join(str(i) for i in fa) + "\n")


def gen_env():
    # Studio HDR environment (equirect PFM): dark base + broad softbox +
    # compact hot key spot + cool accent. The delta-lobed grating smears
    # this into wavelength-shifted rainbows on the disc.
    import struct
    w, h = 1024, 512
    px = []
    for j in range(h):
        v = (j + 0.5) / h            # 0 bottom .. 1 top
        for i in range(w):
            u = (i + 0.5) / w
            # studio base: smooth overcast gradient (silvery sheen)
            r = 0.07 + 0.20 * v
            g = 0.07 + 0.23 * v
            b = 0.08 + 0.28 * v

            def gauss(u0, v0, su, sv):
                du = min(abs(u - u0), 1.0 - abs(u - u0))
                dv = v - v0
                return math.exp(-0.5 * ((du / su) ** 2 + (dv / sv) ** 2))

            # broad warm softbox (upper side) -> base sheen
            s = gauss(0.30, 0.70, 0.10, 0.06)
            r += 7.0 * s; g += 6.2 * s; b += 5.2 * s
            # hot compact key spot near the mirror azimuth -> rings on-disc
            k = gauss(0.62, 0.52, 0.018, 0.024)
            r += 160.0 * k; g += 125.0 * k; b += 80.0 * k
            # cool accent on the opposite side
            c = gauss(0.13, 0.45, 0.02, 0.03)
            r += 15.0 * c; g += 24.0 * c; b += 40.0 * c
            # two thin vertical window strips -> parallel rainbow slashes
            for wu in (0.50, 0.42):
                w_ = gauss(wu, 0.60, 0.005, 0.14)
                r += 20.0 * w_; g += 18.5 * w_; b += 16.0 * w_
            px.append((r, g, b))
    path = os.path.join(HERE, "studio-env.pfm")
    with open(path, "wb") as f:
        f.write(b"PF\n%d %d\n-1.0\n" % (w, h))
        for j in range(h - 1, -1, -1):          # PFM rows are bottom-up
            for i in range(w):
                f.write(struct.pack("<fff", *px[j * w + i]))


if __name__ == "__main__":
    gen_annulus()
    gen_ball()
    gen_env()
    print("wrote cd-annulus.ply + light-ball.ply + studio-env.pfm")
