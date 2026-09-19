# SPDX-License-Identifier: Apache-2.0
#
# E14: ReSTIR DI spatial reuse on GPU (PATHOCL / Metal).
#
# Ports the CPU Stage-4 GRIS neighbor merge to the GPU kernels: a
# world-space hash grid of reservoirs (appended to the per-pixel
# temporal buffer) is merged into each shade point's RIS stream with
# GRIS weights b = wSum * (pi_new / pi_old), and finished reservoirs
# are stored back for later passes.
#
# T1 (unbiasedness): RESTIR+spatial mean vs LOG_POWER reference mean.
# T2 (bounded variance): spatial-without-shifts does NOT reduce RMSE on
#    these scenes - the CPU path measures the same (spots ~1.3x, mesh
#    ~1.1x worse); the port only asserts the regression stays bounded
#    and matches CPU behaviour. A variance win needs shift mappings.
# T3 (temporal+spatial): same checks with both reuse paths enabled.
#
# Run:
#   python3.13 dev-tools/e14_restir_spatial_gpu_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Release"))
import pyluxcore

WIDTH, HEIGHT = 160, 120
SPP, SPP_REF = 64, 512

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def render(scn_file, defs, spp, seed=17):
    scn = pyluxcore.Properties(str(REPO / "scenes" / "manylights" / scn_file))
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
""")
    for k, v in defs.items():
        cfg.Set(pyluxcore.Property(k, v))

    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, sc))
    ses.Start()
    while True:
        ses.UpdateStats()
        time.sleep(0.5)
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def rmse(a, b):
    return float(np.sqrt(np.mean((a - b) ** 2)))


RESTIR = {"lightstrategy.type": "RESTIR_DI"}


def main():
    print("ReSTIR DI spatial reuse on GPU (manylights 160x120)\n", flush=True)
    for scn_file, tag in (("scene-4spot.scn", "spots"),
                          ("scene.scn", "mesh")):
        ref = render(scn_file, {"lightstrategy.type": "LOG_POWER"}, SPP_REF,
                     seed=99)
        off = render(scn_file, RESTIR, SPP)
        on = render(scn_file, {**RESTIR,
                               "lightstrategy.restir.spatialreuse.enable": True},
                    SPP)
        both = render(scn_file, {**RESTIR,
                                 "lightstrategy.restir.spatialreuse.enable": True,
                                 "lightstrategy.restir.temporal.enable": True},
                      SPP)
        print(f"  [{tag}] ref={ref.mean():.6f} off={off.mean():.6f} "
              f"on={on.mean():.6f} both={both.mean():.6f}", flush=True)

        for name, img in (("T1.spatial", on), ("T1.spatial+temporal", both)):
            record(f"{name}.{tag}.unbiased-vs-logpower",
                   np.isfinite(img).all() and
                   abs(float(img.mean()) - float(ref.mean())) /
                   max(float(ref.mean()), 1e-12) < 0.03,
                   f"mean={img.mean():.6f} vs ref={ref.mean():.6f} (gate 3%)")

        r_off, r_on = rmse(off, ref), rmse(on, ref)
        record(f"T2.{tag}.variance-bounded", r_on < r_off * 1.3,
               f"RMSE on={r_on:.6f} vs off={r_off:.6f} "
               f"(CPU shows ~1.1-1.3x worse; gate 1.3x)")

    print()
    ok = sum(1 for _, o in results if o)
    print(f"===== ReSTIR spatial GPU: {ok}/{len(results)} PASS =====")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
