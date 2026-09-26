# SPDX-License-Identifier: Apache-2.0
#
# E33: GPU light-tracing Emit() parity — PATHCPU vs PATHOCL (Metal/OpenCL).
#
# GPU light tracing used to exclude sphere/mappoint/mapsphere/projection
# lights and directional-map (IES) triangle emitters from the emit
# distribution (weight 0 + warning), so their light-tracing contribution
# (caustics under path.hybridbackforward.enable) was silently lost on GPU.
# The missing device Emit() functions are now ported; this test renders a
# caustic-heavy scene using every newly supported light type with hybrid
# back-forward enabled and compares CPU/GPU outputs.
#
# Run from the repo root (after a configure+build of pysuperluxcore Debug):
#   python3.13 dev-tools/e33_lighttracing_emit_parity.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Debug"))
import pysuperluxcore

WIDTH, HEIGHT = 1280, 720
SPP = 96
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300
OUT_DIR = REPO / "dev-tools/out/e33"

IMG = REPO / "scenes/simple-mat/image.png"
SPHERE = REPO / "scenes/mnee/bumpysphere.ply"

# All five newly-ported emit paths in one scene: a glass sphere produces
# caustics on the floor (light tracing), lit by a sphere light, a mappoint
# (directional map), a mapsphere, a projection light and an emissive mesh
# triangle carrying a directional emission map.
SCENE_PROPS = f"""
scene.camera.type = perspective
scene.camera.lookat.orig = 4.2 -4.2 2.6
scene.camera.lookat.target = 0 0 0.9
scene.camera.fieldofview = 42
scene.camera.lensradius = 0
scene.camera.cliphither = 0.01
scene.camera.clipyon = 1000

# Floor (matte)
scene.shapes.floor.type = inlinedmesh
scene.shapes.floor.vertices = -7 -7 0  7 -7 0  7 7 0  -7 7 0
scene.shapes.floor.faces = 0 1 2  0 2 3
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.7 0.7 0.72
scene.objects.floor.ply = floor
scene.objects.floor.material = floor

# Glass sphere -> caustics
scene.shapes.ball.type = mesh
scene.shapes.ball.ply = {SPHERE}
scene.shapes.ball.transformation = 0.55 0 0 0  0 0.55 0 0  0 0 0.55 0  0 0 0.62 1
scene.materials.glass.type = glass
scene.materials.glass.kr = 0.98 0.98 0.98
scene.materials.glass.kt = 0.98 0.98 0.98
scene.materials.glass.interiorior = 1.5
scene.objects.ball.ply = ball
scene.objects.ball.material = glass

# 1) Sphere light above the glass ball
scene.lights.sph.type = sphere
scene.lights.sph.position = 0 0.3 3.2
scene.lights.sph.radius = 0.25
scene.lights.sph.color = 1 0.9 0.75
scene.lights.sph.power = 60
scene.lights.sph.efficency = 30

# 2) MapPoint light (directional map emission)
scene.lights.mp.type = mappoint
scene.lights.mp.position = 2.0 1.0 3.0
scene.lights.mp.mapfile = {IMG}
scene.lights.mp.color = 0.8 0.9 1
scene.lights.mp.power = 50
scene.lights.mp.efficency = 30

# 3) MapSphere light
scene.lights.ms.type = mapsphere
scene.lights.ms.position = -2.0 1.0 3.0
scene.lights.ms.radius = 0.35
scene.lights.ms.mapfile = {IMG}
scene.lights.ms.color = 1 1 1
scene.lights.ms.power = 50
scene.lights.ms.efficency = 30

# 4) Projection light aimed at the glass ball
scene.lights.prj.type = projection
scene.lights.prj.position = 0 -3.0 3.5
scene.lights.prj.target = 0 0 0.6
scene.lights.prj.mapfile = {IMG}
scene.lights.prj.fov = 45
scene.lights.prj.power = 400
scene.lights.prj.efficency = 30

# 5) Emissive mesh triangle with directional emission map (IES-like)
scene.shapes.emitquad.type = inlinedmesh
scene.shapes.emitquad.vertices = -0.8 -0.4 4.2  0.8 -0.4 4.2  0.8 0.4 4.2  -0.8 0.4 4.2
scene.shapes.emitquad.faces = 0 1 2  0 2 3
scene.materials.emit.type = matte
scene.materials.emit.kd = 0.5 0.5 0.5
scene.materials.emit.emission = 1 1 1
scene.materials.emit.emission.power = 80
scene.materials.emit.emission.efficency = 40
scene.materials.emit.emission.mapfile = {IMG}
scene.objects.emit.ply = emitquad
scene.objects.emit.material = emit
"""


def device_mask(want_type):
    pysuperluxcore.Init()  # required: device enumeration is empty before Init
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
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
    props = pysuperluxcore.Properties()
    props.SetFromString(SCENE_PROPS)
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return scene


def render(scene, engine, sel=None, seed=17):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
path.hybridbackforward.enable = 1
path.lighttracing.taskfraction = 0.5
film.imagepipelines.0.0.type = NOP
film.imagepipelines.0.1.type = TONEMAP_LINEAR
film.imagepipelines.0.1.scale = 1
film.imagepipelines.0.2.type = GAMMA_CORRECTION
film.imagepipelines.0.2.value = 2.2
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.index = 0
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(0.5)
        ses.UpdateStats()
        if ses.HasDone():
            break
    ses.Stop()
    film = ses.GetFilm()
    w, h = WIDTH, HEIGHT
    buf = np.zeros((h, w, 3), dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf, 0)
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

    print("=== PATHCPU ===", flush=True)
    results["PATHCPU"] = render(scene, "PATHCPU")
    save_png(results["PATHCPU"], OUT_DIR / "ltemit_cpu.png")

    for want, name in [("METAL_GPU", "PATHOCL-METAL"), ("OPENCL_GPU", "PATHOCL-OPENCL")]:
        mask = device_mask(want)
        if not mask:
            print(f"skip {name}: no {want} device", flush=True)
            continue
        print(f"=== {name} ===", flush=True)
        results[name] = render(scene, "PATHOCL", sel=mask)
        save_png(results[name], OUT_DIR / f"ltemit_{name.lower()}.png")

    ref = results["PATHCPU"]
    refL = luminance(ref)
    print(f"\nPATHCPU mean luminance: {refL.mean():.5f}")
    for name, img in results.items():
        if name == "PATHCPU":
            continue
        L = luminance(img)
        rmse = float(np.sqrt(((L - refL) ** 2).mean()))
        ratio = float(L.mean() / max(refL.mean(), 1e-9))
        print(f"{name}: mean={L.mean():.5f} ratio={ratio:.4f} rmse={rmse:.5f}")


if __name__ == "__main__":
    main()
