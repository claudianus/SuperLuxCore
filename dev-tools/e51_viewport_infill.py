# SPDX-License-Identifier: Apache-2.0
#
# E51: viewport progressive coverage + VIEWPORT_INFILL reconstruction.
#
# Renders scenes/cornell/cornell.scn on RTPATHCPU and RTPATHOCL with two
# imagepipelines side by side:
#   000 = tonemap only (raw merged radiance)
#   001 = VIEWPORT_INFILL + tonemap (holes reconstructed via pull-push)
# Read back after the first passes land and assert the infill pipeline is
# near-fully populated while raw coverage is still sparse - i.e. the
# viewport shows a coherent frame immediately instead of stale blocks.
#
# Also checks RTPATHOCL's lattice coverage: early passes must scatter
# samples across the whole frame (no giant contiguous empty quadrants),
# which is what the old grid+Morton order failed at.
#
# Run from the repo root after a Release build:
#   python3.13 dev-tools/e51_viewport_infill.py
#
# Env: SUPERLUXCORE_BACKENDS=cpu,opencl,metal (leg subset, default all).

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for cfg in ("Release", "Debug"):
    p = REPO / f"out/build/src/pysuperluxcore/{cfg}"
    if (p / "pysuperluxcore.cpython-313-darwin.so").exists():
        sys.path.insert(0, str(p))
        break
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
BOOT_TIMEOUT_S = 240          # kernel JIT / tile repo startup budget

BACKENDS = {b.strip() for b in
        os.environ.get("SUPERLUXCORE_BACKENDS", "cpu,opencl,metal").split(",")
        if b.strip()}


def device_mask(want_type):
    pysuperluxcore.Init()
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


def parse_scene(rel_path):
    try:
        props = pysuperluxcore.Properties(str(REPO / rel_path))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    except Exception:
        pass
    cwd = os.getcwd()
    os.chdir(str(REPO / Path(rel_path).parent))
    try:
        props = pysuperluxcore.Properties(str(Path(rel_path).name))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def nonzero_frac(img):
    lum = img.mean(axis=2)
    return float((lum > 0.001).mean()), lum


def read_pipeline(ses, index, width, height):
    rgb = np.empty(width * height * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, index, True)
    return rgb.reshape(height, width, 3)


def run_leg(scene, engine, sampler, sel=None, width=WIDTH, height=HEIGHT,
            extra_props=""):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {width}
film.height = {height}
renderengine.type = {engine}
sampler.type = {sampler}
{extra_props}
film.imagepipelines.000.0.type = TONEMAP_LINEAR
film.imagepipelines.000.0.scale = 1
film.imagepipelines.001.0.type = VIEWPORT_INFILL
film.imagepipelines.001.1.type = TONEMAP_LINEAR
film.imagepipelines.001.1.scale = 1
renderengine.seed = 17
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()

    # Measure at the first possible instant: poll the pass counter until
    # the first pass has landed, then read both pipelines back to back.
    # At 720p a GPU pass covers ~1.6% (preview) / 6.25% (steady) of the
    # frame, so the raw buffer is still sparse when we catch it.
    deadline = time.monotonic() + BOOT_TIMEOUT_S
    while True:
        ses.UpdateStats()
        try:
            passes = ses.GetStats().Get("stats.renderengine.pass").GetInt()
        except Exception:
            passes = 0
        if passes >= 1:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"{engine}: no pass within {BOOT_TIMEOUT_S}s")
        time.sleep(0.05)

    raw = read_pipeline(ses, 0, width, height)
    filled = read_pipeline(ses, 1, width, height)
    ses.Stop()

    raw_frac, raw_lum = nonzero_frac(raw)
    fill_frac, fill_lum = nonzero_frac(filled)

    # Spatial scatter check: split into 4x4 cells; a block-fill order would
    # cover a few cells fully and leave most exactly zero. The lattice
    # should touch most cells even at very low total coverage.
    cell_nonzero = []
    bh, bw = height // 4, width // 4
    for cy in range(4):
        for cx in range(4):
            cell = raw_lum[cy*bh:(cy+1)*bh, cx*bw:(cx+1)*bw]
            cell_nonzero.append((cell > 0.001).mean() > 0.0)
    cells_touched = sum(cell_nonzero)

    finite = np.isfinite(raw).all() and np.isfinite(filled).all()
    # Infill must never lose coverage the raw buffer has.
    monotonic = fill_frac >= raw_frac - 0.001
    # A sane fill tracks the raw luminance scale (no garbage colours).
    raw_mean = raw_lum[raw_lum > 0.001].mean() if (raw_lum > 0.001).any() else 0.0
    fill_mean = fill_lum[fill_lum > 0.001].mean() if (fill_lum > 0.001).any() else 0.0
    sane = raw_mean <= 0 or (0.2 <= fill_mean / raw_mean <= 5.0)
    if engine == "RTPATHOCL":
        # GPU is where the stale-block bug lived: passes are slow enough
        # that the first frame is still sparse - infill must close the gap.
        reconstructed = fill_frac >= raw_frac + 0.25
        coherent = fill_frac > 0.85
        scattered = cells_touched >= 12
        ok = finite and monotonic and sane and reconstructed and coherent and scattered
    else:
        # CPU sweeps too fast to reliably catch the sparse window; assert
        # correctness invariants instead of a minimum reconstruction gap.
        ok = finite and monotonic and sane and cells_touched >= 12
        reconstructed = coherent = scattered = True
    print(f"[{'PASS' if ok else 'FAIL'}] {engine}: "
          f"raw={raw_frac:.3f} infill={fill_frac:.3f} "
          f"cells={cells_touched}/16 finite={finite} sane={sane}")
    if not ok:
        print(f"       monotonic={monotonic} reconstructed(+25pp)={reconstructed} "
              f"coherent(>85%)={coherent} scattered(>=12/16)={scattered}")
    return ok


def main():
    scene = parse_scene("scenes/cornell/cornell.scn")
    ok_all = True

    if "cpu" in BACKENDS:
        ok_all &= run_leg(scene, "RTPATHCPU", "RTPATHCPUSAMPLER")
        # BIDIR viewport path: infill is engine-agnostic - the pull-push
        # reconstruction reads radiance weights, which bidir writes too.
        ok_all &= run_leg(scene, "BIDIRCPU", "SOBOL")

    for dtype, tag in (("METAL_GPU", "metal"), ("OPENCL_GPU", "opencl")):
        if tag not in BACKENDS:
            continue
        mask = device_mask(dtype)
        if mask:
            ok_all &= run_leg(scene, "RTPATHOCL", "TILEPATHSAMPLER", mask,
                              width=1280, height=720)
            # Default viewport path: PATHOCL with GPU light tracing on.
            # Infill must still produce a coherent finite frame when
            # screen-normalized light splats mix with eye coverage.
            ok_all &= run_leg(scene, "PATHOCL", "SOBOL", mask,
                              width=1280, height=720,
                              extra_props="""
path.lighttracing.enable = true
path.lighttracing.taskfraction = 0.3
""")

    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
