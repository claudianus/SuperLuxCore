# SPDX-License-Identifier: Apache-2.0
#
# E20: Metal image-map texel fetch parity (vload_half offset bug).
#
# The cl2msl.py shim for vload_half(o, p) ignored the element offset and
# always returned p[0]. HALF-storage image maps (all .exr inputs) were
# therefore sampled as a single constant texel on Metal, rendering
# infinite-light and emissive-texture images flat and ~40% dark.
# OpenCL/CUDA/CPU implement vload_half correctly, so the bug was
# Metal-only. The vload2/3/4 and vstore2/3/4/half shims had the same
# offset-ignoring defect; all in-tree call sites used offset 0 so only
# vload_half (imagemap_funcs.cl) was observably wrong.
#
# T1 (HALF env): sky.exr infinite light — Metal vs OpenCL pixel parity
#    and both within 5% of the PATHCPU reference mean.
# T2 (FLOAT env): image.png infinite light — same gates, exercises the
#    non-HALF storage branch.
# T3: all outputs finite.
#
# The Metal leg skips cleanly on hosts without a METAL_GPU device.
#
# Run:
#   python3.13 dev-tools/e20_metal_imagemap_test.py

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
import pyluxcore

WIDTH, HEIGHT = 160, 120
SPP = 16
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 180

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def device_mask(want_type):
    """Build an opencl.devices.select mask enabling only `want_type`."""
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


def render(image_file, sel=None, engine="PATHOCL", spp=SPP, seed=17):
    scn = pyluxcore.Properties()
    scn.SetFromString(f"""
scene.camera.cliphither = 0.001
scene.camera.lookat.orig = 0 0 0
scene.camera.lookat.target = 0 0 -1
scene.camera.up = 0 1 0
scene.camera.screenwindow = -1 1 -0.75 0.75
scene.infinitelight.file = {REPO / "scenes" / "simple-mat" / image_file}
scene.infinitelight.gain = 1.0 1.0 1.0
""")
    scene = pyluxcore.Scene()
    scene.Parse(scn)

    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
""")
    if sel:
        cfg.Set(pyluxcore.Property("opencl.devices.select", sel))

    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(
                f"render stalled below {spp} spp after {RENDER_TIMEOUT_S}s")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    # Parity gates are only meaningful if the leg actually ran on the
    # selected backend - check the per-device render stats keys.
    used = {n.split("stats.renderengine.devices.")[1].rsplit("-", 1)[0]
            for n in ses.GetStats().GetAllNames()
            if n.startswith("stats.renderengine.devices.")}
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3), used


def parity_check(tag, ocl, mtl, cpu):
    """Assert MTL reproduces the OCL image, not just its mean."""
    r = mtl / np.maximum(ocl, 1e-9)
    p50, p99 = float(np.percentile(r, 50)), float(np.percentile(r, 99))
    record(f"{tag}.metal-vs-opencl-mean",
           abs(float(mtl.mean()) - float(ocl.mean())) /
           max(float(ocl.mean()), 1e-12) < 0.03,
           f"MTL={mtl.mean():.6f} OCL={ocl.mean():.6f} (gate 3%)")
    # The buggy build produced a constant image: the mean gate above
    # alone would not catch a flat result that happened to match. The
    # per-pixel ratio spread does: a correct port reproduces the image
    # pixel-for-pixel (same deterministic sampler), while the constant
    # gives ratios proportional to 1/OCL (p50 ~1 but wide spread).
    record(f"{tag}.metal-vs-opencl-pixel",
           0.97 < p50 < 1.03 and 0.90 < p99 < 1.15,
           f"per-pixel ratio p50={p50:.3f} p99={p99:.3f} "
           f"(gates 0.97-1.03 / 0.90-1.15)")
    record(f"{tag}.metal-vs-cpu-mean",
           abs(float(mtl.mean()) - float(cpu.mean())) /
           max(float(cpu.mean()), 1e-12) < 0.05,
           f"MTL={mtl.mean():.6f} CPU={cpu.mean():.6f} (gate 5%)")
    record(f"{tag}.finite",
           np.isfinite(mtl).all() and np.isfinite(ocl).all(),
           "all pixels finite")


def main():
    print("Metal image-map texel fetch parity "
          "(160x120, infinite-light only)\n", flush=True)

    ocl_mask = device_mask("OPENCL_GPU")
    mtl_mask = device_mask("METAL_GPU")
    print(f"  devices: OPENCL_GPU={ocl_mask} METAL_GPU={mtl_mask}",
          flush=True)
    if not ocl_mask or not mtl_mask:
        print("SKIP: need both an OpenCL GPU and a Metal GPU device",
              flush=True)
        return

    for image_file, tag in (("sky.exr", "T1.half-exr"),
                            ("image.png", "T2.float-png")):
        cpu, _ = render(image_file, engine="PATHCPU", spp=SPP)
        ocl, ocl_dev = render(image_file, sel=ocl_mask)
        mtl, mtl_dev = render(image_file, sel=mtl_mask)
        print(f"  [{tag}] cpu={cpu.mean():.6f} ocl={ocl.mean():.6f} "
              f"mtl={mtl.mean():.6f}", flush=True)
        print(f"       ocl devices={sorted(ocl_dev)} "
              f"mtl devices={sorted(mtl_dev)}", flush=True)
        record(f"{tag}.device-assert",
               any("Metal" in d for d in mtl_dev) and
               any("OpenCL" in d for d in ocl_dev),
               "Metal leg must run on MetalIntersect, OpenCL leg on an "
               "OpenCL intersect device")
        parity_check(tag, ocl, mtl, cpu)


if __name__ == "__main__":
    pyluxcore.Init()
    main()
    failed = [n for n, ok in results if not ok]
    print(f"\n{'FAIL ' + str(failed) if failed else 'ALL PASS'} "
          f"({len(results) - len(failed)}/{len(results)})", flush=True)
    sys.exit(1 if failed else 0)
