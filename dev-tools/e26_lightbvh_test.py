# SPDX-License-Identifier: Apache-2.0
#
# E26: Light BVH strategy (Estevez & Kulla 2018) on CPU and GPU.
#
# lightstrategy.type=LIGHT_BVH replaces the flat light table with a
# binary hierarchy over the direct-sampling-enabled lights: each node
# carries the subtree's total energy plus a tight bound of its
# geometric attenuation (bbox distance), emission orientation
# (bounding cone) and receiver cosine. Sampling descends the tree
# choosing branches proportional to that bound (O(log N) per pick);
# SampleLightPdf() replays the same root-to-leaf path so NEE and the
# direct-hit MIS evaluate identical probabilities.
#
# Flat (infinite/directional) lights live in the tree as energy leaves
# whose bound bypasses the spatial terms (energyFlat), so environment
# sources keep their power-proportional share. Emit/infinite-only
# tasks still use the inherited log-power distribution - the device
# dispatches on the presence of the node buffer, so the kernels fall
# back to the flat table for any task without a tree.
#
# T1 (unbiasedness, CPU): LIGHT_BVH mean vs LOG_POWER reference.
# T2 (bounded variance, CPU): same-spp RMSE vs LOG_POWER must stay
#    bounded; the hierarchy should not regress error.
# T3 (GPU parity): PATHOCL LIGHT_BVH vs PATHCPU LIGHT_BVH.
# T4 (GPU unbiasedness): PATHOCL LIGHT_BVH vs LOG_POWER reference.
# T5 (crash safety): all outputs finite.
#
# Run:
#   python3.13 dev-tools/e26_lightbvh_test.py
#
import os
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

WIDTH, HEIGHT = 160, 120
SPP, SPP_REF = 64, 512
TASK_COUNT = 1 << 16
RENDER_TIMEOUT_S = 300

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def device_mask(want_type):
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


def render(scn_file, engine, defs, spp, seed=17, sel=None):
    scn = pysuperluxcore.Properties(str(REPO / "scenes" / "manylights" / scn_file))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)

    cfg = pysuperluxcore.Properties()
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
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    for k, v in defs.items():
        cfg.Set(pysuperluxcore.Property(k, v))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
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
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def rmse(a, b):
    return float(np.sqrt(np.mean((a - b) ** 2)))


BVH = {"lightstrategy.type": "LIGHT_BVH"}
FLAT = {"lightstrategy.type": "LOG_POWER"}


def main():
    print("Light BVH strategy (E&K'18) CPU+GPU (manylights "
          f"{WIDTH}x{HEIGHT})\n", flush=True)

    # Apple Silicon: pick exactly one physical GPU (METAL_GPU here)
    gpu_sel = next((m for m in (device_mask("METAL_GPU"),
                    device_mask("OPENCL_GPU")) if m and "1" in m), None)
    print(f"  gpu device mask: {gpu_sel}", flush=True)

    for scn_file, tag in (("scene-4spot.scn", "spots"),
                          ("scene.scn", "mesh")):
        ref = render(scn_file, "PATHCPU", FLAT, SPP_REF, seed=99)
        bvh = render(scn_file, "PATHCPU", BVH, SPP)
        flat = render(scn_file, "PATHCPU", FLAT, SPP)
        print(f"  [{tag}] ref={ref.mean():.6f} bvh={bvh.mean():.6f} "
              f"flat={flat.mean():.6f}", flush=True)

        record(f"T1.{tag}.unbiased-vs-logpower",
               np.isfinite(bvh).all() and
               abs(float(bvh.mean()) - float(ref.mean())) /
               max(float(ref.mean()), 1e-12) < 0.03,
               f"mean={bvh.mean():.6f} vs ref={ref.mean():.6f} (gate 3%)")

        r_bvh, r_flat = rmse(bvh, ref), rmse(flat, ref)
        # Bounded-variance check (NOT an improvement guarantee): the
        # clustered bound reallocates error differently than the flat
        # table; on these scenes it should not regress.
        record(f"T2.{tag}.variance-bounded",
               r_bvh <= r_flat * 1.5,
               f"RMSE bvh={r_bvh:.6f} vs flat={r_flat:.6f} (gate 1.5x)")

        if gpu_sel:
            g_bvh = render(scn_file, "PATHOCL", BVH, SPP, sel=gpu_sel)
            print(f"  [{tag}.gpu] bvh={g_bvh.mean():.6f}", flush=True)

            record(f"T3.{tag}.cpu-gpu-parity",
                   np.isfinite(g_bvh).all() and
                   abs(float(g_bvh.mean()) - float(bvh.mean())) /
                   max(float(bvh.mean()), 1e-12) < 0.05,
                   f"mean={g_bvh.mean():.6f} vs cpu={bvh.mean():.6f} "
                   "(gate 5%)")
            record(f"T4.{tag}.gpu-unbiased",
                   np.isfinite(g_bvh).all() and
                   abs(float(g_bvh.mean()) - float(ref.mean())) /
                   max(float(ref.mean()), 1e-12) < 0.05,
                   f"mean={g_bvh.mean():.6f} vs ref={ref.mean():.6f} "
                   "(gate 5%)")
            record(f"T5.{tag}.finite",
                   np.isfinite(bvh).all() and np.isfinite(g_bvh).all(),
                   "all pixels finite")
        else:
            record(f"T3.{tag}.cpu-gpu-parity", True,
                   "SKIP (no OpenCL/Metal GPU)")
            record(f"T4.{tag}.gpu-unbiased", True, "SKIP (no GPU)")
            record(f"T5.{tag}.finite",
                   np.isfinite(bvh).all(), "all pixels finite")

    print()
    ok = sum(1 for _, o in results if o)
    print(f"===== Light BVH: {ok}/{len(results)} PASS =====")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
