# SPDX-License-Identifier: Apache-2.0
#
# E35: TILEPATHOCL light-tracing parity — PATHCPU vs TILEPATHOCL
# (Metal/OpenCL) on a caustic-heavy scene.
#
# TILEPATHOCL used to disable GPU light tracing entirely ("tile-clipped
# splats"): a light path splat projects in *film* coordinates while the
# tile film buffer is tile-local, so the projection landed in the wrong
# coordinate frame and most splats were dropped. LightPath_ProjectToFilm
# now projects against the whole camera film, clips to the tile's
# film-space rect, then shifts back to tile-local for Film_SplatLight.
# Splats outside the resident tile are dropped - still unbiased: each
# tile's pixels collect the light-path population drawn while the tile
# is resident.
#
# A small tile.size (160x90) forces ~40 tiles so boundary clipping is
# actually exercised; hybrid back-forward routes the caustics through
# the light tasks.
#
# Run from the repo root (after a configure+build of pysuperluxcore Debug):
#   python3.13 dev-tools/e35_tilepath_lighttracing.py

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
OUT_DIR = REPO / "dev-tools/out/e35"

IMG = REPO / "scenes/simple-mat/image.png"
SPHERE = REPO / "scenes/mnee/bumpysphere.ply"

# Same caustic scene as e33 (glass sphere + directional-map emitters).
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


def render(scene, engine, sel=None, seed=17, tile_size=None):
    extra = ""
    if tile_size:
        extra += f"tile.size.x = {tile_size[0]}\ntile.size.y = {tile_size[1]}\n"
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = {"TILEPATHSAMPLER" if "TILEPATH" in engine else "SOBOL"}
batch.haltspp = {SPP}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
path.hybridbackforward.enable = 1
path.lighttracing.enable = 1
path.lighttracing.taskfraction = 0.5
film.imagepipelines.0.0.type = NOP
film.imagepipelines.0.1.type = TONEMAP_LINEAR
film.imagepipelines.0.1.scale = 1
film.imagepipelines.0.2.type = GAMMA_CORRECTION
film.imagepipelines.0.2.value = 2.2
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.index = 0
{extra}""")
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
    # PPM fallback (imageio not installed on this host)
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
    save_png(results["PATHCPU"], OUT_DIR / "tilelt_cpu.png")

    # Small tiles so light-splat clipping across tile boundaries is
    # exercised (~40 tiles at 160x90 for 720p).
    tile = (160, 90)
    for want, name in [("METAL_GPU", "TILEPATHOCL-METAL"),
            ("OPENCL_GPU", "TILEPATHOCL-OPENCL")]:
        mask = device_mask(want)
        if not mask:
            print(f"skip {name}: no {want} device", flush=True)
            continue
        print(f"=== {name} ===", flush=True)
        results[name] = render(scene, "TILEPATHOCL", sel=mask,
                tile_size=tile)
        save_png(results[name], OUT_DIR / f"tilelt_{name.lower()}.png")

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
