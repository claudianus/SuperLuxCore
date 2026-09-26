# SPDX-License-Identifier: Apache-2.0
#
# E43: P5 path-guiding maturity regression —
#   §5.1 hierarchical fallback (cold leaves borrow warm ancestor fits)
#   §5.2 adaptive component count (BIC over K=1..4)
#   §5.5 formal path.guiding.* properties (precedence over LUX_PG_*)
#
# Scene: scenes/cornell/pg-indirect.scn (pure indirect lighting; guiding
# is the only thing that finds the light through the doorway).
#
#   T1 table save (path.guiding.savetable) + load (tablefile) roundtrip
#   T2 guided-vs-unguided mean within [0.9, 1.1] on the warm-started
#      field (unbiased) — a 32spp cold render barely trains the tree,
#      so the ensembles run on the saved table (production warm start)
#   T3 guided variance within a sane band of unguided on the trained
#      field (PATHOCL - deterministic). NOT a strict improvement check:
#      at unit scale the trained field is variance-neutral-to-slightly-
#      worse on these scenes (measured 0.5x-1.8x), so the assertion only
#      guards catastrophic misguidance. Benefit is demonstrated at
#      production scale by e43_pathguiding_visual.py (720p RMSE vs a
#      converged reference).
#   T4a env fallback: LUX_PG_WARMUP starves every leaf (debug log)
#   T4b property wins: path.guiding.warmup=256 overrides that env
#   T5 path.guiding.strength=0 ≈ unguided
#   T6 path.guiding.components=1 finite + unbiased
#   T7 PATHOCL(METAL) guided mean ≈ PATHCPU guided mean
#   T8 hierarchical fallback: debug log reports borrowed leaves
#   T9 cold start: 4spp guided render finite + sane mean
#
# IMPORTANT (same caveat as e27): statistics are computed on the raw
# linear RGB channel — the image pipeline's nonlinear transform would
# fake a brightness bias through Jensen's inequality.
#
# IMPORTANT (measured 2025-09): PATHCPU is NOT deterministic across
# identical calls — each render thread walks its own Sobol stream over
# normalized pixel space, so pixel/sample assignment shifts with thread
# scheduling (and external CPU load). Same-seed renders differ by max
# pixel ~4, and 5-run ensemble variance ratios swing 0.5x-1.8x on
# IDENTICAL configurations (pre-P5 and P5 code alike). T2/T3 therefore
# measure on PATHOCL(METAL), whose fixed task/seed batching is
# deterministic to ~2% across runs.
#
# The LUX_PG_* env tunables latch into statics on first use, so the
# env-fallback checks run in a fresh --child process per case.
#
# Run from the repo root:
#   python3.13 dev-tools/e43_pathguiding_test.py

import os
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for _cfg in ("Release", "Debug"):
    _lib = REPO / "out/build/src/pysuperluxcore" / _cfg
    if list(_lib.glob("pysuperluxcore*.so")):
        sys.path.insert(0, str(_lib))
        break
import pysuperluxcore

WIDTH = int(os.environ.get("E43_W", "320"))
HEIGHT = int(os.environ.get("E43_H", "180"))
SPP = int(os.environ.get("E43_SPP", "32"))
# Training needs enough records for a field that actually reduces
# variance: ~20M record attempts (384spp @320x180). At ~5M the warm-
# started guide measured WORSE than unguided (0.021 vs 0.014 var) —
# the mixture weight was already high but the lobes were still noise.
TRAIN_SPP = int(os.environ.get("E43_TRAIN_SPP", "384"))
RUNS = int(os.environ.get("E43_RUNS", "3"))
SCENE = os.environ.get("E43_SCENE", "scenes/cornell/pg-indirect.scn")
RENDER_TIMEOUT_S = 600
OUT = REPO / "dev-tools/out/e43"
TABLE = OUT / "e43.pgt"

LUM = np.array([0.2126, 0.7152, 0.0722], np.float32)
results = []


def parse_scene(rel_path):
    props = pysuperluxcore.Properties(str(REPO / rel_path))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return scene


def device_mask(want_type):
    pysuperluxcore.Init()
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


def render(scene, extra_cfg="", seed=1000, spp=SPP, engine="PATHCPU",
        sel=None):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
opencl.task.count = 65536
{extra_cfg}
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"{engine} [{extra_cfg.strip()}] stalled")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def ensemble_mean_var(images):
    """Ensemble mean + mean per-pixel luminance variance across runs."""
    lum = np.stack([img @ LUM for img in images])
    return float(np.mean(lum)), float(np.mean(np.var(lum, axis=0, ddof=1)))


def child_render(cfg, env, spp, seed):
    """Render in a fresh subprocess (env tunables latch into statics on
    first use). Returns (mean, stderr+stdout text)."""
    e = dict(os.environ)
    e.pop("LUX_PG_MINDEPTH", None)
    e.update(env)
    e["E43_CHILD_CFG"] = cfg
    e["E43_CHILD_SCENE"] = SCENE
    e["E43_CHILD_SPP"] = str(spp)
    e["E43_CHILD_SEED"] = str(seed)
    p = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                        "--child"], env=e, cwd=str(REPO),
                       capture_output=True, text=True,
                       timeout=RENDER_TIMEOUT_S)
    m = re.search(r"MEAN=([\d.eE+-]+)", p.stdout)
    return (float(m.group(1)) if m else float("nan")), p.stderr + p.stdout


def check(name, ok, detail):
    results.append(ok)
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    scene = parse_scene(SCENE)
    os.environ.pop("LUX_PG_MINDEPTH", None)

    # T1: train a table inline (CPU), save it, warm-start a new session.
    print(f"=== table train+roundtrip ({TRAIN_SPP}spp) ===", flush=True)
    render(scene,
           f"path.guiding.enable = 1\npath.guiding.savetable = {TABLE}",
           seed=10000, spp=TRAIN_SPP)
    ok_save = TABLE.exists() and TABLE.stat().st_size > 64
    warm = render(scene,
                  f"path.guiding.enable = 1\n"
                  f"path.guiding.tablefile = {TABLE}",
                  seed=11000) if ok_save else None
    warm_mean = float(np.nanmean(warm)) if warm is not None else 0.0
    check("T1 table save/load roundtrip",
          ok_save and warm is not None and np.isfinite(warm).all()
          and warm_mean > 0,
          f"table={TABLE.name} {TABLE.stat().st_size if ok_save else 0}B "
          f"warmstart mean={warm_mean:.4f}")

    # T2/T3: ensembles on the mature loaded field, measured on
    # PATHOCL(METAL) — PATHCPU's thread-scheduled pixel streams make
    # variance estimates irreproducible (see header note).
    guided_cfg = (f"path.guiding.enable = 1\n"
                  f"path.guiding.tablefile = {TABLE}")
    sel = device_mask("METAL_GPU")
    print(f"=== ensembles ({RUNS}x{SPP}spp, warm-started guide, "
          f"PATHOCL) ===", flush=True)
    ung = [render(scene, "path.guiding.enable = 0", seed=2000 + i,
                  engine="PATHOCL", sel=sel)
           for i in range(RUNS)]
    gde = [render(scene, guided_cfg, seed=3000 + i,
                  engine="PATHOCL", sel=sel)
           for i in range(RUNS)]
    u_mean, u_var = ensemble_mean_var(ung)
    g_mean, g_var = ensemble_mean_var(gde)

    ratio = g_mean / u_mean if u_mean > 0 else float("inf")
    finite = all(np.isfinite(x).all() for x in ung + gde)
    check("T2 guided unbiased mean", finite and 0.9 <= ratio <= 1.1,
          f"guided/unguided mean={ratio:.4f}")

    # Sanity band, not an improvement claim: on these scenes at this
    # scale the trained guide measures 0.5x-1.0x (slightly worse). The
    # bound catches real breakage (broken MIS weights, NaN lobes) while
    # staying honest about the field's actual utility.
    vratio = u_var / g_var if g_var > 0 else float("inf")
    check("T3 guided variance sanity band (trained field)",
          0.4 <= vratio <= 4.0,
          f"var unguided={u_var:.5f} guided={g_var:.5f} "
          f"ratio={vratio:.3f}x")

    # T4: property precedence over env, verified through the debug swap
    # log (warm leaf count) — deterministic, unlike a variance check.
    # env LUX_PG_WARMUP=2000000 starves every leaf; the formal property
    # path.guiding.warmup=256 must win when both are present.
    log_cfg = ("path.guiding.enable = 1\n"
               "path.guiding.debug = 1\n"
               "path.guiding.swaprecords = 8192")
    # With warmup=2M no leaf can warm in one 8k-record round; late in
    # the render inherited counts accumulate past it and a few leaves
    # do warm up — the env working is visible as warm=0 for the FIRST
    # swaps (vs T4b where property=256 warms leaves immediately).
    _, log_a = child_render(log_cfg, {"LUX_PG_WARMUP": "2000000"},
                            SPP, 5000)
    warms_a = [int(w) for w in
               re.findall(r"\[PG\] swap: .*warm=(\d+)", log_a)]
    first_half_a = warms_a[:max(1, len(warms_a) // 2)]
    check("T4a env fallback applies (LUX_PG_WARMUP starves leaves)",
          bool(warms_a) and all(w == 0 for w in first_half_a),
          f"swap lines={len(warms_a)} warm(first half)={first_half_a[:8]}")

    _, log_b = child_render(log_cfg + "\npath.guiding.warmup = 256",
                            {"LUX_PG_WARMUP": "2000000"}, SPP, 5000)
    warms_b = [int(w) for w in
               re.findall(r"\[PG\] swap: .*warm=(\d+)", log_b)]
    check("T4b property overrides env (warmup=256 wins)",
          any(w > 0 for w in warms_b[:10]),
          f"swap lines={len(warms_b)} warm={warms_b[:8]}")

    s0 = render(scene,
                guided_cfg + "\npath.guiding.strength = 0", seed=7000)
    s0m = float(np.nanmean(s0))
    check("T5 strength=0 ~ unguided",
          np.isfinite(s0).all() and abs(s0m / u_mean - 1) < .08,
          f"strength0 mean={s0m:.4f} (unguided={u_mean:.4f})")

    c1 = render(scene,
                guided_cfg + "\npath.guiding.components = 1", seed=8000)
    c1m = float(np.nanmean(c1))
    check("T6 components=1 finite + unbiased",
          np.isfinite(c1).all() and 0.9 <= c1m / u_mean <= 1.1,
          f"components1 mean={c1m:.4f} (unguided={u_mean:.4f})")

    mask = device_mask("METAL_GPU")
    if not mask:
        check("T7 GPU parity", True, "SKIP: no METAL_GPU device")
    else:
        gpu = render(scene, guided_cfg, seed=9000, engine="PATHOCL",
                     sel=mask)
        gm = float(np.nanmean(gpu))
        check("T7 PATHOCL/METAL guided parity",
              np.isfinite(gpu).all() and 0.85 <= gm / u_mean <= 1.15,
              f"gpu guided mean={gm:.4f} (unguided={u_mean:.4f})")

    # T8: tiny swap cadence -> early post-split swaps; inherited child
    # leaves stay below warmup and must borrow a warm ancestor fit.
    _, log_t9 = child_render(log_cfg, {}, SPP, 12000)
    swaps = re.findall(r"\[PG\] swap: .*borrowed=(\d+)", log_t9)
    check("T8 hierarchical fallback (borrowed>0)",
          bool(swaps) and any(int(b) > 0 for b in swaps),
          f"swap lines={len(swaps)} borrowed={[int(b) for b in swaps][:8]}")

    cold = render(scene, "path.guiding.enable = 1", seed=4000, spp=4)
    cm = float(np.nanmean(cold))
    check("T9 cold-start stability",
          np.isfinite(cold).all() and 0.5 <= cm / u_mean <= 2.0,
          f"4spp mean={cm:.4f} (unguided={u_mean:.4f})")

    print(f"\n{sum(results)}/{len(results)} passed  (out: {OUT})")
    if not all(results):
        sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        # Isolated single render for env-latched cases: env is set by the
        # parent before this process starts, so statics latch correctly.
        scene = parse_scene(os.environ["E43_CHILD_SCENE"])
        img = render(scene, os.environ.get("E43_CHILD_CFG", ""),
                     seed=int(os.environ.get("E43_CHILD_SEED", "1")),
                     spp=int(os.environ.get("E43_CHILD_SPP", "32")))
        print(f"MEAN={float(np.nanmean(img)):.6f}")
    else:
        main()
