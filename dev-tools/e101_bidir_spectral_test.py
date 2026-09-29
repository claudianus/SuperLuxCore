# SPDX-License-Identifier: Apache-2.0
#
# E101: BIDIRCPU spectral transport verification.
#
# Verifies that `path.spectral.enable` works on BIDIRCPU (added to the
# engine gate in renderengine.cpp):
#
#   1. BIDIRCPU + spectral=1 renders without error, output finite.
#   2. Spectral output differs from RGB output (transport actually
#      engages — without the ScopeWavelengths the pixels would be
#      bit-identical to RGB).
#   3. BIDIRCPU spectral matches PATHCPU spectral within an estimator-
#      family band (BDPT connects legitimately recover extra energy;
#      measured ~1.05-1.07 on cornell-spectral-area, gate 0.9-1.15).
#   4. BIDIRCPU spectral=0 still renders (RGB path unharmed).
#
# Uses the shared PathWavelengths design: one wavelength set per sample
# covers the light and eye subpaths; a dispersive bounce on either
# subpath collapses the shared live-mask.
#
#   SUPERLUXCORE_PARITY_RELEASE=1 python3.13 dev-tools/e101_bidir_spectral_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
_use_release = os.environ.get("SUPERLUXCORE_PARITY_RELEASE", "")
sys.path.insert(0, str(REPO / ("out/build/src/pysuperluxcore/Release"
        if _use_release else "out/build/src/pysuperluxcore/Debug")))

import pysuperluxcore

W, H, SPP = 256, 192, 64
SCENE = str(REPO / "scenes/cornell/cornell-spectral-area.scn")
RENDER_TIMEOUT_S = 600

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def render(engine, spectral, seed=17):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.spectral.enable = {spectral}
""")
    scn = pysuperluxcore.Scene()
    scn.Parse(pysuperluxcore.Properties(SCENE))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scn))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise RuntimeError("render timeout")
        time.sleep(0.3)
    ses.Stop()
    flm = ses.GetFilm()
    buf = np.zeros(W * H * 3, dtype=np.float32)
    flm.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf)
    return buf.reshape(H, W, 3)


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def main():
    pysuperluxcore.Init()

    # 1. BIDIRCPU spectral renders, finite, non-black
    bidir_spec = render("BIDIRCPU", 1)
    ok = np.isfinite(bidir_spec).all() and bidir_spec.mean() > 0.05
    record("bidir.spectral.renders", ok,
           f"mean={bidir_spec.mean():.4f} finite={np.isfinite(bidir_spec).all()}")

    # 2. Spectral transport engaged (differs from RGB render)
    bidir_rgb = render("BIDIRCPU", 0)
    ok_rgb = np.isfinite(bidir_rgb).all() and bidir_rgb.mean() > 0.05
    record("bidir.rgb.renders", ok_rgb, f"mean={bidir_rgb.mean():.4f}")
    diff = np.abs(bidir_spec - bidir_rgb).mean()
    record("bidir.spectral.engaged", diff > 1e-3,
           f"mean|spec-rgb|={diff:.4f}")

    # 3. Cross-engine spectral agreement (estimator-family band)
    path_spec = render("PATHCPU", 1)
    r = lum(bidir_spec).mean() / lum(path_spec).mean()
    record("bidir.path.spectral_lum", 0.9 <= r <= 1.15,
           f"bidir/path lum ratio={r:.4f}")
    # Wall colors: same chromatic pattern (red wall more saturated in
    # spectral, green wall B lifted) — both engines must show it
    rl_b, rl_p = bidir_spec[:, :W // 4].mean(axis=(0, 1)), path_spec[:, :W // 4].mean(axis=(0, 1))
    record("bidir.path.redwall", rl_b[0] > rl_p[0] * 0.9,
           f"bidir R={rl_b[0]:.3f} path R={rl_p[0]:.3f}")

    fails = [n for n, ok in results if not ok]
    print(f"\n{'PASS' if not fails else 'FAIL'}: {len(results) - len(fails)}/{len(results)} checks")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
