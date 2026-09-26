# SPDX-License-Identifier: Apache-2.0
#
# E27: path-guiding variance A/B measurement (M4 SD-tree + vMF mixture).
#
# e26 proves the guided estimator is unbiased; this test measures whether
# it actually REDUCES variance on a deep-indirect scene (pg-indirect.scn
# is lit purely by bounced light).
#
# Method: N independent renders per config (different seeds) at fixed
# spp; the per-pixel variance across runs estimates estimator variance /
# spp. Pooled over ~1M pixels the aggregate is stable. No converged
# reference needed.
#
# Checks:
#   - guided mean stays within [0.9, 1.1] x unguided mean (unbiased)
#   - variance ratio var_unguided / var_guided reported (target > 1)
#   - ensemble-mean images saved for visual inspection.
#
# IMPORTANT: statistics are computed on the raw linear RGB film channel.
# RGB_IMAGEPIPELINE applies a nonlinear transform, and a variance-reduced
# estimator reads systematically brighter through it (Jensen effect) —
# that produced a spurious "+10%" bias reading during M4b RIS bring-up
# even though the linear radiance was unbiased to <0.1%.
#
# Run from the repo root:
#   python3.13 dev-tools/e27_pathguiding_variance_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
# Prefer the Release build (the GNUmakefile default); fall back to Debug.
for _cfg in ("Release", "Debug"):
    _lib = REPO / "out/build/src/pyluxcore" / _cfg
    if list(_lib.glob("pyluxcore*.so")):
        sys.path.insert(0, str(_lib))
        break
import pyluxcore

WIDTH = int(os.environ.get("E27_W", "1280"))
HEIGHT = int(os.environ.get("E27_H", "720"))
SPP = int(os.environ.get("E27_SPP", "64"))
RUNS = int(os.environ.get("E27_RUNS", "4"))
ENGINE = os.environ.get("E27_ENGINE", "PATHCPU")
SCENE = os.environ.get("E27_SCENE", "scenes/cornell/pg-indirect.scn")
RENDER_TIMEOUT_S = 900
OUT = REPO / "dev-tools/out/e27"


def parse_scene(rel_path):
    props = pyluxcore.Properties(str(REPO / rel_path))
    scene = pyluxcore.Scene()
    scene.Parse(props)
    return scene


def render(scene, guiding, seed):
    cfg = pyluxcore.Properties()
    table = os.environ.get("E27_TABLE", "")
    table_line = f"path.guiding.tablefile = {table}" if (guiding and table) else ""
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {ENGINE}
sampler.type = {os.environ.get("E27_SAMPLER", "SOBOL")}
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.guiding.enable = {1 if guiding else 0}
{table_line}
{os.environ.get("E27_EXTRA_CFG", "")}
{os.environ.get("E27_GUIDED_CFG", "") if guiding else ""}
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
            raise TimeoutError(f"{ENGINE} guiding={guiding} seed={seed} stalled")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def save_png(img, name):
    x = np.clip(img, 0.0, None)
    x = x / (1.0 + x)
    x = np.clip(x ** (1.0 / 2.2), 0.0, 1.0)
    # PNG writer (zlib only, no imageio dependency)
    import zlib, struct
    data = (x * 255).astype(np.uint8)
    h, w = data.shape[:2]
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw))
           + chunk(b"IEND", b""))
    (OUT / f"{name}.png").write_bytes(png)


def ensemble_variance(images):
    """Per-pixel luminance variance across runs, then spatial mean."""
    lum = np.stack([img @ np.array([0.2126, 0.7152, 0.0722], np.float32)
                    for img in images])
    return float(np.mean(np.var(lum, axis=0, ddof=1)))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    scene = parse_scene(SCENE)

    unguided = [render(scene, False, seed=1000 + i) for i in range(RUNS)]
    guided = [render(scene, True, seed=1000 + i) for i in range(RUNS)]

    u_mean = np.mean(unguided, axis=0)
    g_mean = np.mean(guided, axis=0)
    save_png(u_mean, "unguided_mean")
    save_png(g_mean, "guided_mean")
    np.save(OUT / "unguided_mean.npy", u_mean)
    np.save(OUT / "guided_mean.npy", g_mean)
    lum_u = u_mean @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    lum_g = g_mean @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    ratio_map = np.clip((lum_g / np.maximum(lum_u, 1e-6) - 1.0) * 4.0 + 0.5,
                        0.0, 1.0)
    save_png(np.stack([ratio_map, np.zeros_like(ratio_map),
                       1.0 - ratio_map], axis=-1), "ratio_map")

    mean_ratio = float(np.nanmean(g_mean)) / float(np.nanmean(u_mean))
    var_u = ensemble_variance(unguided)
    var_g = ensemble_variance(guided)
    var_ratio = var_u / var_g if var_g > 0 else float("inf")

    ok_bias = np.isfinite(g_mean).all() and 0.9 <= mean_ratio <= 1.1
    print(f"[{'PASS' if ok_bias else 'FAIL'}] guided/unguided mean ratio: "
          f"{mean_ratio:.4f}")
    print(f"[INFO] per-pixel luminance variance: unguided={var_u:.6f} "
          f"guided={var_g:.6f} ratio={var_ratio:.3f}x "
          f"({'reduction' if var_ratio > 1 else 'NO reduction'})")
    # Per-block variance ratios: localizes where the guide helps/hurts.
    lum_u = np.stack([img @ np.array([0.2126, 0.7152, 0.0722], np.float32)
                      for img in unguided])
    lum_g = np.stack([img @ np.array([0.2126, 0.7152, 0.0722], np.float32)
                      for img in guided])
    vu = np.var(lum_u, axis=0, ddof=1)
    vg = np.var(lum_g, axis=0, ddof=1)
    BH, BW = 6, 8
    print("block var ratios (unguided/guided, >1 = guided better):")
    for by in range(BH):
        row = []
        for bx in range(BW):
            su = vu[by * HEIGHT // BH:(by + 1) * HEIGHT // BH,
                    bx * WIDTH // BW:(bx + 1) * WIDTH // BW].mean()
            sg = vg[by * HEIGHT // BH:(by + 1) * HEIGHT // BH,
                    bx * WIDTH // BW:(bx + 1) * WIDTH // BW].mean()
            row.append(f"{su / sg:.2f}" if sg > 0 else "inf")
        print("   " + " ".join(f"{r:>5}" for r in row))
    print(f"images: {OUT}")
    if not ok_bias:
        sys.exit(1)


if __name__ == "__main__":
    main()
