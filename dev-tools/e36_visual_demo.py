# SPDX-License-Identifier: Apache-2.0
#
# E36 visual demo: a lock of curved hair strands rendered with the Huang
# model (left: eumelanin brunette, right: pheomelanin blonde) against a
# dark backdrop with a warm rim light. 1280x720, CPU + GPU.
#
#   python3.13 dev-tools/e36_visual_demo.py [cpu|gpu|both]

import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 1280, 720
SPP = 256
OCL_DEV = os.environ.get("E36_OCL_DEV", "10")
HAIR_FILE = Path("/tmp/e36_lock.hair")


def write_lock(path, n=1400, segs=12, seed=7):
    """A falling lock of curved strands: root cluster at top, strands
    arc outward with per-strand curl and jitter."""
    rng = np.random.default_rng(seed)
    strands = []
    for _ in range(n):
        # root position inside a small elliptical scalp patch
        a = rng.uniform(0, 2 * np.pi)
        r = np.sqrt(rng.uniform(0, 1))
        x0 = 0.4 * r * np.cos(a)
        z0 = 0.22 * r * np.sin(a)
        curl = rng.uniform(0.35, 1.15)          # outward bow amount
        dirx = rng.uniform(-0.3, 0.3) + np.sign(x0) * curl * 0.4
        dirz = rng.uniform(-0.15, 0.15) + z0 * 0.3
        length = rng.uniform(1.5, 2.1)
        phase = rng.uniform(0, 2 * np.pi)
        pts = []
        for j in range(segs + 1):
            t = j / segs
            bow = t * t * curl * 0.3
            sway = 0.04 * np.sin(phase + t * 5.0)
            pts.append((
                x0 + dirx * t + bow * np.sign(x0) + sway,
                0.75 - length * t,
                z0 + dirz * t + 0.03 * np.cos(phase + t * 4.0),
            ))
        strands.append(pts)

    header = struct.pack(
        "<4s4I2f3f88s",
        b"HAIR", n, n * (segs + 1), 0b000111, segs,
        0.006, 0.0, 1.0, 1.0, 1.0,
        b"E36 lock".ljust(88, b"\0"),
    )
    segments = b"".join(struct.pack("<H", segs) for _ in strands)
    points = b"".join(struct.pack("<3f", *p) for s in strands for p in s)
    thick = b"".join(struct.pack("<f", 0.006) for s in strands for _ in s)
    path.write_bytes(header + segments + points + thick)


def scene(mat_lines, obj_name, xoff):
    return f"""
scene.camera.lookat.orig = 0.0 0.1 -6.0
scene.camera.lookat.target = 0.0 -0.4 0.0
scene.camera.up = 0.0 1.0 0.0
scene.camera.fieldofview = 26
scene.camera.lensradius = 0.015
scene.camera.focaldistance = 6.0

scene.lights.env.type = constantinfinite
scene.lights.env.color = 0.06 0.06 0.08
scene.lights.env.gain = 1.0 1.0 1.0

scene.materials.backdrop.type = matte
scene.materials.backdrop.kd = 0.012 0.012 0.014
scene.shapes.backdrop_sh.type = mesh
scene.shapes.backdrop_sh.ply = /tmp/e36_backdrop.ply
scene.objects.backdrop.material = backdrop
scene.objects.backdrop.shape = backdrop_sh

scene.materials.strip.type = matte
scene.materials.strip.kd = 0 0 0
scene.materials.strip.emission = 300.0 190.0 90.0
scene.shapes.strip_sh.type = mesh
scene.shapes.strip_sh.ply = /tmp/e36_strip.ply
scene.objects.strip.material = strip
scene.objects.strip.shape = strip_sh

scene.materials.strip2.type = matte
scene.materials.strip2.kd = 0 0 0
scene.materials.strip2.emission = 70.0 85.0 110.0
scene.shapes.strip2_sh.type = mesh
scene.shapes.strip2_sh.ply = /tmp/e36_strip2.ply
scene.objects.strip2.material = strip2
scene.objects.strip2.shape = strip2_sh

{mat_lines}
scene.shapes.{obj_name}.type = strands
scene.shapes.{obj_name}.file = {HAIR_FILE}
scene.shapes.{obj_name}.tessellation.type = solid
scene.shapes.{obj_name}.tessellation.solid.sidecount = 6
scene.shapes.{obj_name}.transformation = 1 0 0 0 0 1 0 0 0 0 1 0 {xoff} 0 0 1
scene.objects.{obj_name}.material = hair_mat_{obj_name}
scene.objects.{obj_name}.shape = {obj_name}
"""


def write_quad(path, verts):
    ply = ("ply\nformat ascii 1.0\n"
           f"element vertex {len(verts)}\n"
           "property float x\nproperty float y\nproperty float z\n"
           "element face 2\nproperty list uchar int vertex_indices\n"
           "end_header\n")
    ply += "".join(f"{v[0]} {v[1]} {v[2]}\n" for v in verts)
    ply += "3 0 1 2\n3 0 2 3\n"
    Path(path).write_text(ply)


def write_strip(path="/tmp/e36_strip.ply"):
    """Back-right warm strip: rim + TT transmission glow."""
    write_quad(path, [(2.0, -2.0, 2.2), (3.4, -2.0, 2.2),
                      (3.4, 2.4, 2.2), (2.0, 2.4, 2.2)])


def write_strip2(path="/tmp/e36_strip2.ply"):
    """Front-left cool strip: R/TRT specular sheen band."""
    write_quad(path, [(-3.2, -1.6, -1.8), (-2.4, -1.6, -1.8),
                      (-2.4, 2.0, -1.8), (-3.2, 2.0, -1.8)])


def write_backdrop(path="/tmp/e36_backdrop.ply"):
    """A single large quad behind/below the locks."""
    verts = [(-8.0, -2.6, 5.5), (8.0, -2.6, 5.5),
             (8.0, 4.0, 5.5), (-8.0, 4.0, 5.5)]
    ply = ("ply\nformat ascii 1.0\n"
           f"element vertex {len(verts)}\n"
           "property float x\nproperty float y\nproperty float z\n"
           "element face 2\nproperty list uchar int vertex_indices\n"
           "end_header\n")
    ply += "".join(f"{v[0]} {v[1]} {v[2]}\n" for v in verts)
    ply += "3 0 1 2\n3 0 2 3\n"
    Path(path).write_text(ply)


def render(props_str, engine):
    props = pysuperluxcore.Properties()
    props.SetFromString(props_str)
    os.chdir(str(REPO))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    cfg = pysuperluxcore.Properties()
    extra = f'opencl.devices.select = "{OCL_DEV}"' if engine == "PATHOCL" else ""
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
film.imagepipelines.0.0.type = TONEMAP_LINEAR
film.imagepipelines.0.0.scale = 0.85
film.imagepipelines.0.1.type = GAMMA_CORRECTION
film.imagepipelines.0.1.value = 2.2
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
{extra}
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + 1200
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled ({engine})")
        time.sleep(1.0)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def main():
    write_lock(HAIR_FILE)
    write_backdrop()
    write_strip()
    write_strip2()
    which = sys.argv[1] if len(sys.argv) > 1 else "both"

    brunette = ("scene.materials.hair_mat_brunette.type = hairmat\n"
                "scene.materials.hair_mat_brunette.model = huang\n"
                "scene.materials.hair_mat_brunette.eumelanin = 0.3\n"
                "scene.materials.hair_mat_brunette.pheomelanin = 0.15\n"
                "scene.materials.hair_mat_brunette.roughness = 0.32\n"
                "scene.materials.hair_mat_brunette.alpha = 4.0\n")
    blonde = ("scene.materials.hair_mat_blonde.type = hairmat\n"
              "scene.materials.hair_mat_blonde.model = huang\n"
              "scene.materials.hair_mat_blonde.eumelanin = 0.02\n"
              "scene.materials.hair_mat_blonde.pheomelanin = 0.35\n"
              "scene.materials.hair_mat_blonde.roughness = 0.28\n"
              "scene.materials.hair_mat_blonde.alpha = 4.0\n")

    # Two locks side by side: brunette left, blonde right. One scene,
    # two objects sharing the strand file with different transforms.
    full = scene(brunette, "brunette", -0.7) + scene(blonde, "blonde", 0.7)
    # drop the duplicated camera/light blocks: keep only the first
    # scene's header, both scenes' object blocks
    head = full.split("scene.materials")[0]
    tail = "\n".join(l for l in full.splitlines()
                     if l.startswith(("scene.materials", "scene.shapes",
                                      "scene.objects")))
    full = head + tail + "\n"

    engines = {"cpu": "PATHCPU", "gpu": "PATHOCL"}
    for tag in ("cpu", "gpu"):
        if which not in ("both", tag):
            continue
        img = render(full, engines[tag])
        from PIL import Image
        out = f"/tmp/e36_lock_{tag}.png"
        Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(out)
        print(f"{tag}: mean {img.mean():.4f} -> {out}")


if __name__ == "__main__":
    main()
