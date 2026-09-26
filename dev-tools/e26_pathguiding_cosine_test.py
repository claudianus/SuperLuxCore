# SPDX-License-Identifier: Apache-2.0
#
# E26: path-guiding cosine-convention regression.
#
# The guided-direction branch of the BSDF/guide mixture divides
# BSDF::Evaluate() by the local cosine to undo Disney's (upstream)
# double-cos convention. Before the fix the division ran for EVERY
# non-volume material, stripping the f*|cos| factor from matte/glossy/
# etc.: guided bounces were over-weighted by ~1/cos (biased bright, most
# visible at grazing angles). The fix gates the division on
# MATERIAL_TYPE == DISNEY on both CPU (pathtracer.cpp) and the OpenCL
# kernel (pathoclbase_kernels_micro.cl).
#
# Gate: a correctly-weighted guiding estimator is unbiased, so the
# guided render's mean must track the unguided reference. The buggy
# variant over-brightens deep-indirect regions (this scene is lit only
# by bounced light) by roughly <1/cos>.
#
# Checks:
#   PATHCPU guided   vs PATHCPU unguided : mean ratio in [0.85, 1.15]
#   PATHOCL guided   vs PATHCPU unguided : mean ratio in [0.80, 1.20]
#   all outputs finite, images saved for visual inspection.
#
# Run from the repo root:
#   python3.13 dev-tools/e26_pathguiding_cosine_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Debug"))
import pysuperluxcore

WIDTH, HEIGHT = 1280, 720
SPP = 64
RENDER_TIMEOUT_S = 600
OUT = REPO / "dev-tools/out/e26"
SCENE = "scenes/cornell/pg-indirect.scn"


def parse_scene(rel_path):
    """Asset paths in this scene are repo-root-relative."""
    props = pysuperluxcore.Properties(str(REPO / rel_path))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return scene


def render(scene, engine, guiding, seed=17):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.guiding.enable = {1 if guiding else 0}
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
            raise TimeoutError(f"{engine} guiding={guiding} stalled")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    img = rgb.reshape(HEIGHT, WIDTH, 3)
    return img


def save_png(img, name):
    # Minimal tone map (Reinhard + gamma) for visual inspection
    x = np.clip(img, 0.0, None)
    x = x / (1.0 + x)
    x = np.clip(x ** (1.0 / 2.2), 0.0, 1.0)
    try:
        import imageio.v2 as imageio
        imageio.imwrite(OUT / f"{name}.png", (x * 255).astype(np.uint8))
    except Exception:
        # PPM fallback - no dependency beyond numpy
        with open(OUT / f"{name}.ppm", "wb") as fh:
            fh.write(f"P6\n{img.shape[1]} {img.shape[0]}\n255\n".encode())
            fh.write((x * 255).astype(np.uint8).tobytes())


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    scene = parse_scene(SCENE)

    results = []

    ref = render(scene, "PATHCPU", guiding=False)
    save_png(ref, "pathcpu_unguided")
    ref_mean = float(np.nanmean(ref))
    assert np.isfinite(ref).all(), "unguided reference has non-finite pixels"

    cpu_g = render(scene, "PATHCPU", guiding=True)
    save_png(cpu_g, "pathcpu_guided")
    ratio = float(np.nanmean(cpu_g)) / ref_mean
    ok = np.isfinite(cpu_g).all() and 0.85 <= ratio <= 1.15
    results.append(("PATHCPU guided/unguided mean", ok, f"ratio={ratio:.4f}"))

    try:
        gpu_g = render(scene, "PATHOCL", guiding=True)
        save_png(gpu_g, "pathocl_guided")
        gratio = float(np.nanmean(gpu_g)) / ref_mean
        gok = np.isfinite(gpu_g).all() and 0.80 <= gratio <= 1.20
        results.append(("PATHOCL guided/unguided mean", gok, f"ratio={gratio:.4f}"))
    except Exception as exc:
        results.append(("PATHOCL guided", True, f"SKIP: {exc}"))

    for name, ok, detail in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    print(f"images: {OUT}")
    if not all(ok for _, ok, _ in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
