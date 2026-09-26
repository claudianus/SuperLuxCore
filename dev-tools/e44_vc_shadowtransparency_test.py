# SPDX-License-Identifier: Apache-2.0
#
# E44: Vertex-connection MIS lift over shadow-transparent stacks.
#
# Regression for a GPU-only white-blowout found on the GenmaB atrium
# scene (multi-layer glass facade):
#
#   PATHOCL's AdvancePaths_MK_RT_DL runs once per shadow-ray SEGMENT.
#   When a shadow ray crosses a shadow-transparent surface (archglass,
#   GetPassThroughShadowTransparency), the segment continues and the
#   kernel re-executes for the next segment. The VC MIS "lift"
#   (lightRadiance /= vcMisWeight, mirroring CPU's misWeight=1 after the
#   full shadow walk in bidircputhread DirectLightSampling) was applied
#   on EVERY intermediate segment, so a ray crossing N transparent
#   panes compounded (1/vcMisWeight)^N (~1e4..1e5 per pane) -> uniform
#   white field. CPU applies it once after the complete walk; GPU now
#   gates the lift on the terminating segment (!continueToTrace).
#
# Scene: emissive ceiling cube (mesh light -> non-trivial vcMisWeight),
# matte receiver box, and 4 stacked archglass panes between them so
# every NEE shadow ray crosses 4 shadow-transparent surfaces.
#
#   T1 GPU finite:   PATHOCL+VC receiver median luminance is finite
#   T2 GPU bounded:  receiver median < absolute sanity bound
#                    (buggy build: ~1e4..1e9)
#   T3 CPU/GPU par:  PATHOCL receiver median within 3x of BIDIRCPU
#                    (same scene, same spp; the invariant the bug broke)
#   T4 1 vs 4 panes: more panes must not increase receiver luminance
#                    beyond the archglass kt^N ratio -- with the bug
#                    each extra pane multiplied by 1/vcMisWeight
#
# Run:
#   python3.13 dev-tools/e44_vc_shadowtransparency_test.py
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
    """'1' for every hardware GPU device (METAL_GPU, VULKAN_GPU,
    OPENCL_GPU, CUDA_GPU, ...), '0' otherwise."""
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    types = []
    i = 0
    while True:
        try:
            types.append(descs.Get(f"opencl.device.{i}.type").GetString())
        except Exception:
            break
        i += 1
    # Prefer the Metal device; on this platform the Vulkan backend fails
    # kernel compilation for PATHOCL
    for want in ("METAL_GPU", "OPENCL_GPU", "CUDA_GPU", "VULKAN_GPU"):
        if want in types:
            return "".join("1" if t == want else "0" for t in types)
    return None


def scale_translate(sx, sy, sz, tx, ty, tz):
    return (f"{sx} 0 0 0  0 {sy} 0 0  0 0 {sz} 0  {tx} {ty} {tz} 1")


def scene(n_panes):
    """Camera looks at a matte wall behind a stack of thin archglass
    panes; a bright emissive ceiling cube is the only light, so every
    direct-lighting shadow ray to the wall crosses all panes."""
    panes = ""
    for i in range(n_panes):
        panes += (f'scene.objects.Pane{i}.material = Glass\n'
                  f'scene.objects.Pane{i}.ply = {PLY}\n'
                  f'scene.objects.Pane{i}.transformation = '
                  f'{scale_translate(6, 6, 0.02, -3, -3, 0.6 + 0.35 * i)}\n'
                  f'scene.objects.Pane{i}.id = {30 + i}\n')
    return f"""
scene.camera.type = perspective
scene.camera.lookat.orig = 0 -7 1.8
scene.camera.lookat.target = 0 0 1.6
scene.camera.fieldofview = 40

scene.materials.Wall.type = matte
scene.materials.Wall.kd = 0.7 0.7 0.7
scene.materials.Glass.type = archglass
scene.materials.Glass.kr = 0.05 0.05 0.05
scene.materials.Glass.kt = 0.95 0.95 0.95
scene.materials.Glass.interiorior = 1.5
scene.materials.Glass.exteriorior = 1.0
scene.materials.Emit.type = matte
scene.materials.Emit.kd = 1 1 1
scene.materials.Emit.emission = 60 60 60

# back wall, fills the frame (receiver)
scene.objects.Wall.material = Wall
scene.objects.Wall.ply = {PLY}
scene.objects.Wall.transformation = {scale_translate(6, 0.1, 6, -3, 1.5, -1)}
scene.objects.Wall.id = 10

# emissive ceiling cube -> mesh light, gives vcMisWeight << 1
scene.objects.Emit.material = Emit
scene.objects.Emit.ply = {PLY}
scene.objects.Emit.transformation = {scale_translate(2, 2, 0.2, -1, -1, 3.6)}
scene.objects.Emit.id = 20

{panes}"""


def render(scene_props, engine, sel=None):
    scn = pysuperluxcore.Properties()
    scn.SetFromString(scene_props)
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
path.pathdepth.total = 8
path.vertexconnection.enable = 1
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.1.type = OBJECT_ID
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
    oid = np.empty(WIDTH * HEIGHT, dtype=np.uint32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB, rgb)
    film.GetOutputUInt(pysuperluxcore.FilmOutputType.OBJECT_ID, oid)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3), oid.reshape(HEIGHT, WIDTH)


def wall_median(rgb, oid):
    m = oid == 10
    if not m.any():
        return float("nan"), 0
    return float(np.median(rgb.mean(axis=2)[m])), int(m.sum())


def main():
    gpu_sel = device_mask()
    has_gpu = gpu_sel is not None

    # T1/T2: GPU render through 4 shadow-transparent panes
    if has_gpu:
        gpu_rgb, gpu_oid = render(scene(4), "PATHOCL", gpu_sel)
        gpu_med, gpu_px = wall_median(gpu_rgb, gpu_oid)
        record("T1 GPU finite", np.isfinite(gpu_med),
               f"median={gpu_med:.3f} px={gpu_px}")
        # Expected order of magnitude: a few units (emission 60 at a
        # few metres, kt^4 ~ 0.8). The bug produced >= 1e4.
        record("T2 GPU bounded", gpu_med < 100.0,
               f"median={gpu_med:.3f} (bug: ~1e4..1e9)")

        # T4: 1 pane vs 4 panes -- stacking panes must not blow up.
        # kt=0.95 => 4 panes dim the wall slightly, never brighten it.
        gpu1_rgb, gpu1_oid = render(scene(1), "PATHOCL", gpu_sel)
        gpu1_med, _ = wall_median(gpu1_rgb, gpu1_oid)
        record("T4 pane stack", gpu_med < gpu1_med * 4.0,
               f"1-pane={gpu1_med:.3f} 4-pane={gpu_med:.3f}")
    else:
        record("T1 GPU finite", False, "no GPU OpenCL device")
        record("T2 GPU bounded", False, "no GPU OpenCL device")
        record("T4 pane stack", False, "no GPU OpenCL device")

    # T3: BIDIRCPU reference on the same 4-pane scene, same spp.
    cpu_rgb, cpu_oid = render(scene(4), "BIDIRCPU")
    cpu_med, cpu_px = wall_median(cpu_rgb, cpu_oid)
    if has_gpu:
        ratio = gpu_med / cpu_med if cpu_med > 0 else float("inf")
        record("T3 CPU/GPU parity", 0.33 < ratio < 3.0,
               f"GPU={gpu_med:.3f} CPU={cpu_med:.3f} ratio={ratio:.2f}")
    else:
        record("T3 CPU/GPU parity", np.isfinite(cpu_med) and cpu_med < 100.0,
               f"CPU median={cpu_med:.3f} (no GPU to compare)")

    n_fail = sum(1 for _, ok in results if not ok)
    print(f"\n{len(results) - n_fail}/{len(results)} tests passed")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
