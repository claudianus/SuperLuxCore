# SPDX-License-Identifier: Apache-2.0
#
# E24: multi-specular MNEE chain with a directional (sharpdistant) endpoint.
#
# Regression coverage for the eye-side chain solver's directional endpoint
# support (pathtracer_mnee.cpp MneeEndpoint + pathoclbase_funcs.cl
# woIsDir/lightIsDir ports):
#
#   tinycaster.scn is a small glass sphere (r~0.08) under a sharpdistant
#   light over a matte floor. The camera->floor->glass->glass->sun path
#   needs a TWO-vertex manifold chain (entry + exit refraction through
#   the sphere) ending at a directional endpoint. Before the endpoint
#   refactor the chain solver reconstructed a fake finite lightPos 1 unit
#   from the receiver (directPdfW=1 for sharpdistant) and every solve
#   failed: the sphere's shadow interior measured exactly 0.
#
# Gates:
#   1. mnee=1 interior luminance must be > 0.5% of background (chain
#      solves and deposits energy); with mnee=0 the interior is ~0.
#   2. PATHOCL must match PATHCPU within noise (parity).
#   3. No NaN/inf in the film.
#
# Run from the repo root:
#   python3.13 dev-tools/e24_mnee_chain_distant_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 640, 360
SPP = 128
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300
SCENE = REPO / "scenes/mnee_dir/tinycaster.scn"

# Sphere footprint at 640x360. The float film is bottom-up vs the saved
# PNG: the ellipse sits at cx~319 cy~102 with radii rx~29 ry~17.
CX, CY, RX, RY = 319, 102, 29.0, 17.0


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
path.lighttracing.enable = 0
path.mnee.enable = 1
path.mnee.maxspecular = 2
path.mnee.maxiterations = 32
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
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def profile(img):
    lum = img.mean(axis=2)
    y, x = np.ogrid[:HEIGHT, :WIDTH]
    e = ((x - CX) / RX) ** 2 + ((y - CY) / RY) ** 2
    inner = lum[e < 0.36].mean()              # inside 60% of the ellipse
    mid = lum[(e >= 0.9) & (e < 1.7)].mean()  # edge/ring annulus
    bg = lum[e > 4.0].mean()
    return inner, mid, bg


def main():
    print(f"MNEE chain + directional endpoint ({WIDTH}x{HEIGHT} @ {SPP}spp)\n",
          flush=True)
    scene = parse_scene()

    cpu_on = render(scene, "PATHCPU", "")
    cpu_off = render(scene, "PATHCPU", "path.mnee.enable = 0\n")
    gpu_on = render(scene, "PATHOCL", "")

    ok = True

    for name, img in [("cpu-on", cpu_on), ("cpu-off", cpu_off),
                      ("gpu-on", gpu_on)]:
        finite = np.isfinite(img).all()
        ok &= finite
        print(f"[{'PASS' if finite else 'FAIL'}] {name}: film finite",
              flush=True)

    ci, cm, cb = profile(cpu_on)
    oi, om, ob = profile(cpu_off)
    gi, gm, gb = profile(gpu_on)

    # Chain deposits energy inside the shadow; mnee=0 leaves it ~black.
    good = (ci > 0.005 * cb) and (oi < 0.2 * ci)
    ok &= good
    print(f"[{'PASS' if good else 'FAIL'}] interior: "
          f"mnee-on={ci:.4f} mnee-off={oi:.4f} bg={cb:.4f}", flush=True)

    # CPU/GPU parity on the caustic profile.
    for tag, a, b in [("inner", ci, gi), ("mid", cm, gm), ("bg", cb, gb)]:
        ratio = b / max(a, 1e-6)
        good = 0.5 < ratio < 2.0
        ok &= good
        print(f"[{'PASS' if good else 'FAIL'}] parity-{tag}: "
              f"cpu={a:.4f} gpu={b:.4f} ratio={ratio:.2f}", flush=True)

    print(f"\n{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
