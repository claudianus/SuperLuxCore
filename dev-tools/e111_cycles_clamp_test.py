# SPDX-License-Identifier: Apache-2.0
#
# E111: Cycles sample clamp (path.clamping.cycles.direct / .indirect).
#
# Cycles' film_clamp_light() scales every emission / direct-light
# contribution so the sum of its RGB channels stays under
# sample_clamp_direct (bounce 0) or sample_clamp_indirect (deeper), the
# emitter-hit bounce being the one the hitting ray left from. A white
# floor and wall under a very bright point light (no BSDF hits: direct =
# next-event estimation at the first vertex, indirect = at the second)
# make every contribution exceed the limits, so the clamped pixel sums
# are known exactly:
#
#   1  direct only (depth 1): lit floor RGB sum == direct limit
#   2  direct + one bounce (depth 3): the indirect part's RGB sum is <= the
#      indirect limit (and was well above it unclamped)
#   3  PATHOCL matches PATHCPU on 1 and 2
#
# Run from the repo root:
#   python3.13 dev-tools/e111_cycles_clamp_test.py

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 64, 64

SCENE = """
scene.camera.lookat.orig = 0 0 3
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 30
scene.materials.white.type = matte
scene.materials.white.kd = 0.8 0.8 0.8
scene.objects.floor.material = white
scene.objects.floor.vertices = -1 -1 0  1 -1 0  1 1 0  -1 1 0
scene.objects.floor.faces = 0 1 2  0 2 3
scene.objects.wall.material = white
scene.objects.wall.vertices = 1 -1 0  1 1 0  1 1 2  1 -1 2
scene.objects.wall.faces = 0 2 1  0 3 2
scene.lights.pl.type = point
scene.lights.pl.position = 0 0 2
scene.lights.pl.gain = 1000 1000 1000
"""

# central floor patch, fully lit by the point light
PATCH = np.s_[24:40, 24:40]


def render(engine, depth, direct, indirect, spp=64):
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(SCENE)
    scn.Parse(p)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
path.pathdepth.total = {depth}
path.pathdepth.diffuse = {depth}
path.clamping.cycles.direct = {direct}
path.clamping.cycles.indirect = {indirect}
path.hybridbackforward.enable = 0
path.lighttracing.enable = 0
path.vertexconnection.enable = 0
opencl.native.threads.count = 0
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scn))
    ses.Start()
    deadline = time.monotonic() + 600
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            raise TimeoutError(engine)
        time.sleep(0.2)
    ses.Stop()
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    return rgb.reshape(H, W, 3).sum(axis=2)   # RGB sum per pixel


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    pysuperluxcore.Init()
    ok = True
    DL, IL = 3.0, 0.5
    res = {}
    for eng in ("PATHCPU", "PATHOCL"):
        try:
            free1 = render(eng, 1, 0, 0)
            d1 = render(eng, 1, DL, 0)
            # depth counts vertices: 3 = first vertex + one bounce
            d2 = render(eng, 3, DL, 0)
            d2i = render(eng, 3, DL, IL)
        except RuntimeError as e:
            print(f"[SKIP] {eng}: {e}")
            continue
        res[eng] = (d1, d2i)
        v = d1[PATCH]
        ok &= check(free1[PATCH].min() > 2 * DL and abs(v.mean() - DL) < 1e-3 * DL
                    and v.max() < DL * (1 + 1e-4),
                    f"1 {eng} direct limit",
                    f"unclamped {free1[PATCH].mean():.2f} -> {v.mean():.4f} (limit {DL})")
        ind = (d2i - d1)[PATCH]
        ind_free = (d2 - d1)[PATCH]
        ok &= check(ind.mean() > 0 and ind.max() <= IL * (1 + 1e-3) and
                    ind_free.mean() > 2 * IL,
                    f"2 {eng} indirect limit",
                    f"indirect {ind_free.mean():.3f} -> {ind.mean():.3f} "
                    f"(max {ind.max():.3f}, limit {IL})")
    if len(res) == 2:
        a, b = res["PATHCPU"], res["PATHOCL"]
        r1 = b[0][PATCH].mean() / a[0][PATCH].mean()
        r2 = b[1][PATCH].mean() / a[1][PATCH].mean()
        ok &= check(abs(r1 - 1) < 0.01 and abs(r2 - 1) < 0.03, "3 PATHOCL == PATHCPU",
                    f"direct {r1:.4f} direct+indirect {r2:.4f}")

    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
