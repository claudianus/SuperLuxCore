# SPDX-License-Identifier: Apache-2.0
#
# E95 visual: ReSTIR PT (PT-1) on/off at 1280x720 PATHCPU cornell.
# Saves unique PNGs under dev-tools/out/e95_pt/ and reports the
# mean/RMSE delta - the visual check is the primary gate.
#
# Run: python3.13 dev-tools/e95_restir_pt_visual.py

import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 1280, 720
SPP, SPP_REF = 48, 256
RENDER_TIMEOUT_S = 900
OUT = REPO / "dev-tools" / "out" / "e95_pt"
OUT.mkdir(parents=True, exist_ok=True)


def render(defs, spp, seed=17):
    scn = pysuperluxcore.Properties(str(REPO / "scenes" / "cornell" /
                                   "cornell.scn"))
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


def main():
    off, t_off = render({}, SPP)
    save_png(f"cornell_PATHCPU_off_{SPP}spp.png", off)
    print(f"off {SPP}spp: mean={off.mean():.5f} ({t_off:.0f}s)", flush=True)

    pt, t_pt = render({"path.restir.pt.enable": True}, SPP)
    save_png(f"cornell_PATHCPU_pt_{SPP}spp.png", pt)
    print(f"pt  {SPP}spp: mean={pt.mean():.5f} ({t_pt:.0f}s)", flush=True)

    ref, _ = render({}, SPP_REF, seed=99)
    save_png(f"cornell_PATHCPU_ref_{SPP_REF}spp.png", ref)

    rmse = lambda a, b: float(np.sqrt(np.mean((a - b) ** 2)))
    print(f"\nRMSE off={rmse(off, ref):.5f}  pt={rmse(pt, ref):.5f}")
    print(f"mean parity: off={(off.mean() / ref.mean() - 1) * 100:+.2f}% "
          f"pt={(pt.mean() / ref.mean() - 1) * 100:+.2f}%")
    print(f"finite: off={np.isfinite(off).all()} pt={np.isfinite(pt).all()}")


if __name__ == "__main__":
    main()
