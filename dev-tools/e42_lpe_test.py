# SPDX-License-Identifier: Apache-2.0
#
# E42: Light Path Expressions (film.lpe.N.expression).
#
# Each expression compiles to a bounded NFA (<=32 states); every path
# carries one live-state bitmask per expression, stepped on camera (C),
# per-vertex class x direction symbols (<RD>, <GT>, ...) and on the
# terminal (L emitter / E environment). Accepted terminals route the
# contribution into the expression's channel buffer; outputs select the
# expression via film.outputs.N.type = LPE + .index.
#
# Scene: cornell box - every vertex is diffuse-reflect so paths decompose
# exactly as C <RD>* L. Checks:
#   T1 partition: CL + C<RD>L + C<RD><RD>+L ~= C.*L ~= RGB (same render)
#   T2 CL ~= EMISSION (same path class, same estimator)
#   T3 C<RD>L >= DIRECT_DIFFUSE (LPE direct = NEE + BSDF-sampled hits)
#   T4 wildcard/alternation grammar: C(.|<RD>)+L still ~= RGB
#   T5 CPU/GPU parity on all four channels (PATHOCL Metal/Vulkan)
#   T6 malformed expression raises; missing expression -> no channels
#   T7 EXR output carries LPE.<name>.R/G/B channels + output count
#
# Run:
#   python3.13 dev-tools/e42_lpe_test.py
#
import subprocess
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

EXRHEADER = (REPO.parent / "LuxCore/out/dependencies/full_deploy/host/openexr/3.4.14"
             "/Release/armv8/bin/exrheader")

WIDTH, HEIGHT = 256, 256
SPP = 64
TASK_COUNT = 1 << 16
RENDER_TIMEOUT_S = 300
OUTDIR = Path("/tmp/e42_lpe")
OUTDIR.mkdir(exist_ok=True)

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


LPES = (
    ("CL", "emit"),
    ("C<RD>L", "direct"),
    ("C<RD><RD>+L", "indirect"),
    ("C.*L", "total"),
)


def render(engine, sel=None, lpes=LPES, extra_cfg=""):
    scn = pysuperluxcore.Properties(str(REPO / "scenes/cornell/cornell.scn"))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)

    lpe_cfg = "".join(
        f"film.lpe.{i}.expression = {expr}\nfilm.lpe.{i}.name = {name}\n"
        for i, (expr, name) in enumerate(lpes))
    lpe_out = "".join(
        f"film.outputs.lpe{i}.type = LPE\nfilm.outputs.lpe{i}.index = {i}\n"
        f"film.outputs.lpe{i}.filename = {OUTDIR}/lpe_{name}.exr\n"
        for i, (expr, name) in enumerate(lpes))

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
opencl.task.count = {TASK_COUNT}
path.pathdepth.total = 5
film.imagepipelines.0.0.type = NOP
{lpe_cfg}
film.outputs.rgb.type = RGB
film.outputs.rgb.filename = {OUTDIR}/rgb.exr
film.outputs.em.type = EMISSION
film.outputs.em.filename = {OUTDIR}/emission.exr
film.outputs.dd.type = DIRECT_DIFFUSE
film.outputs.dd.filename = {OUTDIR}/ddiffuse.exr
{lpe_out}
{extra_cfg}
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {SPP} spp")
        time.sleep(0.25)

    film = ses.GetFilm()
    n = WIDTH * HEIGHT
    rgb = np.empty(n * 3, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB, rgb)
    outs = [rgb.reshape(HEIGHT, WIDTH, 3)]
    for i in range(len(lpes)):
        buf = np.empty(n * 3, dtype=np.float32)
        film.GetOutputFloat(pysuperluxcore.FilmOutputType.LPE, buf, i, True)
        outs.append(buf.reshape(HEIGHT, WIDTH, 3))
    for t, name in ((pysuperluxcore.FilmOutputType.EMISSION, "emission"),
                    (pysuperluxcore.FilmOutputType.DIRECT_DIFFUSE, "ddiffuse")):
        buf = np.empty(n * 3, dtype=np.float32)
        film.GetOutputFloat(t, buf)
        outs.append(buf.reshape(HEIGHT, WIDTH, 3))
    count = film.GetOutputCount(pysuperluxcore.FilmOutputType.LPE)
    film.SaveOutputs()
    ses.Stop()
    # outs: [rgb, *lpes, emission, ddiffuse]
    return outs, count


def lum(a):
    return a.mean(axis=2)


def med_rel(a, b, eps=1e-4):
    """Median |a-b|/(|b|+eps) over pixels where |b| is meaningful."""
    la, lb = lum(a), lum(b)
    m = lb > 0.01
    return float(np.median(np.abs(la[m] - lb[m]) / (lb[m] + eps))) if m.any() else float("nan")


def main():
    cpu = "PATHCPU"

    outs, count = render(cpu)
    rgb, emit, direct, indirect, total, emission, ddiffuse = outs

    # T1: the three classes partition all contributions
    part = emit + direct + indirect
    d1 = med_rel(part, total)
    d2 = med_rel(total, rgb)
    record("T1 partition", d1 < 0.03 and d2 < 0.03,
           f"|emit+direct+indirect-total|={d1:.4f} |total-rgb|={d2:.4f}")

    # T2: CL == first-vertex emission channel
    d3 = med_rel(emit, emission)
    record("T2 CL~EMISSION", d3 < 0.02, f"|CL-emission|={d3:.4f}")

    # T3: C<RD>L is a superset of DIRECT_DIFFUSE (NEE + sampled hits)
    ld, ldd = lum(direct), lum(ddiffuse)
    m = ldd > 0.01
    superset = float(np.mean(ld[m] >= ldd[m] * 0.9)) > 0.9 if m.any() else False
    record("T3 direct>=DDIFFUSE", superset and lum(direct).mean() > 0.01,
           f"direct={lum(direct).mean():.4f} ddiffuse={lum(ddiffuse).mean():.4f} "
           f"superset_px={np.mean(ld[m] >= ldd[m] * 0.9) if m.any() else 0:.3f}")

    # T4: wildcard/alternation compiles and still covers all paths
    outs4, _ = render(cpu, lpes=(("C(.|<RD>)+L", "wild"),))
    wild = outs4[1]
    d4 = med_rel(wild, outs4[0])
    record("T4 wild/alternation", d4 < 0.03, f"|wild-rgb|={d4:.4f}")

    # T5: GPU parity on every LPE channel
    sel = device_mask("METAL_GPU") or device_mask("VULKAN_GPU")
    if sel:
        try:
            gouts, _ = render("PATHOCL", sel=sel)
            ok = True
            det = []
            for name, i in (("emit", 1), ("direct", 2), ("indirect", 3), ("total", 4)):
                d = med_rel(gouts[i], outs[i])
                det.append(f"{name}={d:.3f}")
                ok = ok and d < 0.15
            d5 = med_rel(gouts[4], gouts[0])
            det.append(f"gpu|total-rgb|={d5:.3f}")
            ok = ok and d5 < 0.03
            record("T5 GPU parity", ok, " ".join(det))
        except Exception as e:
            record("T5 GPU parity", False, f"exception: {e}")
    else:
        record("T5 GPU parity", True, "no GPU device - skipped")

    # T6a: malformed expression must raise
    bad = False
    try:
        render(cpu, lpes=(("C(", "bad"),))
    except Exception as e:
        bad = True
    record("T6a malformed", bad, "exception raised" if bad else "no exception")

    # T6b: no film.lpe.* -> zero LPE outputs
    _, cnt0 = render(cpu, lpes=())
    record("T6b no-LPE count", cnt0 == 0, f"GetOutputCount={cnt0}")

    # T7: EXR channel names + output count
    chans_ok, names_ok = False, True
    if EXRHEADER.exists():
        hdr = subprocess.run([str(EXRHEADER), str(OUTDIR / "lpe_emit.exr")],
                             capture_output=True, text=True).stdout
        chans_ok = all(f"LPE.emit.{c}" in hdr for c in "RGB")
        hdr2 = subprocess.run([str(EXRHEADER), str(OUTDIR / "lpe_total.exr")],
                              capture_output=True, text=True).stdout
        names_ok = "LPE.total.R" in hdr2
    record("T7 EXR channels", chans_ok and names_ok and count == len(LPES),
           f"emit-chans={chans_ok} total-name={names_ok} count={count}")

    fails = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(fails)}/{len(results)} passed"
          + (f" - failed: {fails}" if fails else ""))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
