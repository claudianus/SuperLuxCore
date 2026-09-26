# SPDX-License-Identifier: Apache-2.0
#
# E31: bevel texture (bump) parity — PATHCPU vs PATHOCL (Metal/OpenCL).
#
# The "bevel" SDL texture was dead code (parser commented out, no GPU eval
# op). It is now restored and ported to the device path; this test renders a
# cube wrapped in an edgedetectoraov shape with a bevel texture as bumptex
# and compares CPU/GPU outputs.
#
# Run from the repo root (after a configure+build of pyluxcore Debug):
#   python3.13 dev-tools/e31_bevel_texture_parity.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
import pyluxcore

WIDTH, HEIGHT = 1280, 720
SPP = 64
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300
OUT_DIR = REPO / "dev-tools/out/e31"

SCENE_PROPS = f"""
scene.camera.type = perspective
scene.camera.lookat.orig = 4.5 -4.5 3.2
scene.camera.lookat.target = 0 0 1.8
scene.camera.fieldofview = 40
scene.camera.lensradius = 0
scene.camera.focaldistance = 7
scene.camera.autofocus.enable = 0
scene.camera.cliphither = 0.01
scene.camera.clipyon = 1000
scene.lights.sky.type = sky2
scene.lights.sky.dir = 0.2 0.3 1
scene.lights.sky.gain = 1.5e-5 1.5e-5 1.5e-5
scene.lights.sky.turbidity = 4
scene.lights.sun.type = sun
scene.lights.sun.dir = -0.5 -0.4 -0.7
scene.lights.sun.gain = 3 3 3
scene.lights.sun.turbidity = 4
scene.lights.sun.relsize = 2
scene.shapes.cube_raw.type = mesh
scene.shapes.cube_raw.ply = {REPO / "scenes/bevel/mesh-00000.ply"}
scene.shapes.cube_edge.type = edgedetectoraov
scene.shapes.cube_edge.source = cube_raw
scene.textures.bevel_tex.type = bevel
scene.textures.bevel_tex.radius = 0.1
scene.materials.cube_mat.type = matte
scene.materials.cube_mat.kd = 0.65 0.62 0.55
scene.materials.cube_mat.bumptex = bevel_tex
scene.objects.cube.ply = cube_edge
scene.objects.cube.material = cube_mat
scene.objects.cube.transformation = 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1
"""


def device_mask(want_type):
    pyluxcore.Init()  # required: device enumeration is empty before Init
    descs = pyluxcore.GetOpenCLDeviceDescs()
    mask = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        mask += "1" if t == want_type else "0"
        i += 1
    return mask or None


def parse_scene():
    props = pyluxcore.Properties()
    props.SetFromString(SCENE_PROPS)
    scene = pyluxcore.Scene()
    scene.Parse(props)
    return scene


def render(scene, engine, sel=None, seed=17):
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
film.imagepipelines.0.0.type = NOP
film.imagepipelines.0.1.type = TONEMAP_LINEAR
film.imagepipelines.0.1.scale = 1
film.imagepipelines.0.2.type = GAMMA_CORRECTION
film.imagepipelines.0.2.value = 2.2
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.index = 0
""")
    if sel:
        cfg.Set(pyluxcore.Property("opencl.devices.select", sel))
    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(0.5)
        stats = ses.GetStats()
        try:
            spp = stats.Get("stats.engine.renderengine.samplepersec").GetFloat()
        except Exception:
            pass
        try:
            done = stats.Get("stats.engine.renderengine.converged").GetFloat()
        except Exception:
            done = 0
        if ses.HasDone():
            break
    ses.Stop()
    film = ses.GetFilm()
    w, h = WIDTH, HEIGHT
    buf = np.zeros((h, w, 3), dtype=np.float32)
    film.GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf, 0)
    return buf


def save_png(img, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    arr = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    try:
        import imageio.v2 as imageio
        imageio.imwrite(str(path), arr)
    except ImportError:
        # Fall back to PPM (still viewable)
        with open(str(path.with_suffix(".ppm")), "wb") as f:
            f.write(f"P6\n{arr.shape[1]} {arr.shape[0]}\n255\n".encode())
            f.write(arr.tobytes())


def luminance(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    scene = parse_scene()

    results = {}

    print("=== PATHCPU ===")
    results["PATHCPU"] = render(scene, "PATHCPU")
    save_png(results["PATHCPU"], OUT_DIR / "bevel_cpu.png")

    for want, name in [("METAL_GPU", "PATHOCL-METAL"), ("OPENCL_GPU", "PATHOCL-OPENCL")]:
        mask = device_mask(want)
        if not mask:
            print(f"=== {name}: no device, skipped ===")
            continue
        print(f"=== {name} (mask {mask}) ===")
        try:
            results[name] = render(scene, "PATHOCL", sel=mask)
            save_png(results[name], OUT_DIR / f"bevel_{name.lower()}.png")
        except Exception as e:
            print(f"{name} failed: {e}")

    cpu = results["PATHCPU"]
    cpu_lum = luminance(cpu)
    print(f"\nPATHCPU mean luminance: {cpu_lum.mean():.5f}")
    for name, img in results.items():
        if name == "PATHCPU":
            continue
        lum = luminance(img)
        ratio = lum.mean() / max(cpu_lum.mean(), 1e-9)
        rmse = float(np.sqrt(((lum - cpu_lum) ** 2).mean()))
        finite = np.isfinite(img).all()
        print(f"{name}: mean={lum.mean():.5f} ratio={ratio:.3f} "
              f"rmse={rmse:.5f} finite={finite}")


if __name__ == "__main__":
    main()
