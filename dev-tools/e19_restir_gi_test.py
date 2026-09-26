# SPDX-License-Identifier: Apache-2.0
#
# E19: ReSTIR GI (G1) - per-pixel first-bounce reservoir on PATHCPU.
#
# path.restir.gi.enable=1 resamples the depth-0 continuation vertex x2:
# K BSDF-sampled candidates are evaluated with the proxy target
# pi_hat = (f*cos)(x1->x2) * L_hat(x2), where L_hat is x2's direct
# light estimate (emission + one NEE shadow ray). The winning x2's
# continuation is traced normally and scaled by the RIS weight
# W = wSum/(M*pi_hat) - unbiased by GRIS construction. The pixel's
# reservoir is temporally merged via a Jacobian-corrected reconnection
# shift with a binary visibility test on the x1_cur->x2 segment, and
# G1-b merges up to 2 same-surface-gated neighbour pixels with the same
# shift; the stored reservoir keeps its pre-spatial state so inflated
# merge weights cannot feed back.
#
# Cornell box: the wall/floor pixels have a large indirect component
# (color bleeding), so a correct implementation must (a) stay unbiased
# against a converged reference and (b) not blow up - reuse quality is
# asserted only as bounded, not as guaranteed improvement (the proxy
# target is intentionally cheap in G1).
#
# T1: unbiasedness - GI mean vs converged baseline within 3%.
# T2: bounded error - RMSE within 3x of the plain PATHCPU baseline.
# T3: temporal+spatial reuse combo - same gates with all merges on.
# T4: crash safety - all outputs finite.
#
# Run:
#   python3.13 dev-tools/e19_restir_gi_test.py

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


def render(defs, spp, seed=17, engine="PATHCPU"):
    scn = pyluxcore.Properties(str(REPO / "scenes" / "cornell" /
                                   "cornell.scn"))
    sc = pyluxcore.Scene()
    sc.Parse(scn)

    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
""")
    if engine == "PATHOCL":
        cfg.SetFromString("opencl.task.count = 16384\n")
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


GI = {"path.restir.gi.enable": True}
GI_MERGE_OFF = {**GI, "path.restir.gi.temporal.enable": False,
              "path.restir.gi.spatial.enable": False}


def main():
    print("ReSTIR GI (G1) first-bounce reservoir on PATHCPU "
          "(cornell 160x120)\n", flush=True)

    ref = render({}, SPP_REF, seed=99)
    off = render({}, SPP)
    gi = render(GI_MERGE_OFF, SPP)
    combo = render(GI, SPP)
    print(f"  ref={ref.mean():.6f} off={off.mean():.6f} "
          f"gi={gi.mean():.6f} gi+merges={combo.mean():.6f}",
          flush=True)

    for name, img in (("T1.gi", gi), ("T3.gi+merges", combo)):
        record(f"{name}.unbiased-vs-ref",
               np.isfinite(img).all() and
               abs(float(img.mean()) - float(ref.mean())) /
               max(float(ref.mean()), 1e-12) < 0.03,
               f"mean={img.mean():.6f} vs ref={ref.mean():.6f} "
               f"(gate 3%)")

    # Bounded-error gate (NOT an improvement guarantee): the G1 proxy
    # target is x2's direct light only, so reuse quality is asserted
    # bounded rather than better - the gate catches explosions.
    r_off, r_gi = rmse(off, ref), rmse(gi, ref)
    record("T2.gi.rmse-bounded",
           r_gi <= r_off * 3.0,
           f"RMSE gi={r_gi:.6f} vs off={r_off:.6f} (gate 3.0x)")
    r_combo = rmse(combo, ref)
    record("T3.gi+merges.rmse-bounded",
           r_combo <= r_off * 3.0,
           f"RMSE combo={r_combo:.6f} vs off={r_off:.6f} (gate 3.0x)")

    record("T4.finite",
           np.isfinite(gi).all() and np.isfinite(combo).all(),
           "all pixels finite")

    # ---- G2: PATHOCL parity ------------------------------------------
    # The GPU path merges per-pixel reservoirs that sibling tasks write
    # concurrently, so its correctness additionally depends on the
    # seqlock publish + vSeq visibility pairing (a torn or superseded
    # read once inflated wSum into ~1e7x hot pixels on Metal). The
    # pixel-ratio tail gate is the regression tripwire for that class
    # of failure: beyond noise, merges may not create bright outliers.
    print("\n-- PATHOCL (GPU) --", flush=True)
    off_g = render({}, SPP, engine="PATHOCL")
    gi_g = render(GI_MERGE_OFF, SPP, engine="PATHOCL")
    combo_g = render(GI, SPP, engine="PATHOCL")
    print(f"  off={off_g.mean():.6f} gi={gi_g.mean():.6f} "
          f"gi+merges={combo_g.mean():.6f}", flush=True)

    for name, img in (("T5.gpu.gi", gi_g), ("T6.gpu.gi+merges", combo_g)):
        record(f"{name}.unbiased-vs-ref",
               np.isfinite(img).all() and
               abs(float(img.mean()) - float(ref.mean())) /
               max(float(ref.mean()), 1e-12) < 0.03,
               f"mean={img.mean():.6f} vs ref={ref.mean():.6f} "
               f"(gate 3%)")

    r_gi_g = rmse(gi_g, ref)
    record("T5.gpu.gi.rmse-bounded", r_gi_g <= r_off * 3.0,
           f"RMSE gi={r_gi_g:.6f} vs off={r_off:.6f} (gate 3.0x)")
    r_combo_g = rmse(combo_g, ref)
    record("T6.gpu.gi+merges.rmse-bounded",
           r_combo_g <= r_off * 3.0,
           f"RMSE combo={r_combo_g:.6f} vs off={r_off:.6f} (gate 3.0x)")

    # Merge-explosion tripwire: per-pixel ratios against the GPU
    # baseline. The pre-seqlock failure mode inflated the BULK of the
    # image (p99 ~ 44-400, max ~1e7-1e15); a healthy merge stream still
    # produces a handful of bounded-tail pixels (~10-300x) at low spp -
    # the CPU path shows the same tail - so the gate is on the bulk
    # quantile and the hot-pixel fraction, not on the max alone.
    ratios = combo_g / np.maximum(off_g, 1e-6)
    p99, mx = np.percentile(ratios, 99), float(ratios.max())
    hotFrac = float(np.mean(ratios > 8.0))
    record("T6.gpu.merges.no-explosion",
           np.isfinite(combo_g).all() and p99 < 3.0 and hotFrac < 0.005,
           f"pixel-ratio p99={p99:.3f} max={mx:.3f} "
           f"hot(>8x)={hotFrac * 100:.3f}% (gates p99<3, hot<0.5%)")

    print()
    ok = sum(1 for _, o in results if o)
    print(f"===== ReSTIR GI (G1+G2) CPU+GPU: {ok}/{len(results)} PASS =====")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
