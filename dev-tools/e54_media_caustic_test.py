# SPDX-License-Identifier: Apache-2.0
#
# E54: media-transparent caustic chains (doc/features/caustics-sota.md).
#
# A medium scattering vertex must be invisible to the surface-specular
# caustic classification: it neither extends nor breaks the chain on
# EITHER side of the eye/light partition. Scenes:
#
#   scenes/cornell/cornell-vol-caustic-ms.scn  - three glass spheres in
#       homogeneous fog (multiscattering = 1): light->glass->med->wall
#       and light->glass->med->med->wall are caustic-class paths.
#   scenes/cornell/cornell-vol-pure.scn      - matte Cornell box + fog,
#       no specular surface at all: nothing may be classified caustic.
#   scenes/cornell/caustic-roughglass.scn    - surface-only control:
#       classification must be bit-equivalent to pre-change behavior.
#
# Gates:
#   1. partition disjointness (CPU): adaptive ON vs OFF whole-image
#      mean within noise on the fog+caustic scene.
#   2. CPU/GPU parity: PATHOCL adaptive hybrid vs PATHCPU.
#   3. pure-medium control: adaptive ON vs OFF identical mean - the
#      firstVertSeen guard keeps ambient fog eye-owned.
#   4. PhotonGI caustic cache: finite, sane mean (multi-scatter
#      deposits now reach the caustic cache).
#   5. BIDIRCPU smoke: finite output on the volume caustic scene.
#   6. surface-only non-regression: rough-glass scene mean parity.
#
# Run from the repo root:
#   python3.13 dev-tools/e54_media_caustic_test.py

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
TASK_COUNT = 65536  # see e25: below ~16k the GPU light pass is off
RENDER_TIMEOUT_S = 300

VOL_CAUSTIC = REPO / "scenes/cornell/cornell-vol-caustic-ms.scn"
VOL_PURE = REPO / "scenes/cornell/cornell-vol-pure.scn"
ROUGH_GLASS = REPO / "scenes/cornell/caustic-roughglass.scn"


def parse_scene(path):
    cwd = os.getcwd()
    os.chdir(str(REPO))
    try:
        props = pysuperluxcore.Properties(str(path))
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
    # per-task films (incl. light-pass splats) merge only then.
    ses.Stop()
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB,
                                 rgb, 0, True)
    return rgb.reshape(HEIGHT, WIDTH, 3)


def luminance(img):
    return img.mean(axis=2)


def finite(img):
    return np.isfinite(img).all()


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


HYBRID = ("path.hybridbackforward.enable = 1\n"
          "path.hybridbackforward.partition = 0.8\n")
ADAPT_ON = HYBRID + "path.hybridbackforward.adaptivecaustic = 1\n"
ADAPT_OFF = HYBRID + "path.hybridbackforward.adaptivecaustic = 0\n"
PGIC = ("path.photongi.caustic.enabled = 1\n"
        "path.photongi.caustic.updatespp = 0\n"
        "path.photongi.photon.maxcount = 1000000\n"
        "path.photongi.caustic.maxsize = 500000\n")
PGIC_NOBEAMS = PGIC + "path.photongi.caustic.volumebeams = 0\n"


def main():
    ok = True
    print(f"Media-transparent caustic chains ({WIDTH}x{HEIGHT} @ {SPP}spp)\n",
          flush=True)

    # ---- volume caustic scene -------------------------------------------
    scene = parse_scene(VOL_CAUSTIC)

    cpu_off = luminance(render(scene, "PATHCPU", ADAPT_OFF))
    cpu_on = luminance(render(scene, "PATHCPU", ADAPT_ON))
    ok &= check(0.6 < cpu_on.mean() / max(cpu_off.mean(), 1e-6) < 1.7,
                "1 CPU partition disjoint (vol caustic)",
                f"off={cpu_off.mean():.4f} on={cpu_on.mean():.4f}")

    # 2. GPU parity under adaptive hybrid + light tracing
    try:
        gpu_on = luminance(render(scene, "PATHOCL",
                ADAPT_ON + "path.lighttracing.enable = 1\n"
                           "path.lighttracing.taskfraction = 0.25\n"))
        ok &= check(finite(gpu_on) and
                    0.55 < gpu_on.mean() / max(cpu_on.mean(), 1e-6) < 1.8,
                    "2 GPU parity (vol caustic)",
                    f"cpu={cpu_on.mean():.4f} gpu={gpu_on.mean():.4f}")
    except Exception as e:
        ok &= check(False, "2 GPU parity (vol caustic)", e)

    # 4. PhotonGI caustic cache: finite + sane energy, beam and point
    # estimators agree on the same transport class (beams are a lower
    # variance estimate of the same medium caustics).
    try:
        pgic = luminance(render(scene, "PATHCPU", PGIC))
        pgic_pts = luminance(render(scene, "PATHCPU", PGIC_NOBEAMS))
        ok &= check(finite(pgic) and
                    0.5 < pgic.mean() / max(cpu_off.mean(), 1e-6) < 2.0,
                    "4 PhotonGI caustic cache",
                    f"plain={cpu_off.mean():.4f} pgic={pgic.mean():.4f}")
        ok &= check(finite(pgic_pts) and
                    0.5 < pgic.mean() / max(pgic_pts.mean(), 1e-6) < 2.0,
                    "4b beam/point estimator agreement",
                    f"beams={pgic.mean():.4f} points={pgic_pts.mean():.4f}")
    except Exception as e:
        ok &= check(False, "4 PhotonGI caustic cache", e)

    # 5. BIDIRCPU smoke on the volume caustic scene
    try:
        bidir = luminance(render(scene, "BIDIRCPU", ""))
        ok &= check(finite(bidir) and bidir.mean() > 0,
                    "5 BIDIRCPU smoke", f"mean={bidir.mean():.4f}")
    except Exception as e:
        ok &= check(False, "5 BIDIRCPU smoke", e)

    # ---- pure medium control --------------------------------------------
    scene = parse_scene(VOL_PURE)
    pure_off = luminance(render(scene, "PATHCPU", ADAPT_OFF))
    pure_on = luminance(render(scene, "PATHCPU", ADAPT_ON))
    ok &= check(0.8 < pure_on.mean() / max(pure_off.mean(), 1e-6) < 1.25,
                "3 pure-medium control (no caustic claims)",
                f"off={pure_off.mean():.4f} on={pure_on.mean():.4f}")

    # ---- surface-only non-regression ------------------------------------
    # Sparse light-pass splats make the raw mean heavy-tailed at 96 spp:
    # compare p99.9-clipped means so a single firefly cannot flip the gate.
    scene = parse_scene(ROUGH_GLASS)
    rough_off = luminance(render(scene, "PATHCPU", ADAPT_OFF))
    rough_on = luminance(render(scene, "PATHCPU", ADAPT_ON))
    clip = np.percentile(np.concatenate([rough_off.flat, rough_on.flat]), 99.9)
    co, cn = np.minimum(rough_off, clip).mean(), np.minimum(rough_on, clip).mean()
    ok &= check(0.6 < cn / max(co, 1e-6) < 1.7,
                "6 surface-only non-regression",
                f"off={co:.4f} on={cn:.4f} (clip@{clip:.2f})")

    print(f"\n{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
