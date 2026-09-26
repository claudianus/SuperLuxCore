# SPDX-License-Identifier: Apache-2.0
#
# E36: METROPOLIS + GPU light tracing — PATHCPU vs PATHOCL
# (Metal/OpenCL) on a caustic-heavy scene.
#
# GPU light tasks used to be disabled under METROPOLIS because the
# sampler dispatch had no light-path case for it: light paths are not
# Metropolis chains (there is no accept/reject semantics for a task that
# only deposits splats), so each light task now draws an i.i.d. uniform
# stream from its private seed via Sampler_GetLightSample.
#
# This also covers the Metropolis samplesDataBuff stride fix: the
# current/proposed slot pair is indexed (2*gid + slot)*TOTAL_U_SIZE;
# the old (gid + slot)*TOTAL_U_SIZE made adjacent tasks overwrite each
# other's vectors.
#
# NOTE: Metropolis eye-path chains are implementation-dependent - CPU
# and GPU sample different Markov chains, so per-pixel noise differs.
# What must match is the mean (energy) and the presence of light-task
# caustics on GPU; RMSE stays above deterministic-sampler levels.
#
# Run from the repo root (after a configure+build of pyluxcore Debug):
#   python3.13 dev-tools/e36_metropolis_lighttracing.py

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
import pyluxcore

WIDTH, HEIGHT = 1280, 720
SPP = 128
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300
OUT_DIR = REPO / "dev-tools/out/e36"

IMG = REPO / "scenes/simple-mat/image.png"
SPHERE = REPO / "scenes/mnee/bumpysphere.ply"

# Same caustic scene as e33/e35 (glass sphere + directional emitters).
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

# Sphere light above the glass ball
scene.lights.sph.type = sphere
scene.lights.sph.position = 0 0.3 3.2
scene.lights.sph.radius = 0.25
scene.lights.sph.color = 1 0.9 0.75
scene.lights.sph.power = 60
scene.lights.sph.efficency = 30

# Projection light aimed at the glass ball
scene.lights.prj.type = projection
scene.lights.prj.position = 0 -3.0 3.5
scene.lights.prj.target = 0 0 0.6
scene.lights.prj.mapfile = {IMG}
scene.lights.prj.fov = 45
scene.lights.prj.power = 400
scene.lights.prj.efficency = 30

# Emissive mesh triangle with directional emission map
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


def render(scene, engine, lt, sel=None, seed=17):
    # On CPU the light pass is the native hybridbackforward thread; on
    # GPU it is the light-task population (path.lighttracing.enable).
    light_cfg = ("path.hybridbackforward.enable = 1\n"
            if engine == "PATHCPU" else
            "path.hybridbackforward.enable = 1\n"
            "path.lighttracing.enable = 1\n"
            "path.lighttracing.taskfraction = 0.5\n") if lt else ""
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = METROPOLIS
batch.haltspp = {SPP}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
{light_cfg}film.imagepipelines.0.0.type = NOP
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
        if ses.HasDone():
            break
    ses.Stop()
    film = ses.GetFilm()
    w, h = WIDTH, HEIGHT
    buf = np.zeros((h, w, 3), dtype=np.float32)
    film.GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf, 0)
    return buf


def save_ppm(img, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    arr = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    with open(str(path.with_suffix(".ppm")), "wb") as f:
        f.write(f"P6\n{arr.shape[1]} {arr.shape[0]}\n255\n".encode())
        f.write(arr.tobytes())


def luminance(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    scene = parse_scene()

    # --- Part 1: Metropolis + light tracing (the phase-1.6 feature) ---
    lt = {}
    print("=== PATHCPU METROPOLIS +LT ===", flush=True)
    lt["cpu_lt"] = render(scene, "PATHCPU", lt=True)
    save_ppm(lt["cpu_lt"], OUT_DIR / "mcpu_lt.png")

    for want, name in [("METAL_GPU", "metal_lt"), ("OPENCL_GPU", "ocl_lt")]:
        mask = device_mask(want)
        if not mask:
            print(f"skip {name}: no {want} device", flush=True)
            continue
        print(f"=== PATHOCL METROPOLIS +LT ({want}) ===", flush=True)
        lt[name] = render(scene, "PATHOCL", lt=True, sel=mask)
        save_ppm(lt[name], OUT_DIR / f"m{name}.png")

    refL = luminance(lt["cpu_lt"])
    print(f"\n[LT] PATHCPU mean luminance: {refL.mean():.5f}")
    for name, img in lt.items():
        if name == "cpu_lt":
            continue
        L = luminance(img)
        rmse = float(np.sqrt(((L - refL) ** 2).mean()))
        ratio = float(L.mean() / max(refL.mean(), 1e-9))
        nan = int(np.isnan(img).sum())
        print(f"[LT] {name}: mean={L.mean():.5f} ratio={ratio:.4f} "
              f"rmse={rmse:.5f} nans={nan}")

    # --- Part 2: Metropolis eye-path only (stride-fix regression) ---
    print("\n=== PATHCPU METROPOLIS (no LT) ===", flush=True)
    cpu_eye = render(scene, "PATHCPU", lt=False)
    save_ppm(cpu_eye, OUT_DIR / "mcpu_eye.png")

    mask = device_mask("METAL_GPU")
    if mask:
        print("=== PATHOCL METROPOLIS (no LT, Metal) ===", flush=True)
        gpu_eye = render(scene, "PATHOCL", lt=False, sel=mask)
        save_ppm(gpu_eye, OUT_DIR / "mmetal_eye.png")

        refL = luminance(cpu_eye)
        L = luminance(gpu_eye)
        rmse = float(np.sqrt(((L - refL) ** 2).mean()))
        ratio = float(L.mean() / max(refL.mean(), 1e-9))
        print(f"\n[eye] PATHCPU mean: {refL.mean():.5f}")
        print(f"[eye] PATHOCL-METAL: mean={L.mean():.5f} "
              f"ratio={ratio:.4f} rmse={rmse:.5f} nans={int(np.isnan(gpu_eye).sum())}")


if __name__ == "__main__":
    main()
