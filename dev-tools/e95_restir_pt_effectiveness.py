# SPDX-License-Identifier: Apache-2.0
#
# E95 effectiveness: does ReSTIR PT actually win where it should?
# Same-spp RMSE-vs-reference comparison on indirect-heavy scenes
# (pg-indirect: diffuse-indirect dominant; pg-glossy-indirect: glossy
# multi-bounce). Cornell is diffuse-flat - PT is not expected to win
# there; these scenes are the target regime.
#
# Run: python3.13 dev-tools/e95_restir_pt_effectiveness.py [scene]

import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 1280, 720
SPP, SPP_REF = 32, 256
RENDER_TIMEOUT_S = 1200
OUT = REPO / "dev-tools" / "out" / "e95_pt"
OUT.mkdir(parents=True, exist_ok=True)

SCENES = {
    "pg-indirect": "cornell/pg-indirect.scn",
    "pg-glossy": "cornell/pg-glossy-indirect.scn",
    "area-caustic": "cornell/cornell-area-caustic.scn",
}


def render(scene_rel, defs, spp, seed=17):
    scn = pysuperluxcore.Properties(str(REPO / "scenes" / scene_rel))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHCPU
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
""")
    for k, v in defs.items():
        cfg.Set(pysuperluxcore.Property(k, v))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    t0 = time.monotonic()
    ses.Start()
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() - t0 > RENDER_TIMEOUT_S:
            ses.Stop()
            raise TimeoutError(f"stalled below {spp} spp")
        time.sleep(1.0)
    elapsed = time.monotonic() - t0
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3), elapsed


def save_png(name, img):
    img8 = np.clip(np.flipud(img) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(img8).save(OUT / name)
    print(f"  saved {OUT / name}", flush=True)


def rmse(a, b):
    return float(np.sqrt(np.mean((a - b) ** 2)))


def main():
    key = sys.argv[1] if len(sys.argv) > 1 else "pg-indirect"
    scene_rel = SCENES[key]
    print(f"ReSTIR PT effectiveness: {key} {WIDTH}x{HEIGHT} PATHCPU\n",
          flush=True)

    ref, _ = render(scene_rel, {}, SPP_REF, seed=99)
    save_png(f"{key}_ref_{SPP_REF}spp.png", ref)
    print(f"ref {SPP_REF}spp mean={ref.mean():.5f}", flush=True)

    off, t_off = render(scene_rel, {}, SPP)
    save_png(f"{key}_off_{SPP}spp.png", off)
    pt, t_pt = render(scene_rel, {"path.restir.pt.enable": True}, SPP)
    save_png(f"{key}_pt_{SPP}spp.png", pt)

    r_off, r_pt = rmse(off, ref), rmse(pt, ref)
    print(f"\n{key} @{SPP}spp  off={t_off:.0f}s pt={t_pt:.0f}s")
    print(f"  RMSE off={r_off:.5f}  pt={r_pt:.5f}  "
          f"ratio={r_pt / max(r_off, 1e-9):.3f} (<1 = PT wins)")
    print(f"  mean parity off={(off.mean() / ref.mean() - 1) * 100:+.2f}% "
          f"pt={(pt.mean() / ref.mean() - 1) * 100:+.2f}%")
    print(f"  finite off={np.isfinite(off).all()} "
          f"pt={np.isfinite(pt).all()}")


if __name__ == "__main__":
    main()
