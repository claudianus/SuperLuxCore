# SPDX-License-Identifier: Apache-2.0
#
# E37: light-strategy audit regression (B1-B10 fixes).
#
# Scene: a matte floor marked shadowcatcher + onlyinfinitelights, an
# opaque sphere, a glass sphere, an emitter quad, sky2+sun (infinite)
# and two point lights (finite). That combination exercises:
#   B1  GPU DLS_CACHE infinite-strategy compile (wrong dynamic_cast)
#   B2  GPU DLSC lookup bypass on only-infinite vertices
#   B3  GPU DLSC landing-shade-normal keying
#   B4  DirectHit MIS must use the previous vertex's light strategy
#   B5  GPU SampleLightPdf null-distribution -> 0.f (not NULL_INDEX)
#   B7  GPU ReSTIR GRIS merge weight unclamped (CPU parity)
#   B10 shade-normal convention in restirgi / bidircpu sampling
#
# Checks (per strategy, PATHCPU + PATHOCL/Metal):
#   finite output, no dead-pixel flood, GPU/CPU mean-luminance ratio
#   and luminance RMSE within bounds.
#
# Run:
#   python3.13 dev-tools/e37_lightstrategy_audit.py
#   python3.13 dev-tools/e37_lightstrategy_audit.py --strategies DLS_CACHE,RESTIR_DI

import argparse
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for cfg in ("Release", "Debug"):
    p = REPO / "out/build/src/pyluxcore" / cfg
    if (p / "pyluxcore.cpython-313-darwin.so").exists():
        sys.path.insert(0, str(p))
        break
import pyluxcore

WIDTH, HEIGHT = 1280, 720
SPP = 48
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300
OUT_DIR = REPO / "dev-tools/out/e37"

STRATEGIES = ["UNIFORM", "POWER", "LOG_POWER", "DLS_CACHE", "RESTIR_DI"]

# Floor uses shadowcatcher.onlyinfinitelights so its NEE candidates come
# from the infinite distribution while the spheres still see everything.
SCENE_PROPS = """
scene.camera.type = perspective
scene.camera.lookat.orig = 0.0 -6.0 1.4
scene.camera.lookat.target = 0.0 0.0 0.8
scene.camera.fieldofview = 45

scene.shapes.floor.type = inlinedmesh
scene.shapes.floor.vertices = -10 -10 0  10 -10 0  10 10 0  -10 10 0
scene.shapes.floor.faces = 0 1 2  0 2 3
scene.shapes.sphere1.type = inlinedmesh
scene.shapes.sphere1.vertices = {SPHERE1_V}
scene.shapes.sphere1.faces = {SPHERE1_F}
scene.shapes.sphere2.type = inlinedmesh
scene.shapes.sphere2.vertices = {SPHERE2_V}
scene.shapes.sphere2.faces = {SPHERE2_F}
scene.shapes.emit.type = inlinedmesh
scene.shapes.emit.vertices = -0.8 -0.8 3.5  0.8 -0.8 3.5  0.8 0.8 3.5  -0.8 0.8 3.5
scene.shapes.emit.faces = 0 1 2  0 2 3

scene.materials.Floor.type = matte
scene.materials.Floor.kd = 0.7 0.7 0.7
scene.materials.Floor.shadowcatcher.enable = 1
scene.materials.Floor.shadowcatcher.onlyinfinitelights = 1
scene.materials.Sphere.type = matte
scene.materials.Sphere.kd = 0.8 0.4 0.2
scene.materials.Glass.type = glass
scene.materials.Glass.kr = 1.0 1.0 1.0
scene.materials.Glass.kt = 1.0 1.0 1.0
scene.materials.Glass.ioroutside = 1.0
scene.materials.Glass.iorinside = 1.5
scene.materials.Emitter.type = matte
scene.materials.Emitter.emission = 40.0 40.0 40.0

scene.objects.floor.shape = floor
scene.objects.floor.material = Floor
scene.objects.sphere1.shape = sphere1
scene.objects.sphere1.material = Sphere
scene.objects.sphere2.shape = sphere2
scene.objects.sphere2.material = Glass
scene.objects.emit.shape = emit
scene.objects.emit.material = Emitter

scene.lights.sky.type = sky2
scene.lights.sky.dir = 0.3 -0.5 0.8
scene.lights.sky.turbidity = 2.2
scene.lights.sky.gain = 0.00002 0.00002 0.00002
scene.lights.sun.type = sun
scene.lights.sun.dir = 0.3 -0.5 0.8
scene.lights.sun.gain = 0.00003 0.00003 0.00003
scene.lights.sun.turbidity = 2.2
scene.lights.sun.relsize = 32
scene.lights.ptl.type = point
scene.lights.ptl.position = 1.5 0.5 3.0
scene.lights.ptl.gain = 60.0 45.0 30.0
scene.lights.ptl2.type = point
scene.lights.ptl2.position = -2.0 0.0 2.0
scene.lights.ptl2.gain = 25.0 30.0 45.0
"""


def sphere(cx, cy, cz, r, nseg=20, nring=12):
    import math
    verts = [(cx, cy, cz + r)]
    for i in range(1, nring):
        th = math.pi * i / nring
        for j in range(nseg):
            ph = 2 * math.pi * j / nseg
            verts.append((cx + r * math.sin(th) * math.cos(ph),
                          cy + r * math.sin(th) * math.sin(ph),
                          cz + r * math.cos(th)))
    verts.append((cx, cy, cz - r))
    faces = []
    for j in range(nseg):
        faces.append((0, 1 + j, 1 + (j + 1) % nseg))
    for i in range(nring - 2):
        for j in range(nseg):
            a = 1 + i * nseg + j
            b = 1 + i * nseg + (j + 1) % nseg
            faces.append((a, a + nseg, b + nseg))
            faces.append((a, b + nseg, b))
    bot = len(verts) - 1
    for j in range(nseg):
        a = 1 + (nring - 2) * nseg + j
        b = 1 + (nring - 2) * nseg + (j + 1) % nseg
        faces.append((a, bot, b))
    v = "  ".join(f"{x:.5f} {y:.5f} {z:.5f}" for x, y, z in verts)
    f = "  ".join(" ".join(map(str, t)) for t in faces)
    return v, f


def build_scene():
    v1, f1 = sphere(-0.8, 0.0, 0.8, 0.8)
    v2, f2 = sphere(1.0, -0.5, 0.55, 0.55)
    props = pyluxcore.Properties()
    props.SetFromString(SCENE_PROPS.format(
        SPHERE1_V=v1, SPHERE1_F=f1, SPHERE2_V=v2, SPHERE2_F=f2))
    scene = pyluxcore.Scene()
    scene.Parse(props)
    return scene


def device_mask(want_type):
    pyluxcore.Init()
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


def render(scene, engine, strategy, sel=None, seed=17):
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
lightstrategy.type = {strategy}
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
    while True:
        # UpdateStats() drives UpdateFilm()->RunTests() where spp is
        # evaluated; HasDone() alone never observes the halt.
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {SPP} spp")
        time.sleep(0.5)
    buf = np.zeros((HEIGHT, WIDTH, 3), dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf, 0)
    ses.Stop()
    return buf


def save_png(img, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    arr = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    try:
        import imageio.v2 as imageio
        imageio.imwrite(str(path), arr)
    except ImportError:
        with open(str(path.with_suffix(".ppm")), "wb") as f:
            f.write(f"P6\n{arr.shape[1]} {arr.shape[0]}\n255\n".encode())
            f.write(arr.tobytes())


def luminance(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strategies", default=",".join(STRATEGIES))
    args = ap.parse_args()
    wanted = args.strategies.split(",")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    scene = build_scene()
    mask = device_mask("METAL_GPU") or device_mask("OPENCL_GPU")

    results = {}
    fails = []
    for strat in wanted:
        print(f"=== {strat} PATHCPU ===", flush=True)
        cpu = render(scene, "PATHCPU", strat)
        save_png(cpu, OUT_DIR / f"{strat.lower()}_cpu.png")
        results[("cpu", strat)] = cpu

        if not np.isfinite(cpu).all():
            fails.append(f"{strat} PATHCPU: NaN/Inf pixels")
        if (cpu < 1e-7).mean() > 0.5:
            fails.append(f"{strat} PATHCPU: >50% dead pixels")

        if mask:
            print(f"=== {strat} PATHOCL ===", flush=True)
            try:
                gpu = render(scene, "PATHOCL", strat, sel=mask)
            except Exception as e:
                fails.append(f"{strat} PATHOCL: {e}")
                continue
            save_png(gpu, OUT_DIR / f"{strat.lower()}_gpu.png")

            if not np.isfinite(gpu).all():
                fails.append(f"{strat} PATHOCL: NaN/Inf pixels")
                continue
            cl, gl = luminance(cpu), luminance(gpu)
            ratio = gl.mean() / max(cl.mean(), 1e-9)
            rmse = float(np.sqrt(((gl - cl) ** 2).mean()))
            black = float((gl < 1e-7).mean())
            print(f"  ratio={ratio:.4f} rmse={rmse:.5f} black={black:.4f}",
                  flush=True)
            # Strategies share the same converged answer; 48spp leaves
            # sampling noise, so bounds are structural, not exact.
            if not (0.90 <= ratio <= 1.10):
                fails.append(f"{strat} PATHOCL: ratio {ratio:.3f}")
            if rmse > 0.15:
                fails.append(f"{strat} PATHOCL: rmse {rmse:.4f}")
            if black > 0.30:
                fails.append(f"{strat} PATHOCL: black {black:.3f}")
        else:
            print(f"=== {strat} PATHOCL: no GPU device, skipped ===")

    print()
    for f in fails:
        print(f"[FAIL] {f}")
    print(f"{'PASS' if not fails else 'FAIL'}: "
          f"{len(wanted)} strategies audited, {len(fails)} failures")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
