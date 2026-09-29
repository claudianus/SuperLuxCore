# SPDX-License-Identifier: Apache-2.0
#
# E96: ReSTIR PT (PT-2) - GPU suffix-reservoir parity on PATHOCL.
#
# path.restir.pt.enable=1 on PATHOCL runs the MK_PT_BOUNCE /
# MK_PT_RESOLVE tail-queue states: K candidate bounce rays + K NEE
# probes + 3 merge-visibility rays per task, measured-suffix payload
# (L_suf), and the armed-pick ledger (pending 3 -> landed 4 ->
# committed at MK_SPLAT_SAMPLE). A stored-suffix win pays
# thr*fcos*W*L_suf directly and ends the path.
#
# Regime note: same bounded-bias regime as PT-1 (see restirpt.h);
# parity gates at 5% mean error vs a converged PATHOCL reference.
#
# T1: parity - PT mean vs converged baseline within 5%.
# T2: bounded error - RMSE within 3x of plain PATHOCL.
# T3: temporal+spatial combo - same gates with all merges on.
# T4: crash safety - all outputs finite.
# T5: merge explosion tripwire - bulk pixel-ratio bounded.
# T6: sanity - PT render retains scene structure (not black/degenerate).
#
# Run:
#   python3.13 dev-tools/e96_restir_pt_gpu_test.py
#
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


def device_mask(want_type):
    pysuperluxcore.Init()
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    mask, i = "", 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        mask += "1" if t == want_type else "0"
        i += 1
    return mask or None


def render(defs, spp, seed=17):
    scn = pysuperluxcore.Properties(str(REPO / "scenes" / "cornell" /
                                   "cornell.scn"))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHOCL
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
""")
    sel = device_mask("METAL_GPU")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
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
    print("ReSTIR PT (PT-2) path-suffix reservoir on PATHOCL "
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

    ratios = combo / np.maximum(off, 1e-6)
    p99 = np.percentile(ratios, 99)
    hotFrac = float(np.mean(ratios > 8.0))
    record("T5.merges.no-explosion",
           p99 < 3.0 and hotFrac < 0.005,
           f"pixel-ratio p99={p99:.3f} max={float(ratios.max()):.3f} "
           f"hot(>8x)={hotFrac * 100:.3f}% (gates p99<3, hot<0.5%)")

    # Degenerate-output guard: the PT image must retain the scene
    # (nonzero variance, mean inside a sane band of the off render).
    record("T6.sanity",
           float(pt.std()) > 0.01 and
           0.5 < float(pt.mean()) / max(float(off.mean()), 1e-12) < 2.0,
           f"pt std={pt.std():.4f} mean-ratio={pt.mean() / off.mean():.3f}")

    print()
    ok = sum(1 for _, o in results if o)
    print(f"===== ReSTIR PT (PT-2) PATHOCL: {ok}/{len(results)} PASS =====")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
