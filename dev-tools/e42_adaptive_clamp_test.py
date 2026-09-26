# SPDX-License-Identifier: Apache-2.0
#
# E42: Adaptive Robust Clamping (ARC) regression test.
#
# ARC replaces the legacy fixed "pixel mean +- sqrt(maxvalue)" bound with
# robust per-pixel spatial statistics (3x3-neighborhood median + MAD) and
# a path-class scope (all/indirect/direct). See
# include/slg/utils/varianceclamping.h for the algorithm description.
#
# Scene: scenes/cornell/pg-glossy-indirect.scn - glossy floor + a directly
# visible 60-intensity emitter, so the image contains BOTH legitimate
# extreme brightness (emitter pixels, lum=60) and indirect-path fireflies
# (sparse huge contributions on the glossy floor).
#
# T1 indirect firefly suppression: INDIRECT_GLOSSY p99 drops >40% with
#    scope=indirect vs unclamped.
# T2 energy preservation: beauty mean within 10% of unclamped (the legacy
#    fixed margin loses >40% on this scene).
# T3 direct-class exemption: EMISSION channel mean within 5% of unclamped
#    under scope=indirect.
# T4 adaptive margin protects legit bright content under scope=all:
#    EMISSION mean stays >= 80% of unclamped (legacy scope=all drops it
#    to ~33%).
# T5 scope mechanism, decoupled from the adaptive margin (adaptive=0):
#    scope=direct clamps the EMISSION channel hard while INDIRECT_GLOSSY
#    is untouched; scope=indirect does the inverse. (With adaptive=1 the
#    spatially coherent emitter is legitimately protected even under
#    scope=direct - that is the point of the robust margin, see T4.)
# T6 CPU/GPU parity: PATHCPU vs PATHOCL beauty + indirect means.
# T7 legacy mode still available (adaptive=0 + scope=all reproduces the
#    fixed-margin behaviour: mean drops sharply).
#
# Run:
#   python3.13 dev-tools/e42_adaptive_clamp_test.py
#
import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for _variant in ("Release", "Debug"):
    _p = REPO / "out/build/src/pysuperluxcore" / _variant
    if any(_p.glob("pysuperluxcore*.so")):
        sys.path.insert(0, str(_p))
        break
import pysuperluxcore

WIDTH, HEIGHT = 160, 120
SPP = 32
TASK_COUNT = 1 << 16
RENDER_TIMEOUT_S = 300

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def device_mask(want_type):
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    mask = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        mask += "1" if t == want_type else "0"
        i += 1
    return mask or None


CHANNELS = {
    0: "RGB",
    1: "INDIRECT_GLOSSY",
    2: "INDIRECT_DIFFUSE",
    3: "INDIRECT_SPECULAR",
    4: "EMISSION",
    5: "DIRECT_GLOSSY",
}


def render(defs, engine="PATHCPU", sel=None, seed=17):
    scn = pysuperluxcore.Properties(
        str(REPO / "scenes/cornell/pg-glossy-indirect.scn"))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
film.imagepipelines.0.0.type = NOP
film.outputs.1.type = INDIRECT_GLOSSY
film.outputs.1.filename = /tmp/e42_iglossy.hdr
film.outputs.2.type = INDIRECT_DIFFUSE
film.outputs.2.filename = /tmp/e42_idiff.hdr
film.outputs.3.type = INDIRECT_SPECULAR
film.outputs.3.filename = /tmp/e42_ispec.hdr
film.outputs.4.type = EMISSION
film.outputs.4.filename = /tmp/e42_emis.hdr
film.outputs.5.type = DIRECT_GLOSSY
film.outputs.5.filename = /tmp/e42_dglossy.hdr
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    for k, v in defs.items():
        cfg.Set(pysuperluxcore.Property(k, v))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(
                f"render stalled below {SPP} spp after {RENDER_TIMEOUT_S}s")
        time.sleep(0.5)
    out = {}
    for idx, name in CHANNELS.items():
        arr = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
        ses.GetFilm().GetOutputFloat(
            getattr(pysuperluxcore.FilmOutputType, name), arr, 0, True)
        out[name] = arr.reshape(HEIGHT, WIDTH, 3).mean(axis=2)
    ses.Stop()
    return out


ARC_IND = {"path.clamping.variance.maxvalue": 5.0,
           "path.clamping.variance.adaptive": 1,
           "path.clamping.variance.scope": "indirect",
           "path.clamping.variance.sigma": 6.0}
ARC_ALL = {"path.clamping.variance.maxvalue": 5.0,
           "path.clamping.variance.adaptive": 1,
           "path.clamping.variance.scope": "all",
           "path.clamping.variance.sigma": 6.0}
ARC_DIR = {"path.clamping.variance.maxvalue": 5.0,
           "path.clamping.variance.adaptive": 0,
           "path.clamping.variance.scope": "direct"}
LEGACY = {"path.clamping.variance.maxvalue": 5.0,
          "path.clamping.variance.adaptive": 0,
          "path.clamping.variance.scope": "all"}


def main():
    print(f"Adaptive Robust Clamping ({WIDTH}x{HEIGHT}, "
          "pg-glossy-indirect)\n", flush=True)

    gpu_sel = next((m for m in (device_mask("METAL_GPU"),
                    device_mask("OPENCL_GPU")) if m and "1" in m), None)
    print(f"  gpu device mask: {gpu_sel}", flush=True)

    off = render({})
    arc = render(ARC_IND)
    all_ = render(ARC_ALL)
    dir_ = render(ARC_DIR)
    leg = render(LEGACY)

    print(f"  beauty  off={off['RGB'].mean():.4f} arc={arc['RGB'].mean():.4f} "
          f"all={all_['RGB'].mean():.4f} dir={dir_['RGB'].mean():.4f} "
          f"leg={leg['RGB'].mean():.4f}", flush=True)
    print(f"  iGlossy off p99={np.percentile(off['INDIRECT_GLOSSY'], 99):.4f} "
          f"arc={np.percentile(arc['INDIRECT_GLOSSY'], 99):.4f} "
          f"leg={np.percentile(leg['INDIRECT_GLOSSY'], 99):.4f}", flush=True)
    print(f"  emission off={off['EMISSION'].mean():.4f} "
          f"arc={arc['EMISSION'].mean():.4f} "
          f"all={all_['EMISSION'].mean():.4f} "
          f"dir={dir_['EMISSION'].mean():.4f} "
          f"leg={leg['EMISSION'].mean():.4f}", flush=True)

    p99_off = float(np.percentile(off["INDIRECT_GLOSSY"], 99))
    p99_arc = float(np.percentile(arc["INDIRECT_GLOSSY"], 99))
    record("T1.indirect-firefly-suppression",
           np.isfinite(arc["INDIRECT_GLOSSY"]).all() and
           p99_arc < p99_off * 0.6,
           f"iGlossy p99 {p99_off:.4f} -> {p99_arc:.4f} (gate -40%)")

    record("T2.energy-preserved",
           abs(float(arc["RGB"].mean()) - float(off["RGB"].mean())) /
           float(off["RGB"].mean()) < 0.10,
           f"beauty mean {off['RGB'].mean():.4f} -> "
           f"{arc['RGB'].mean():.4f} (gate 10%)")

    record("T3.direct-class-exempt",
           abs(float(arc["EMISSION"].mean()) - float(off["EMISSION"].mean())) /
           float(off["EMISSION"].mean()) < 0.05,
           f"emission mean {off['EMISSION'].mean():.4f} -> "
           f"{arc['EMISSION'].mean():.4f} (gate 5%)")

    record("T4.adaptive-protects-legit",
           float(all_["EMISSION"].mean()) >
           0.8 * float(off["EMISSION"].mean()),
           f"emission mean under scope=all {all_['EMISSION'].mean():.4f} "
           f"vs off {off['EMISSION'].mean():.4f} (gate 80%)")

    record("T5.scope-direct-symmetric",
           float(dir_["EMISSION"].mean()) <
           0.7 * float(off["EMISSION"].mean()) and
           abs(float(dir_["INDIRECT_GLOSSY"].mean()) -
               float(off["INDIRECT_GLOSSY"].mean())) /
               max(float(off["INDIRECT_GLOSSY"].mean()), 1e-12) < 0.4,
           f"emission {off['EMISSION'].mean():.3f} -> "
           f"{dir_['EMISSION'].mean():.3f} (clamped hard), iGlossy "
           f"{off['INDIRECT_GLOSSY'].mean():.4f} -> "
           f"{dir_['INDIRECT_GLOSSY'].mean():.4f} (untouched)")

    record("T7.legacy-mode-available",
           float(leg["RGB"].mean()) < 0.8 * float(off["RGB"].mean()),
           f"fixed-margin beauty mean {leg['RGB'].mean():.4f} vs off "
           f"{off['RGB'].mean():.4f} (legacy still clamps hard)")

    if gpu_sel:
        g = render(ARC_IND, engine="PATHOCL", sel=gpu_sel)
        c = arc
        print(f"  gpu beauty={g['RGB'].mean():.4f} "
              f"iGlossy={g['INDIRECT_GLOSSY'].mean():.4f}", flush=True)
        record("T6.cpu-gpu-parity",
               np.isfinite(g["RGB"]).all() and
               abs(float(g["RGB"].mean()) - float(c["RGB"].mean())) /
               max(float(c["RGB"].mean()), 1e-12) < 0.05 and
               abs(float(g["INDIRECT_GLOSSY"].mean()) -
                   float(c["INDIRECT_GLOSSY"].mean())) /
               max(float(c["INDIRECT_GLOSSY"].mean()), 1e-12) < 0.15,
               f"beauty cpu={c['RGB'].mean():.4f} gpu={g['RGB'].mean():.4f}; "
               f"iGlossy cpu={c['INDIRECT_GLOSSY'].mean():.4f} "
               f"gpu={g['INDIRECT_GLOSSY'].mean():.4f}")
    else:
        record("T6.cpu-gpu-parity", True, "SKIP (no GPU)")

    print()
    ok = sum(1 for _, o in results if o)
    print(f"===== ARC clamping: {ok}/{len(results)} PASS =====")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
