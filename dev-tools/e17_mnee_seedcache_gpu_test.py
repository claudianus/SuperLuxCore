#!/usr/bin/env python3
"""E4: MNEE manifold seed cache on GPU.

The seed cache stores converged single-vertex MNEE solutions in a hashed
world-space grid and reuses them as Newton warm-start seeds for nearby
attempts on the same occluder/light (path.mnee.seedcache, default on).

Correctness contract: the cache only changes WHERE Newton starts, never
what is accepted - every candidate seed still goes through the same
half-vector constraint solve and post-solve mode checks. The rendered
result must therefore be statistically identical to seedcache=off.

Checks:
  T1 cache-on mean matches cache-off (paired seeds, same SPP) within 2%
  T2 cache-on image is finite everywhere (no NaN/Inf)
  T3 the on/off difference is confined to MNEE-affected pixels and the
     caustic-region mean is preserved (a cached seed can legitimately
     land Newton in a different valid basin on a caustic pixel, so a
     pixelwise diff is expected there - it must NOT appear elsewhere
     and must not drain the caustic)

Scene: scenes/juice/test.scn - a point light behind a glass object:
shadow rays blocked by delta specular material trigger single-vertex
MNEE solves every sample.

Safety: the earlier ReSTIR-visibility development wedged the GPU twice
via an undersized ray buffer (WindowServer userspace-watchdog panic).
This test therefore runs with a small task count and a hard per-render
deadline instead of an infinite wait; run it under an external shell
timeout as well.
"""
import sys, time
sys.path.insert(0, "/Users/modumaru/.zcode/workspace/default/LuxCore/out/build/src/pyluxcore/Release")
import pyluxcore
import numpy as np

# PLY refs inside the .scn are cwd-relative: run from scenes/juice.
SCENE_DIR = "/Users/modumaru/.zcode/workspace/default/LuxCore/scenes/juice"
TASK_COUNT = 8192       # small: development-time bound, not production size
RENDER_TIMEOUT_S = 240  # hard deadline; far below the macOS GPU watchdog
SPP = 64
W, H = 160, 120


def render(seedcache, scene, seed=1):
    scn = pyluxcore.Properties(scene)
    sc = pyluxcore.Scene(); sc.Parse(scn)
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = PATHOCL
sampler.type = SOBOL
sampler.sobol.rng0 = {seed}
batch.haltspp = {SPP}
opencl.task.count = {TASK_COUNT}
path.mnee.enable = 1
path.mnee.seedcache = {seedcache}
""")
    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, sc))
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
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    ses.Stop()
    img = rgb.reshape(H, W, 3)
    if not ok:
        print(f"  seedcache={seedcache}: TIMED OUT at {RENDER_TIMEOUT_S}s", flush=True)
    return img, time.monotonic() - t0


def check(scene, label):
    print(f"  [{label}]")
    results = []
    for seedcache in (0, 1, 0, 1):
        img, dt = render(seedcache, scene)
        finite = np.isfinite(img).all()
        results.append(img)
        print(f"    seedcache={seedcache}: mean={img.mean():.5f} "
              f"max={img.max():.3f} finite={finite} t={dt:.0f}s", flush=True)

    off = (results[0] + results[2]) / 2
    on = (results[1] + results[3]) / 2

    m_off, m_on = off.mean(), on.mean()
    d = np.abs(on - off).mean(axis=2)
    lum = off.mean(axis=2)
    # MNEE-affected pixels are the caustic (bright) ones: the diff must
    # live there and must not drain their mean (solve coverage loss).
    bright = lum > np.percentile(lum, 90)
    caustic_ratio = on[bright].mean() / max(off[bright].mean(), 1e-9)
    diff_outside = d[~bright].max() if (~bright).any() else 0.

    checks = [
        ("T1.unbiased-mean", abs(m_on - m_off) / max(m_off, 1e-9) < 0.02,
         f"on={m_on:.5f} off={m_off:.5f}"),
        ("T2.finite", np.isfinite(on).all(), f"finite={np.isfinite(on).all()}"),
        ("T3a.diff-confined-to-caustic", diff_outside < 1e-4,
         f"max|diff| outside bright10%={diff_outside:.6f}"),
        ("T3b.caustic-mean-preserved", abs(caustic_ratio - 1.) < 0.02,
         f"caustic on/off={caustic_ratio:.4f}"),
    ]
    allok = True
    for name, ok, info in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {info}")
        allok &= ok
    return allok


def main():
    print(f"MNEE seed cache on GPU (juice {W}x{H}, {SPP}spp)")
    ok_glass = check(SCENE_DIR + "/test.scn", "glass occluder (line-seed path)")
    ok_mirror = check(SCENE_DIR + "/test-mirror.scn", "mirror occluder (seed-trace path)")
    print(f"===== MNEE seed cache: "
          f"{'ALL PASS' if ok_glass and ok_mirror else 'FAIL'} =====")


if __name__ == "__main__":
    main()
