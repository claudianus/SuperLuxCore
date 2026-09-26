# SPDX-License-Identifier: Apache-2.0
#
# E43: diffraction material functional tests.
#
#   T1  math sanity: spacing >> lambda collapses to order 0 -> the render must
#       equal a plain mirror render of the same geometry.
#   T2  rainbow: at 1.6um spacing + spectral mode the disc shows hue variation
#       (orders fan different wavelengths to different directions).
#   T3  CPU/GPU parity: PATHCPU vs PATHOCL on the same scene.
#   T4  RGB (non-spectral) mode still produces a finite, non-black image.
#
# Run from the repo root:
#   python3.13 dev-tools/e43_diffraction_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = 128
RENDER_TIMEOUT_S = 900


# Dedicated test scene (kept inline so showcase-scene edits cannot break
# the regression): flat annulus + finite emissive ball + faint uniform fill.
TEST_SCN = """
scene.camera.lookat = 0. -0.16 0.075 0. 0. 0.008
scene.camera.fieldofview = 38.0
scene.materials.dfr.type = diffraction
scene.materials.dfr.kr = 0.95 0.95 0.95
scene.materials.dfr.spacing = 1600.
scene.materials.dfr.orientation = radial
scene.materials.dfr.center = 0. 0. 0.
scene.materials.dfr.roughness = 0.0
scene.materials.dfr.fillfactor = 0.5
scene.materials.dfr.blaze = 0.
scene.materials.dfr.orders = 8
scene.materials.ballMat.type = matte
scene.materials.ballMat.emission = 60. 60. 60.
scene.materials.ballMat.kd = 0. 0. 0.
scene.objects.disc.material = dfr
scene.objects.disc.ply = scenes/diffraction/cd-annulus.ply
scene.objects.ball.material = ballMat
scene.objects.ball.ply = scenes/diffraction/light-ball.ply
scene.objects.ball.transformation = 1. 0. 0. 0. 0. 1. 0. 0.19 0. 0. 1. 0.09 0. 0. 0. 1.
scene.lights.fill.type = constantinfinite
scene.lights.fill.color = 1. 1. 1.
scene.lights.fill.gain = 0.02 0.02 0.02
"""


def load_props():
    cwd = os.getcwd()
    os.chdir(str(REPO))  # relative .ply paths resolve against the repo root
    try:
        props = pysuperluxcore.Properties()
        props.SetFromString(TEST_SCN)
        return props
    finally:
        os.chdir(cwd)


def render(props, engine, spectral, seed=17):
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.pathdepth.total = 8
path.spectral.enable = {1 if spectral else 0}
film.imagepipeline.0.type = NOP
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
            raise TimeoutError(f"render stalled ({engine})")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def disc_mask():
    # approximate disc region: center-ish circle in screen space
    yy, xx = np.mgrid[0:HEIGHT, 0:WIDTH]
    cx, cy, r = WIDTH * 0.5, HEIGHT * 0.52, HEIGHT * 0.42
    return ((xx - cx) ** 2 + (yy - cy) ** 2) < r * r


def main():
    props = load_props()
    ok = True

    # T1: huge spacing + no roughness -> every order collapses to the
    # specular direction -> render must equal a plain mirror (up to ~1e-9
    # rad direction error, only visible on the lamp's hard silhouette).
    p1 = pysuperluxcore.Properties(props)
    p1.Set(pysuperluxcore.Property("scene.materials.dfr.spacing", "1e12"))
    p1.Set(pysuperluxcore.Property("scene.materials.dfr.roughness", "0."))
    img_dfr = render(p1, "PATHCPU", spectral=True)
    p1m = pysuperluxcore.Properties(p1)
    p1m.Set(pysuperluxcore.Property("scene.materials.dfr.type", "mirror"))
    img_mir = render(p1m, "PATHCPU", spectral=True)
    mask = disc_mask()
    # Per-pixel diffs are expected only along the lamp silhouette (~1e-9 rad
    # direction epsilon flips hit/miss). Compare block averages instead:
    # a systematic BSDF error would show up at any block size.
    B = 8
    h, w = img_dfr.shape[:2]
    a_blk = img_dfr[:h // B * B, :w // B * B].reshape(h // B, B, w // B, B, 3).mean((1, 3))
    b_blk = img_mir[:h // B * B, :w // B * B].reshape(h // B, B, w // B, B, 3).mean((1, 3))
    d_blk = np.abs(a_blk - b_blk)
    denom = np.maximum(b_blk, 0.05)
    rel_blk = (d_blk / denom).max(axis=2)
    frac_bad = float((rel_blk > 0.10).mean())
    ratio = float(img_dfr[mask].mean() / max(img_mir[mask].mean(), 1e-6))
    print(f"T1 mirror-equivalence: block rel diff>10% {frac_bad:.2%}, "
          f"energy ratio {ratio:.4f}")
    if frac_bad > 0.01 or abs(ratio - 1.0) > 0.05:
        ok = False
        print("   FAIL: diffraction with no valid orders != mirror")

    # T2: rainbow presence under spectral mode at CD spacing
    img = render(props, "PATHCPU", spectral=True)
    mask = disc_mask()
    px = img[mask]
    lum = px.mean(axis=1)
    lit = px[lum > 0.1 * lum.max()]
    hue_spread = float((lit.max(axis=1) - lit.min(axis=1)).mean()) if lit.size else 0.0
    print(f"T2 spectral disc mean={px.mean():.4f} lit px={lit.size} hue spread={hue_spread:.4f}")
    if lit.size < 200 or hue_spread < 0.05:
        ok = False
        print("   FAIL: no visible hue separation on the disc")

    # T3: CPU vs GPU parity (spectral)
    gpu = render(props, "PATHOCL", spectral=True)
    err = img[mask] - gpu[mask]
    rmse = float(np.sqrt((err ** 2).mean()))
    ref = float(img[mask].mean())
    print(f"T3 CPU/GPU parity: rmse={rmse:.5f} ref mean={ref:.5f} ratio={rmse/max(ref,1e-6):.3f}")
    if rmse / max(ref, 1e-6) > 0.35:
        ok = False
        print("   FAIL: PATHCPU/PATHOCL diverge beyond MC noise")

    # T4: non-spectral RGB fallback is finite and non-black
    rgb = render(props, "PATHCPU", spectral=False)
    finite = np.isfinite(rgb).all()
    mean = float(rgb[mask].mean())
    print(f"T4 RGB mode: finite={finite} disc mean={mean:.4f}")
    if not finite or mean < 1e-4:
        ok = False
        print("   FAIL: RGB fallback black/NaN")

    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
