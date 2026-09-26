# SPDX-License-Identifier: Apache-2.0
#
# E23: LMNEE light-tracing parity on a dispersive glass caster.
#
# Regression coverage for the GPU light-side manifold path:
#   lt-dispersion.scn places the camera below the pedestal so the disc
#   receivers face AWAY from the lens - the straight receiver->lens BSDF
#   eval is black there and only a refracted manifold through the sphere
#   can connect them. This exercises two code paths that silently
#   regressed once before:
#
#   1. The connect probe: a black bsdfEval must still queue the
#      visibility ray so a delta occluder can start LMNEE
#      (pathtracer.cpp ConnectToEye + pathoclbase_kernels_micro.cl).
#      Without it the upper disc is simply never attempted.
#   2. The light-side chain line search: MS_TRIAL->MS_COMMIT must keep
#      the beta that produced the validated trial positions until the
#      commit re-projections consumed them (pathoclbase_funcs.cl).
#      Doubling beta before the commit made every commit re-projection
#      target a 2x-displaced, off-surface point - chains never solved.
#
# Gate: GPU (PATHOCL lighttracing.only) vs CPU (LIGHTCPU) mean luminance
# inside the sphere-view disc, split into upper/lower halves. Before the
# fixes the GPU upper half was ~3x darker than CPU while the lower half
# matched; afterwards both halves sit within noise of parity.
#
# Run from the repo root:
#   python3.13 dev-tools/e23_lmnee_lighttracing_parity_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Release"))
import pyluxcore

WIDTH, HEIGHT = 320, 180
SPP = 128
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300
SCENE = REPO / "scenes/cornell/lt-dispersion.scn"

# Disc bounds at 320x180 (sphere view center ~161,100, radius ~35).
# Upper half is where the pedestal-top receivers project - the region
# the missing probe and the beta bug blacked out.
DISC_X0, DISC_X1 = 128, 196
DISC_YT, DISC_YM, DISC_YB = 66, 100, 134  # top-down rows


def parse_scene():
    # The scene references meshes as scenes/cornell/*.ply - repo root cwd
    cwd = os.getcwd()
    os.chdir(str(REPO))
    try:
        props = pyluxcore.Properties(str(SCENE))
        scene = pyluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def render(scene, engine, extra):
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 17
opencl.task.count = {TASK_COUNT}
path.spectral.enable = 1
path.mnee.enable = 1
path.mnee.maxspecular = 4
path.mnee.maxiterations = 64
opencl.native.threads.count = 0
{extra}
""")
    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
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
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def region(img, y0, y1):
    return img[y0:y1, DISC_X0:DISC_X1].mean()


def main():
    print(f"LMNEE light-tracing parity ({WIDTH}x{HEIGHT} @ {SPP}spp)\n",
          flush=True)
    scene = parse_scene()

    cpu = render(scene, "LIGHTCPU", "")
    gpu = render(scene, "PATHOCL",
                 "path.lighttracing.enable = 1\n"
                 "path.lighttracing.taskfraction = 1.0\n"
                 "path.lighttracing.only = 1\n")

    ok = True
    for name, y0, y1 in [("upper", DISC_YT, DISC_YM),
                         ("lower", DISC_YM, DISC_YB),
                         ("all", DISC_YT, DISC_YB)]:
        cm, gm = region(cpu, y0, y1), region(gpu, y0, y1)
        ratio = gm / max(cm, 1e-6)
        # The wedge defect produced ratio ~0.3 in the upper half while
        # lower stayed near 1; a healthy build sits within noise of 1.
        good = (ratio > 0.55) and (ratio < 1.8) and np.isfinite(gm)
        ok &= good
        print(f"[{'PASS' if good else 'FAIL'}] disc-{name}: "
              f"cpu={cm:.4f} gpu={gm:.4f} ratio={ratio:.2f}", flush=True)

    print(f"\n{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
