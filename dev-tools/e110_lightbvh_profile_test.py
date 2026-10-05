# SPDX-License-Identifier: Apache-2.0
#
# E110: light BVH importance of profile emitters (emission.spot / spread
# maps) and of compact clusters outside a large cluster box.
#
# A white floor sits inside the bounding box of two wall panels (large
# area lights standing on the floor at x = +-1.9); a small sized-spot
# disk (radius 1 cm, 17 deg cone, the BlendLuxCore export of a Cycles
# spot with a radius) lights the floor centre from outside that box.
#
# The importance used to take the bare point-to-box distance (0 inside
# a box -> 1/minDist2 ~ 1e6) and judge the spot by its total power over
# a hemisphere: a floor point inside the (spot + right panel) box picked
# that subtree ~always and the left panel's pick pdf fell to ~4e-7 (in
# the 083 perfume studio the spot starved the same way: every BSDF hit
# of the disk carried MIS weight ~1 - 1e5 fireflies). NEE then never
# sampled the starved light and its whole contribution came from BSDF
# hits.
#
#   1  LIGHT_BVH mean matches LOG_POWER (unbiased)
#   2  LIGHT_BVH error in the spot pool is well below LOG_POWER's
#      (the starved tree was only ~0.6x of it, now ~0.15x)
#   3  PATHOCL LIGHT_BVH matches PATHCPU
#
# Run from the repo root:
#   python3.13 dev-tools/e110_lightbvh_profile_test.py

import math
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 128, 128


def disk(name, center, normal, radius, segs=16):
    n = np.asarray(normal, float); n /= np.linalg.norm(n)
    t = np.cross(n, [0, 0, 1] if abs(n[2]) < 0.9 else [1, 0, 0]); t /= np.linalg.norm(t)
    b = np.cross(n, t)
    c = np.asarray(center, float)
    v = [c + radius * (math.cos(2 * math.pi * k / segs) * t +
                       math.sin(2 * math.pi * k / segs) * b) for k in range(segs)]
    v.append(c)
    # wind so the geometric normal is +n (ring CCW around n)
    f = [(segs, k, (k + 1) % segs) for k in range(segs)]
    return (f"scene.objects.{name}.material = {name}\n"
            f"scene.objects.{name}.vertices = " +
            " ".join("%g %g %g" % tuple(p) for p in v) + "\n" +
            f"scene.objects.{name}.faces = " +
            " ".join("%d %d %d" % tri for tri in f) + "\n")


def quad(name, mat, p0, p1, p2, p3):
    return (f"scene.objects.{name}.material = {mat}\n"
            f"scene.objects.{name}.vertices = " +
            " ".join("%g %g %g" % p for p in (p0, p1, p2, p3)) + "\n" +
            f"scene.objects.{name}.faces = 0 1 2  0 2 3\n")


SPOT_POS = (0.0, -1.5, 1.0)
SCENE = f"""
scene.camera.lookat.orig = 0 -0.01 3
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 40
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.6 0.6 0.6
scene.materials.panel.type = matte
scene.materials.panel.kd = 0 0 0
scene.materials.panel.emission = 0.5 0.5 0.5
scene.materials.spot.type = matte
scene.materials.spot.kd = 0 0 0
scene.materials.spot.emission = 1 1 1
scene.materials.spot.emission.gain = 20000 20000 20000
scene.materials.spot.emission.spot.angle = 0.3
scene.materials.spot.emission.spot.blend = 0.1
scene.materials.spot.transparency.shadow = 1 1 1
""" + quad("floor", "floor", (-3, -3, 0), (3, -3, 0), (3, 3, 0), (-3, 3, 0)) + \
    quad("pl", "panel", (-1.9, -0.5, 0), (-1.9, 0.5, 0), (-1.9, 0.5, 1), (-1.9, -0.5, 1)) + \
    quad("pr", "panel", (1.9, 0.5, 0), (1.9, -0.5, 0), (1.9, -0.5, 1), (1.9, 0.5, 1)) + \
    disk("spot", SPOT_POS, tuple(-np.asarray(SPOT_POS)), 0.01)

# spot pool on the floor (centre of the frame)
POOL = np.s_[44:84, 44:84]


def render(strategy, engine, spp, seed=17):
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
renderengine.seed = {seed}
sampler.type = SOBOL
batch.haltspp = {spp}
lightstrategy.type = {strategy}
path.pathdepth.total = 3
path.pathdepth.diffuse = 3
path.hybridbackforward.enable = 0
path.lighttracing.enable = 0
path.mnee.enable = 0
path.regularization.auto = 0
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
    return rgb.reshape(H, W, 3).mean(axis=2)


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    pysuperluxcore.Init()
    ok = True
    ref = render("LOG_POWER", "PATHCPU", 1024, seed=3)
    flat = render("LOG_POWER", "PATHCPU", 32)
    bvh = render("LIGHT_BVH", "PATHCPU", 32)
    pool = ref[POOL].mean()
    print(f"spot pool radiance {pool:.4f} (frame mean {ref.mean():.4f})")

    r = bvh.mean() / ref.mean()
    ok &= check(abs(r - 1) < 0.03, "1 LIGHT_BVH unbiased", f"mean ratio={r:.4f}")

    e_flat = np.sqrt(((flat[POOL] - ref[POOL]) ** 2).mean()) / pool
    e_bvh = np.sqrt(((bvh[POOL] - ref[POOL]) ** 2).mean()) / pool
    ok &= check(e_bvh <= 0.35 * e_flat, "2 LIGHT_BVH spot-pool error",
                f"rel RMSE bvh={e_bvh:.3f} flat={e_flat:.3f}")

    try:
        gpu = render("LIGHT_BVH", "PATHOCL", 32)
        r = gpu.mean() / bvh.mean()
        e_gpu = np.sqrt(((gpu[POOL] - ref[POOL]) ** 2).mean()) / pool
        ok &= check(abs(r - 1) < 0.03 and e_gpu <= 0.35 * e_flat,
                    "3 PATHOCL LIGHT_BVH", f"mean ratio={r:.4f} rel RMSE={e_gpu:.3f}")
    except RuntimeError as e:
        print(f"[SKIP] 3 PATHOCL: {e}")

    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
