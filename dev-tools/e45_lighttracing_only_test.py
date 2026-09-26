# SPDX-License-Identifier: Apache-2.0
#
# E45: path.lighttracing.only must not be contaminated by native threads.
#
# Regression for a mode-contamination bug found after the GenmaB
# whiteout fix: PathOCLNativeRenderThread always created an eye sampler
# and called PathTracer::RenderSample(), even when
# path.lighttracing.only was set. opencl.native.threads.count defaults
# to the hardware thread count, so an "lt-only" GPU render was actually
# a GPU light tracing + native CPU eye-path hybrid and measured ~2x the
# LIGHTCPU brightness on an opaque test scene (the screen-normalized
# channel received splats from both estimators).
#
# The fix adds a light-only branch: native threads allocate a light
# sampler (SCREEN_NORMALIZED_ONLY, sampler.imagesamples.enable=false)
# and call RenderLightSample() instead, mirroring LIGHTCPU.
#
# Scene: matte box + emissive ceiling cube; lt-only output is pure
# light-traced splats (the wall receives no direct camera emission).
#
#   T1 native on/off:   PATHOCL lt-only mean with native threads
#                       enabled vs disabled must match (bug: ~2x)
#   T2 vs LIGHTCPU:     PATHOCL lt-only (natives on) within 2x of the
#                       LIGHTCPU reference at the same spp
#   T3 finite+nonzero:  lt-only output is finite and non-trivial
#   T4 eye path sane:   ordinary PATHOCL (lt-only off) with natives on
#                       still matches natives-off (native branch did
#                       not break normal rendering)
#
# Run:
#   python3.13 dev-tools/e45_lighttracing_only_test.py
#
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for _variant in ("Release", "Debug"):
    _p = REPO / "out/build/src/pysuperluxcore" / _variant
    if any(_p.glob("pysuperluxcore*.so")):
        sys.path.insert(0, str(_p))
        break
import pysuperluxcore

WIDTH, HEIGHT = 320, 180
SPP = 64
TASK_COUNT = 1 << 16
RENDER_TIMEOUT_S = 300
PLY = REPO / "scenes/cornell/unitcube.ply"

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def device_mask():
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    types = []
    i = 0
    while True:
        try:
            types.append(descs.Get(f"opencl.device.{i}.type").GetString())
        except Exception:
            break
        i += 1
    for want in ("METAL_GPU", "OPENCL_GPU", "CUDA_GPU", "VULKAN_GPU"):
        if want in types:
            return "".join("1" if t == want else "0" for t in types)
    return None


def scale_translate(sx, sy, sz, tx, ty, tz):
    return (f"{sx} 0 0 0  0 {sy} 0 0  0 0 {sz} 0  {tx} {ty} {tz} 1")


def scene():
    return f"""
scene.camera.type = perspective
scene.camera.lookat.orig = 0 -7 1.8
scene.camera.lookat.target = 0 0 1.6
scene.camera.fieldofview = 40

scene.materials.Wall.type = matte
scene.materials.Wall.kd = 0.7 0.7 0.7
scene.materials.Emit.type = matte
scene.materials.Emit.kd = 1 1 1
scene.materials.Emit.emission = 60 60 60

scene.objects.Wall.material = Wall
scene.objects.Wall.ply = {PLY}
scene.objects.Wall.transformation = {scale_translate(6, 0.1, 6, -3, 1.5, -1)}
scene.objects.Wall.id = 10

scene.objects.Emit.material = Emit
scene.objects.Emit.ply = {PLY}
scene.objects.Emit.transformation = {scale_translate(2, 2, 0.2, -1, -1, 3.6)}
scene.objects.Emit.id = 20
"""


def render(engine, lt_only, native_threads, sel=None):
    scn = pysuperluxcore.Properties()
    scn.SetFromString(scene())
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
opencl.task.count = {TASK_COUNT}
opencl.native.threads.count = {native_threads}
path.pathdepth.total = 8
path.lighttracing.enable = 1
path.lighttracing.only = {1 if lt_only else 0}
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = RGB_IMAGEPIPELINE
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {SPP} spp")
        time.sleep(0.25)

    film = ses.GetFilm()
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB, rgb)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def img_mean(rgb):
    return float(np.mean(rgb.mean(axis=2)))


def main():
    gpu_sel = device_mask()
    has_gpu = gpu_sel is not None

    if has_gpu:
        # T1: the invariant the bug broke -- native threads must not
        # change the lt-only result
        gpu_nat = render("PATHOCL", True, 4, gpu_sel)
        gpu_nonat = render("PATHOCL", True, 0, gpu_sel)
        m_nat, m_nonat = img_mean(gpu_nat), img_mean(gpu_nonat)
        ratio = m_nat / m_nonat if m_nonat > 0 else float("inf")
        record("T1 native on/off", 0.7 < ratio < 1.4,
               f"natives=4 mean={m_nat:.4f} natives=0 mean={m_nonat:.4f} "
               f"ratio={ratio:.3f} (bug: ~2x)")

        # T3: lt-only output must be finite and non-zero
        record("T3 finite+nonzero",
               np.isfinite(m_nat) and m_nat > 1e-4,
               f"mean={m_nat:.4f}")

        # T4: ordinary PATHOCL unaffected by the new branch
        eye_nat = render("PATHOCL", False, 4, gpu_sel)
        eye_nonat = render("PATHOCL", False, 0, gpu_sel)
        e_nat, e_nonat = img_mean(eye_nat), img_mean(eye_nonat)
        eratio = e_nat / e_nonat if e_nonat > 0 else float("inf")
        record("T4 eye path sane", 0.7 < eratio < 1.4,
               f"natives=4 mean={e_nat:.4f} natives=0 mean={e_nonat:.4f} "
               f"ratio={eratio:.3f}")
    else:
        record("T1 native on/off", False, "no GPU OpenCL device")
        record("T3 finite+nonzero", False, "no GPU OpenCL device")
        record("T4 eye path sane", False, "no GPU OpenCL device")
        m_nat = float("nan")

    # T2: LIGHTCPU reference, same scene same spp
    cpu_rgb = render("LIGHTCPU", True, 0)
    m_cpu = img_mean(cpu_rgb)
    if has_gpu:
        ratio = m_nat / m_cpu if m_cpu > 0 else float("inf")
        record("T2 vs LIGHTCPU", 0.5 < ratio < 2.0,
               f"GPU={m_nat:.4f} CPU={m_cpu:.4f} ratio={ratio:.3f} "
               f"(bug: ~2x)")
    else:
        record("T2 vs LIGHTCPU",
               np.isfinite(m_cpu) and m_cpu > 1e-4,
               f"LIGHTCPU mean={m_cpu:.4f} (no GPU to compare)")

    n_fail = sum(1 for _, ok in results if not ok)
    print(f"\n{len(results) - n_fail}/{len(results)} tests passed")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
