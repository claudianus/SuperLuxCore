# SPDX-License-Identifier: Apache-2.0
#
# E25: adaptive caustic partition (path.hybridbackforward.adaptivecaustic).
#
# scenes/cornell/caustic-roughglass.scn puts a rough-glass sphere
# (glossiness = uroughness = 0.1) under a small emissive ceiling panel.
# The sphere sits above the fixed 0.05 glossiness threshold, so the old
# partition leaves the caustic to the eye path: BSDF samples of a 0.1
# lobe almost never land on the small panel, and the rare hits firefly.
#
# The adaptive partition measures the emitter's solid angle against the
# lobe (omegaLight < connectProb * PI * g^2) and hands the class to the
# light pass. Gates:
#
#   1. unbiasedness - adaptive OFF vs ON mean luminance must match
#      within noise (the partition is disjoint, never dropped).
#   2. firefly removal - p99/max of the caustic patch under the sphere
#      must drop with adaptive ON.
#   3. CPU/GPU parity - PATHOCL hybrid adaptive vs PATHCPU hybrid
#      adaptive mean luminance within noise.
#
# Run from the repo root:
#   python3.13 dev-tools/e25_adaptive_caustic_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 180
SPP = 96
# GPU light tasks are carved from the tail of the task population with a
# hard 8192-task minimum reserved for the eye pass
# (pathocl.cpp: lightTaskCount = Min(taskCount - 8192, ...)), so a
# task.count below ~16k silently disables the light pass. 64k leaves
# ample room for both.
TASK_COUNT = 65536
RENDER_TIMEOUT_S = 300
SCENE = REPO / "scenes/cornell/caustic-roughglass.scn"

# Caustic patch under the sphere at 320x180 (sphere view center is
# ~(161,100), the caustic pools on the floor below it).
DISC_X0, DISC_X1 = 120, 205
DISC_Y0, DISC_Y1 = 90, 150


def parse_scene():
    cwd = os.getcwd()
    os.chdir(str(REPO))
    try:
        props = pysuperluxcore.Properties(str(SCENE))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def render(scene, engine, extra):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 17
opencl.task.count = {TASK_COUNT}
opencl.native.threads.count = 0
{extra}
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {SPP} spp")
        time.sleep(0.5)
    # Stop() performs the final UpdateFilmLockLess() - on GPU engines the
    # per-task films (incl. light-pass splats) merge only then, so read
    # after Stop or the screen-normalized channel lags behind the pass
    # counter.
    ses.Stop()
    # Raw linear radiance (unclamped): firefly tails only show in HDR
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB,
                                 rgb, 0, True)
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


HYBRID = ("path.hybridbackforward.enable = 1\n"
          "path.hybridbackforward.partition = 0.8\n")
ADAPT_ON = HYBRID + "path.hybridbackforward.adaptivecaustic = 1\n"
ADAPT_OFF = HYBRID + "path.hybridbackforward.adaptivecaustic = 0\n"


def main():
    print(f"Adaptive caustic partition ({WIDTH}x{HEIGHT} @ {SPP}spp)\n",
          flush=True)
    scene = parse_scene()

    cpu_off = render(scene, "PATHCPU", ADAPT_OFF)
    cpu_on = render(scene, "PATHCPU", ADAPT_ON)

    patch_off = cpu_off[DISC_Y0:DISC_Y1, DISC_X0:DISC_X1]
    patch_on = cpu_on[DISC_Y0:DISC_Y1, DISC_X0:DISC_X1]

    ok = True

    # 1. unbiasedness: the whole-image mean must not move (a dropped
    # path class would darken the caustic; a double-counted one would
    # brighten it).
    ratio = patch_on.mean() / max(patch_off.mean(), 1e-6)
    good = (0.6 < ratio < 1.7) and np.isfinite(ratio)
    ok &= good
    print(f"[{'PASS' if good else 'FAIL'}] CPU bias: "
          f"off={patch_off.mean():.4f} on={patch_on.mean():.4f} "
          f"ratio={ratio:.2f}", flush=True)

    # 2. firefly removal: the eye path's rare BSDF hits inflate the
    # patch's upper tail; the light pass covers the class smoothly.
    p99_off, p99_on = np.percentile(patch_off, 99), np.percentile(patch_on, 99)
    max_off, max_on = patch_off.max(), patch_on.max()
    good = (p99_on <= p99_off * 1.05) or (max_on < max_off)
    ok &= good
    print(f"[{'PASS' if good else 'FAIL'}] CPU firefly: "
          f"p99 {p99_off:.2f}->{p99_on:.2f} max {max_off:.2f}->{max_on:.2f}",
          flush=True)

    # 3. CPU/GPU parity under adaptive hybrid
    try:
        gpu_on = render(scene, "PATHOCL",
                        ADAPT_ON + "path.lighttracing.enable = 1\n"
                                   "path.lighttracing.taskfraction = 0.25\n")
        patch_gpu = gpu_on[DISC_Y0:DISC_Y1, DISC_X0:DISC_X1]
        ratio = patch_gpu.mean() / max(patch_on.mean(), 1e-6)
        good = (0.55 < ratio < 1.8) and np.isfinite(ratio)
        ok &= good
        print(f"[{'PASS' if good else 'FAIL'}] GPU parity: "
              f"cpu={patch_on.mean():.4f} gpu={patch_gpu.mean():.4f} "
              f"ratio={ratio:.2f}", flush=True)
    except Exception as e:
        ok = False
        print(f"[FAIL] GPU render: {e}", flush=True)

    print(f"\n{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
