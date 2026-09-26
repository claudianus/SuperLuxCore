# SPDX-License-Identifier: Apache-2.0
#
# E34: volume path-guiding CPU/GPU parity — PATHCPU vs PATHOCL
# (Metal/OpenCL) inside a participating medium.
#
# The GPU guiding gate used to require a GLOSSY BSDF event, so volume
# scattering vertices (phase-function events, always guidable on CPU
# via GuidableBsdf's IsVolume branch) were never guided. Guide_Sample/
# Guide_Pdf now take an isotropic flag: full-sphere bins without cosine
# weighting and a uniform-sphere fallback (pdf = 1/4pi), mirroring the
# CPU Sample(..., isotropic=true) path.
#
# Also exercises the M2b-2 record fidelity path: device training
# records now carry the exact receiver position + direction (drained
# into the CPU write tree via Record()), match the CPU vertex gates
# (non-delta, depth >= 1), and buffers are cleared after each drain so
# stale markers cannot re-count the same record.
#
# Scene: a scattering world volume (sigma_s = 0.06) lit by a bright
# sphere light through a glass ball (volumetric caustic) plus bounce
# light off a matte floor - deep-indirect volume transport is where
# guiding helps. path.guiding.enable = 1 on both engines.
#
# Run from the repo root (after a configure+build of pyluxcore Debug):
#   python3.13 dev-tools/e34_volume_guiding_parity.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
import pyluxcore

WIDTH, HEIGHT = 1280, 720
SPP = 96
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300
OUT_DIR = REPO / "dev-tools/out/e34"

SPHERE = REPO / "scenes/mnee/bumpysphere.ply"

# Scattering medium fills the scene; a glass ball focuses the sphere
# light into a volumetric caustic and the floor adds bounce light.
SCENE_PROPS = f"""
scene.camera.type = perspective
scene.camera.lookat.orig = 4.2 -4.2 2.6
scene.camera.lookat.target = 0 0 0.9
scene.camera.fieldofview = 42
scene.camera.lensradius = 0
scene.camera.cliphither = 0.01
scene.camera.clipyon = 1000

# World volume: isotropic scattering
scene.world.volume.default = vol_air
scene.volumes.vol_air.type = homogeneous
scene.volumes.vol_air.absorption = 0.01 0.01 0.01
scene.volumes.vol_air.scattering = 0.035 0.035 0.035
scene.volumes.vol_air.asymmetry = 0.0 0.0 0.0
scene.volumes.vol_air.multiscattering = 1

# Floor (matte) - bounce light into the medium
scene.shapes.floor.type = inlinedmesh
scene.shapes.floor.vertices = -7 -7 0  7 -7 0  7 7 0  -7 7 0
scene.shapes.floor.faces = 0 1 2  0 2 3
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.7 0.7 0.72
scene.objects.floor.ply = floor
scene.objects.floor.material = floor

# Glass sphere -> volumetric caustic
scene.shapes.ball.type = mesh
scene.shapes.ball.ply = {SPHERE}
scene.shapes.ball.transformation = 0.55 0 0 0  0 0.55 0 0  0 0 0.55 0  0 0 0.62 1
scene.materials.glass.type = glass
scene.materials.glass.kr = 0.98 0.98 0.98
scene.materials.glass.kt = 0.98 0.98 0.98
scene.materials.glass.interiorior = 1.5
scene.objects.ball.ply = ball
scene.objects.ball.material = glass

# Bright sphere light above the glass ball
scene.lights.sph.type = sphere
scene.lights.sph.position = 0 0.3 3.2
scene.lights.sph.radius = 0.25
scene.lights.sph.color = 1 0.9 0.75
scene.lights.sph.power = 15
scene.lights.sph.efficency = 30

# Fill point light (indirect illumination inside the medium)
scene.lights.pt.type = point
scene.lights.pt.position = -2.5 1.5 2.5
scene.lights.pt.color = 0.7 0.8 1
scene.lights.pt.power = 8
scene.lights.pt.efficency = 30
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
path.guiding.enable = 1
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
        ses.UpdateStats()
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

    print("=== PATHCPU ===", flush=True)
    results["PATHCPU"] = render(scene, "PATHCPU")
    save_png(results["PATHCPU"], OUT_DIR / "volguide_cpu.png")

    for want, name in [("METAL_GPU", "PATHOCL-METAL"), ("OPENCL_GPU", "PATHOCL-OPENCL")]:
        mask = device_mask(want)
        if not mask:
            print(f"skip {name}: no {want} device", flush=True)
            continue
        print(f"=== {name} ===", flush=True)
        results[name] = render(scene, "PATHOCL", sel=mask)
        save_png(results[name], OUT_DIR / f"volguide_{name.lower()}.png")

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
