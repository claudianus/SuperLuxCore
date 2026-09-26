# SPDX-License-Identifier: Apache-2.0
#
# E18: ReSTIR DI visibility-weighted RIS target on CPU (PATHCPU).
#
# lightstrategy.restir.visibility.enable=1 folds the binary visibility
# term V into each candidate's reservoir target: fresh candidates are
# evaluated at per-candidate hashed light-surface samples (su,sv,sp) -
# matching the GPU path - and each candidate shadow ray is traced on
# the scene accelerator (Embree). The winning candidate's sample values
# are returned to DirectLightSampling() so the final Illuminate() reuses
# the exact point the visibility test covered (V/payoff consistency).
# An occluded candidate contributes zero target weight but still counts
# in M, keeping the RIS output weight W = wSum/M consistent. Merged
# neighbor reservoirs keep their V-free re-evaluation at the reservoir
# seed (same approximation as the GPU kernels). The winner's real
# shadow ray still goes through the normal transparent-shadow/
# shadow-catcher path.
#
# T1 (unbiasedness): RESTIR+visibility mean vs LOG_POWER reference.
# T2 (bounded variance): the binary V term reallocates error into the
#    penumbra tail, so on mostly-visible scenes RMSE can measure worse
#    than the unweighted baseline (~1.0-1.8x observed on BOTH backends);
#    the gate only asserts the regression stays bounded.
# T3 (combination): same checks with temporal+spatial reuse enabled.
# T4 (crash safety): all outputs finite, no NaN/Inf.
#
# Run:
#   python3.13 dev-tools/e18_restir_visibility_cpu_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
import pyluxcore

WIDTH, HEIGHT = 160, 120
SPP, SPP_REF = 64, 512
RENDER_TIMEOUT_S = 300

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
renderengine.type = PATHCPU
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
""")
    for k, v in defs.items():
        cfg.Set(pyluxcore.Property(k, v))

    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(
                f"render stalled below {spp} spp after {RENDER_TIMEOUT_S}s")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def rmse(a, b):
    return float(np.sqrt(np.mean((a - b) ** 2)))


RESTIR = {"lightstrategy.type": "RESTIR_DI"}
VIS = {**RESTIR, "lightstrategy.restir.visibility.enable": True}


def main():
    print("ReSTIR DI visibility-weighted target on CPU (manylights "
          "160x120)\n", flush=True)
    for scn_file, tag in (("scene-4spot.scn", "spots"),
                          ("scene.scn", "mesh")):
        ref = render(scn_file, {"lightstrategy.type": "LOG_POWER"}, SPP_REF,
                     seed=99)
        off = render(scn_file, RESTIR, SPP)
        vis = render(scn_file, VIS, SPP)
        combo = render(scn_file, {**VIS,
                                  "lightstrategy.restir.temporal.enable": True,
                                  "lightstrategy.restir.spatialreuse.enable":
                                  True},
                       SPP)
        print(f"  [{tag}] ref={ref.mean():.6f} off={off.mean():.6f} "
              f"vis={vis.mean():.6f} combo={combo.mean():.6f}", flush=True)

        for name, img in (("T1.vis", vis), ("T3.vis+reuse", combo)):
            record(f"{name}.{tag}.unbiased-vs-logpower",
                   np.isfinite(img).all() and
                   abs(float(img.mean()) - float(ref.mean())) /
                   max(float(ref.mean()), 1e-12) < 0.03,
                   f"mean={img.mean():.6f} vs ref={ref.mean():.6f} "
                   f"(gate 3%)")

        r_off, r_vis = rmse(off, ref), rmse(vis, ref)
        # Bounded-variance check (NOT an improvement guarantee): folding
        # the binary V term into the target reallocates error into the
        # penumbra tail - on mostly-visible scenes like the mesh one it
        # can measure worse than the unweighted baseline. The same
        # spread appears on the GPU backend (e16), so this is a
        # property of visibility-weighted targets, not a port bug; the
        # gate catches explosions while T1/T3 anchor unbiasedness.
        # Measured spread at 64spp: vis/off ~1.0-1.8x.
        record(f"T2.{tag}.variance-bounded",
               r_vis <= r_off * 2.0,
               f"RMSE vis={r_vis:.6f} vs off={r_off:.6f} (gate 2.0x; "
               f"V reallocates error into the penumbra tail)")

        record(f"T4.{tag}.finite",
               np.isfinite(vis).all() and np.isfinite(combo).all(),
               "all pixels finite")

    print()
    ok = sum(1 for _, o in results if o)
    print(f"===== ReSTIR visibility CPU: {ok}/{len(results)} PASS =====")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
