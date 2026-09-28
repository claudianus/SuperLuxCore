# SPDX-License-Identifier: Apache-2.0
#
# E95: ReSTIR PT (PT-1) - per-pixel path-suffix reservoir on PATHCPU.
#
# path.restir.pt.enable=1 resamples the depth-0 continuation like GI,
# but stored reservoir entries carry the MEASURED suffix radiance
# L_suf = delta_radiance / landing_throughput captured at path end.
# A stored-suffix winner contributes f*cos*W*connThr*L_suf directly and
# terminates the path (one shadow ray for the reconnection instead of a
# suffix retrace). Temporal + same-surface-gated spatial merges use the
# GI Jacobian/binary-V machinery; the merge target uses measured L_suf.
#
# Regime note (see restirpt.h): measured-suffix reuse is the
# bounded-bias "empirical reuse" regime - convergence parity is gated
# here at 5% mean error, not the GI 3%, and merge-explosion / RMSE
# tripwires bound the reuse failure modes.
#
# T1: parity - PT mean vs converged baseline within 5%.
# T2: bounded error - RMSE within 3x of plain PATHCPU.
# T3: temporal+spatial combo - same gates with all merges on.
# T4: crash safety - all outputs finite.
# T5: merge explosion tripwire - bulk pixel-ratio bounded.
#
# Run:
#   python3.13 dev-tools/e95_restir_pt_test.py

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 160, 120
SPP, SPP_REF = 64, 512
RENDER_TIMEOUT_S = 300

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


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
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def rmse(a, b):
    return float(np.sqrt(np.mean((a - b) ** 2)))


PT = {"path.restir.pt.enable": True}
PT_MERGE_OFF = {**PT, "path.restir.pt.temporal.enable": False,
                "path.restir.pt.spatial.enable": False}


def main():
    print("ReSTIR PT (PT-1) path-suffix reservoir on PATHCPU "
          "(cornell 160x120)\n", flush=True)

    ref = render({}, SPP_REF, seed=99)
    off = render({}, SPP)
    pt = render(PT_MERGE_OFF, SPP)
    combo = render(PT, SPP)
    print(f"  ref={ref.mean():.6f} off={off.mean():.6f} "
          f"pt={pt.mean():.6f} pt+merges={combo.mean():.6f}",
          flush=True)

    for name, img in (("T1.pt", pt), ("T3.pt+merges", combo)):
        record(f"{name}.parity-vs-ref",
               np.isfinite(img).all() and
               abs(float(img.mean()) - float(ref.mean())) /
               max(float(ref.mean()), 1e-12) < 0.05,
               f"mean={img.mean():.6f} vs ref={ref.mean():.6f} "
               f"(gate 5%, bounded-bias regime)")

    r_off = rmse(off, ref)
    r_pt = rmse(pt, ref)
    record("T2.pt.rmse-bounded", r_pt <= r_off * 3.0,
           f"RMSE pt={r_pt:.6f} vs off={r_off:.6f} (gate 3.0x)")
    r_combo = rmse(combo, ref)
    record("T3.pt+merges.rmse-bounded", r_combo <= r_off * 3.0,
           f"RMSE combo={r_combo:.6f} vs off={r_off:.6f} (gate 3.0x)")

    record("T4.finite",
           np.isfinite(ref).all() and np.isfinite(off).all() and
           np.isfinite(pt).all() and np.isfinite(combo).all(),
           "all outputs finite")

    # Merge-explosion tripwire (same gates as e19): stored-suffix wins
    # are the reuse path most likely to blow up.
    ratios = combo / np.maximum(off, 1e-6)
    p99 = np.percentile(ratios, 99)
    hotFrac = float(np.mean(ratios > 8.0))
    record("T5.merges.no-explosion",
           p99 < 3.0 and hotFrac < 0.005,
           f"pixel-ratio p99={p99:.3f} max={float(ratios.max()):.3f} "
           f"hot(>8x)={hotFrac * 100:.3f}% (gates p99<3, hot<0.5%)")

    print()
    ok = sum(1 for _, o in results if o)
    print(f"===== ReSTIR PT (PT-1) PATHCPU: {ok}/{len(results)} PASS =====")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
