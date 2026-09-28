# SPDX-License-Identifier: Apache-2.0
#
# E52: statistical adaptive error / noise-level halt (E5).
#
# Exercises FilmAdaptiveError on CPU and GPU films:
#   - film.adaptiveerror.target drives a per-pixel relative standard
#     error estimate (VARIANCE/SAMPLECOUNT -> relErr -> 3x3 dilation ->
#     95th-percentile global noise level).
#   - stats.renderengine.noiselevel must exist, be finite after warmup,
#     and strictly decrease as samples accumulate.
#   - A loose target must halt the render (stats.renderengine.convergence
#     reaching 1.0) on both PATHCPU and PATHOCL.
#   - A tight target must NOT halt inside the time cap.
#   - The NOISE channel must carry a finite, spatially-varying
#     importance map (0..1) once the first test has run.
#
# Every render runs in its own subprocess so a crash surfaces as a FAIL
# row instead of killing the suite.
#
# Run from the repo root:
#   python3.13 dev-tools/e52_adaptive_noise_test.py

import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

WIDTH, HEIGHT = 128, 128
SCENE = REPO / "scenes/cornell/cornell.scn"
RENDER_TIMEOUT_S = 150
CHILD_TIMEOUT_S = 210


# -----------------------------------------------------------------------------
# child mode: render with an adaptiveerror target; report the noise level
# trajectory, halt outcome and NOISE-channel statistics
# -----------------------------------------------------------------------------

def child_render(engine, target, warmup, step, cap_s):
    import pysuperluxcore

    os.chdir(str(REPO))
    scene = pysuperluxcore.Scene()
    scene.Parse(pysuperluxcore.Properties(str(SCENE)))

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
renderengine.seed = 17
batch.halttime = 0
batch.haltspp = 0
film.adaptiveerror.target = {target}
film.adaptiveerror.warmup = {warmup}
film.adaptiveerror.step = {step}
opencl.task.count = 65536
opencl.native.threads.count = 0
film.imagepipeline.0.type = TONEMAP_LINEAR
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()

    first_nl = None
    last_nl = None
    halted = False
    t0 = time.monotonic()
    while time.monotonic() - t0 < cap_s:
        time.sleep(1.0)
        ses.UpdateStats()
        stats = ses.GetStats()
        conv = stats.Get("stats.renderengine.convergence").GetFloat()
        nl = stats.Get("stats.renderengine.noiselevel").GetFloat()
        if np.isfinite(nl):
            if first_nl is None:
                first_nl = nl
            last_nl = nl
        if conv >= 1.0:
            halted = True
            break
    ses.Stop()

    # NOISE channel must carry the statistical importance map
    noise = np.full(WIDTH * HEIGHT, np.nan, dtype=np.float32)
    try:
        ses.GetFilm().GetOutputFloat(
            pysuperluxcore.FilmOutputType.NOISE, noise, 0, True)
    except Exception:
        pass
    noise_finite = bool(np.isfinite(noise).all())
    noise_var = float(np.nanstd(noise))

    print(f"RESULT first_nl={first_nl if first_nl is not None else 'nan'} "
          f"last_nl={last_nl if last_nl is not None else 'nan'} "
          f"halted={int(halted)} conv={conv:.4f} "
          f"noise_finite={int(noise_finite)} noise_var={noise_var:.4f}",
          flush=True)


def render(engine, target, cap_s, warmup=4, step=4):
    args = [sys.executable, str(Path(__file__).resolve()), "--render",
            engine, str(target), str(warmup), str(step), str(cap_s)]
    try:
        r = subprocess.run(args, capture_output=True, text=True,
                           timeout=CHILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"timeout after {CHILD_TIMEOUT_S}s")

    res = {}
    for line in r.stdout.splitlines():
        if line.startswith("RESULT "):
            for tok in line.split()[1:]:
                k, v = tok.split("=")
                res[k] = v
    if not res:
        tail = (r.stderr or r.stdout or "")[-400:].strip()
        raise RuntimeError(f"render died (rc={r.returncode}) {tail!r}")
    return res


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


# -----------------------------------------------------------------------------
# suite
# -----------------------------------------------------------------------------

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--render":
        child_render(sys.argv[2], float(sys.argv[3]), int(sys.argv[4]),
                     int(sys.argv[5]), float(sys.argv[6]))
        return

    ok = True
    print(f"Adaptive noise-level suite ({WIDTH}x{HEIGHT})\n", flush=True)

    # 1. Loose target must halt quickly, on CPU and GPU alike.
    for engine, cap in (("PATHCPU", 60), ("PATHOCL", 90)):
        try:
            r = render(engine, 0.6, cap)
            ok &= check(
                r["halted"] == "1"
                and float(r["last_nl"]) <= 0.6 * 1.25  # allow overshoot between tests
                and r["noise_finite"] == "1",
                f"{engine} loose halt",
                f"halted={r['halted']} last_nl={r['last_nl']} "
                f"conv={r['conv']} noise_var={r['noise_var']}")
        except Exception as e:
            ok &= check(False, f"{engine} loose halt", e)

    # 2. Tight target must not halt early; noise level must still drop.
    for engine, cap in (("PATHCPU", 40), ("PATHOCL", 60)):
        try:
            r = render(engine, 0.002, cap)
            first = float(r["first_nl"])
            last = float(r["last_nl"])
            ok &= check(
                r["halted"] == "0" and last < first * 0.8,
                f"{engine} tight no-halt+decrease",
                f"halted={r['halted']} first_nl={first:.4f} "
                f"last_nl={last:.4f} ratio={last / max(first, 1e-9):.3f}")
        except Exception as e:
            ok &= check(False, f"{engine} tight no-halt+decrease", e)

    # 3. NOISE map must be spatially varying (nonzero std) once tests ran.
    try:
        r = render("PATHCPU", 0.05, 30)
        ok &= check(
            r["noise_finite"] == "1" and float(r["noise_var"]) > 1e-4,
            "PATHCPU NOISE map",
            f"noise_finite={r['noise_finite']} noise_var={r['noise_var']}")
    except Exception as e:
        ok &= check(False, "PATHCPU NOISE map", e)

    print(f"{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
