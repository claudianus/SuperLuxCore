#!/usr/bin/env python3
"""E4: MNEE manifold seed cache on GPU.

The seed cache stores converged single-vertex MNEE solutions in a hashed
world-space grid and reuses them as Newton warm-start seeds for nearby
attempts on the same occluder/light (path.mnee.seedcache, default on).

Correctness contract: the cache is a basin-selection rescue, never the
authority on which root is found. For glass (eta != 1) the cold line
seed runs first and the cache only re-seeds a FAILED solve; for mirror
(eta == 1) the cache gets first refusal because the cold seed costs an
extra trace. Rescued solves can only add valid contributions, so
cache-on may legitimately exceed cache-off - the contract is "never
drains", not "statistically identical" (cache-first seeding once drained
~5% of the caustic by pinning attempts to the first-cached basin).

Scope (measured, doc/features/mnee.md): the cache accelerates the
*single-vertex* solve only. That is the default path
(path.mnee.maxspecular = 1). Mirror blocking is almost always
opposite-side -> the expensive chain solver, which is uncached, so this
test targets the glass (refraction) warm-start path.

Checks:
  T0 ACTIVITY - render path.mnee.enable=0 vs 1. With MNEE off the
     shadow ray through the delta-specular sheet fails, so the caustic
     region is dark; with MNEE on it resolves. The caustic mean must be
     significantly higher with MNEE on - if it is not, the scene never
     triggers MNEE and every other check below is vacuous. (This is the
     regression the original transparency.shadow=1 scenes fell into:
     shadow rays passed through, MNEE never ran, and the 'unbiased'
     result was trivially true.)
  T1 unbiased - cache-on mean matches cache-off (paired seeds, same SPP)
  T2 finite - cache-on image is finite everywhere (no NaN/Inf)
  T3 caustic-region energy preserved on/off (a cached seed can
     legitimately land Newton in a different valid basin on a caustic
     pixel, so a pixelwise diff is expected there; it must not drain the
     caustic or shift the global mean)

Scene: scenes/mnee/seedcache.scn - a point light inside a closed
bumpy-glass sphere over a diffuse floor; floor->light shadow rays
refract once through the sphere surface so single-vertex MNEE solves
fire and produce a refracted caustic pool on the floor.

Safety: runs with a small task count and a hard per-render deadline
instead of an infinite wait; run it under an external shell timeout too.
"""
import sys, os, time
sys.path.insert(0, "/Users/modumaru/Desktop/code/superluxcore/LuxCore/out/build/src/pysuperluxcore/Release")
import pysuperluxcore
import numpy as np

# PLY refs inside the .scn are cwd-relative: chdir into the scene dir.
SCENE_DIR = "/Users/modumaru/Desktop/code/superluxcore/LuxCore/scenes/mnee"
SCENE = "seedcache.scn"
TASK_COUNT = 8192       # small: development-time bound, not production size
RENDER_TIMEOUT_S = 240  # hard deadline; far below the macOS GPU watchdog
SPP = 256
W, H = 192, 144


def render(mnee_enable, seedcache, seed=1):
    scn = pysuperluxcore.Properties(SCENE)  # ply refs resolve via chdir(SCENE_DIR)
    sc = pysuperluxcore.Scene(); sc.Parse(scn)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = PATHOCL
sampler.type = SOBOL
sampler.sobol.rng0 = {seed}
batch.haltspp = {SPP}
opencl.task.count = {TASK_COUNT}
path.mnee.enable = {mnee_enable}
path.mnee.seedcache = {seedcache}
film.imagepipelines.0.0.type = NOP
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    t0 = time.monotonic()
    ok = False
    while time.monotonic() - t0 < RENDER_TIMEOUT_S:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            ok = True
            break
        time.sleep(0.5)
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    ses.Stop()
    img = rgb.reshape(H, W, 3)
    if not ok:
        print(f"    mnee={mnee_enable} seedcache={seedcache}: TIMED OUT at {RENDER_TIMEOUT_S}s", flush=True)
    return img


def main():
    os.chdir(SCENE_DIR)
    pysuperluxcore.Init()  # enable SLG_LOG so kernel compile / progress is visible
    print(f"MNEE seed cache on GPU ({SCENE}, {W}x{H}, {SPP}spp)", flush=True)
    checks = []

    # --- T0: prove MNEE actually runs in this scene ---------------------
    # Without MNEE the refracted caustic cannot be connected, so the
    # bright region collapses. If it does not, the scene is vacuous.
    img_off_mnee = render(0, 0)
    img_on_mnee = render(1, 1)
    lum_off = img_off_mnee.mean(axis=2)
    lum_on = img_on_mnee.mean(axis=2)
    bright = lum_on > np.percentile(lum_on, 90)
    caustic_off = img_off_mnee[bright].mean()
    caustic_on = img_on_mnee[bright].mean()
    t0_gain = caustic_on / max(caustic_off, 1e-9)
    checks.append(("T0.mnee-actually-fires", t0_gain > 1.5,
                   f"caustic mean mnee=on {caustic_on:.4f} vs mnee=off "
                   f"{caustic_off:.4f} (x{t0_gain:.2f})"))
    print(f"  [{'PASS' if checks[-1][1] else 'FAIL'}] {checks[-1][0]}: {checks[-1][2]}", flush=True)
    if not checks[-1][1]:
        print("  scene does not exercise MNEE - aborting (vacuous)", flush=True)
        print("===== MNEE seed cache: FAIL =====")
        return

    # --- T1/T2/T3: seed-cache on/off equivalence on an MNEE-active scene -
    off = (render(1, 0, 1) + render(1, 0, 2)) / 2
    on = (render(1, 1, 1) + render(1, 1, 2)) / 2

    m_off, m_on = off.mean(), on.mean()
    lum = off.mean(axis=2)
    bright = lum > np.percentile(lum, 90)
    caustic_ratio = on[bright].mean() / max(off[bright].mean(), 1e-9)

    checks += [
        # The cache is a failure-rescue accelerator (cold-first policy):
        # rescued roots only ADD contribution where the cold solve failed,
        # so cache-on may legitimately EXCEED cache-off - the contract is
        # "never drains", not "statistically identical". A drained caustic
        # (the original bug: cache-first seeds pinned attempts to the
        # first-cached basin) fails the lower bound; a blow-up fails the
        # upper sanity bound.
        ("T1.no-global-drain", m_on > m_off * 0.97 and m_on < m_off * 1.5,
         f"on={m_on:.5f} off={m_off:.5f} ratio={m_on/max(m_off,1e-9):.4f}"),
        ("T2.finite", np.isfinite(on).all(),
         f"finite={np.isfinite(on).all()}"),
        ("T3.caustic-energy-preserved", caustic_ratio > 0.97 and caustic_ratio < 1.5,
         f"caustic on/off={caustic_ratio:.4f}"),
    ]
    for name, ok, info in checks[1:]:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {info}", flush=True)

    allok = all(ok for _, ok, _ in checks)
    print(f"===== MNEE seed cache: {'ALL PASS' if allok else 'FAIL'} =====", flush=True)


if __name__ == "__main__":
    main()
