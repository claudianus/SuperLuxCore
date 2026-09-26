# SPDX-License-Identifier: Apache-2.0
#
# E30: ReSTIR DI effectiveness audit on manylights @ 720p (PATHOCL).
#
# Answers the audit question "does ReSTIR actually help?": renders the
# same scene with LOG_POWER vs RESTIR_DI (plain / +temporal+spatial /
# +visibility) at equal spp and compares RMSE against a high-spp
# LOG_POWER reference, plus wall-clock render time.
#
# Writes PNGs to dev-tools/out/e30/ for visual inspection.
#
# Run:
#   python3.13 dev-tools/e30_restir_effectiveness_720p.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
import pyluxcore

WIDTH, HEIGHT = 1280, 720
SPP, SPP_REF = 32, 256
TASK_COUNT = 65536
RENDER_TIMEOUT_S = 900
OUT = REPO / "dev-tools" / "out" / "e30"
OUT.mkdir(parents=True, exist_ok=True)


def render(defs, spp, seed=17):
    scn = pyluxcore.Properties(str(REPO / "scenes" / "manylights" / "scene.scn"))
    sc = pyluxcore.Scene()
    sc.Parse(scn)

    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHOCL
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
""")
    for k, v in defs.items():
        cfg.Set(pyluxcore.Property(k, v))

    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, sc))
    t0 = time.monotonic()
    ses.Start()
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() - t0 > RENDER_TIMEOUT_S:
            ses.Stop()
            raise TimeoutError(f"stalled below {spp} spp")
        time.sleep(0.5)
    elapsed = time.monotonic() - t0
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3), elapsed


def save_png(name, img):
    from PIL import Image
    img8 = np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(img8).save(OUT / name)


def rmse(a, b):
    return float(np.sqrt(np.mean((a - b) ** 2)))


def main():
    print("ReSTIR DI effectiveness @ 1280x720 PATHOCL manylights\n", flush=True)

    ref, t_ref = render({}, SPP_REF, seed=99)
    save_png("ref_logpower_256spp.png", ref)
    print(f"ref LOG_POWER {SPP_REF}spp: mean={ref.mean():.6f} "
          f"({t_ref:.0f}s)", flush=True)

    variants = [
        ("logpower", {}, "uniform+power baseline"),
        ("restir", {"lightstrategy.type": "RESTIR_DI",
                    "lightstrategy.restir.temporal.enable": False,
                    "lightstrategy.restir.spatialreuse.enable": False},
         "RIS only"),
        ("restir+reuse", {"lightstrategy.type": "RESTIR_DI",
                          "lightstrategy.restir.temporal.enable": True,
                          "lightstrategy.restir.spatialreuse.enable": True},
         "RIS + temporal + spatial"),
        ("restir+vis+reuse", {"lightstrategy.type": "RESTIR_DI",
                              "lightstrategy.restir.visibility.enable": True,
                              "lightstrategy.restir.temporal.enable": True,
                              "lightstrategy.restir.spatialreuse.enable": True},
         "RIS + visibility + temporal + spatial"),
    ]

    for name, defs, desc in variants:
        img, t = render(defs, SPP)
        save_png(f"{name}_{SPP}spp.png", img)
        r = rmse(img, ref)
        dmean = (img.mean() - ref.mean()) / ref.mean() * 100
        print(f"{name:>18} ({desc}): mean={img.mean():.6f} "
              f"({dmean:+.2f}% vs ref) RMSE={r:.6f} ({t:.0f}s)",
              flush=True)

    print(f"\nPNGs written to {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
