# SPDX-License-Identifier: Apache-2.0
#
# E115: zero-config SDS caustics - a pool floor seen through the water
# surface (light -> water -> floor -> water -> eye) must render at the
# brightness of a converged, depth-unlimited path tracer with no caustic
# settings at all.
#
# A matte pool filled with a glass water box, lit by an area light the
# plain path tracer can still hit through the water (so a no-cache
# reference converges inside a test budget). The camera looks down
# through the surface: refraction pulls the near floor below the
# straight film frustum. Regressions this guards:
#
#   - path.photongi.caustic.auto must enable the cache with no config
#     (the light pass can not splat S->E, NEE can not connect S->light)
#   - caustic deposits must not be coverage-gated: the frustum test
#     (and the visibility-map gate) dropped the refracted near floor
#     while the eye side still cut its light hit (floor at 0.37x)
#   - GPU light paths must not take the radiance eta^2 at refraction
#     (B1' deposits ending in water came out 1/1.33^2 = 0.565x)
#   - GPU ingest generations must keep refreshing once maxSize is
#     reached (sliding window, exact traced-count normalization)
#   - an opaque Disney floor (what the Blender add-on exports for every
#     Principled BSDF) must receive caustic photons: Disney reported
#     TRANSMIT unconditionally and was never a cache receiver - and
#     once it was, its GLOSSY-only events kept the eye side from
#     cutting the light hit the cache replaces (floor 1.8x)
#
#   1  PATHCPU zero-config: cache auto-enabled, floor matches reference
#   2  PATHOCL zero-config: cache auto-enabled, floor matches reference
#   3  PATHOCL vs PATHCPU floor parity
#   4  Disney floor: the cache receives photons (showcaustic debug view)
#   5  Disney floor: PATHCPU and PATHOCL zero-config match the reference
#
# Run from the repo root:
#   python3.13 dev-tools/e115_pool_sds_caustic_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 128, 96
OCL_DEV = os.environ.get("E115_OCL_DEV", "")


def box(name, x0, x1, y0, y1, z0, z1, mat, inward=False, open_top=False):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    if open_top:
        f = f[:2] + f[4:]
    if inward:
        f = [(a, c, b) for a, b, c in f]
    return (f"scene.objects.{name}.material = {mat}\n"
            f"scene.objects.{name}.vertices = " +
            " ".join("%g %g %g" % p for p in v) + "\n" +
            f"scene.objects.{name}.faces = " +
            " ".join("%d %d %d" % t for t in f) + "\n")


SCENE = """
scene.camera.lookat.orig = 0 -1.7 2.4
scene.camera.lookat.target = 0 0.15 0
scene.camera.up = 0 0 1
scene.camera.fieldofview = 50
scene.materials.tile.type = matte
scene.materials.tile.kd = 0.7 0.7 0.7
scene.materials.water.type = glass
scene.materials.water.kr = 1 1 1
scene.materials.water.kt = 1 1 1
scene.materials.water.interiorior = 1.33
scene.materials.light.type = matte
scene.materials.light.kd = 0 0 0
scene.materials.light.emission = 30 30 30
scene.objects.lamp.material = light
scene.objects.lamp.vertices = -0.6 -0.2 4  0.6 -0.2 4  0.6 1.0 4  -0.6 1.0 4
scene.objects.lamp.faces = 0 2 1  0 3 2
""" + box("pool", -1, 1, -1, 1, 0, 1, "tile", inward=True, open_top=True) + \
    box("waterbox", -0.999, 0.999, -0.999, 0.999, 0.001, 0.8, "water")

# deep limits on both sides: the default diffuse depth truncates the
# pool's inter-reflection, and the cache (which skips the water -> light
# hop) would then read brighter than a truncated reference
DEPTH = """
path.pathdepth.total = 32
path.pathdepth.diffuse = 32
path.pathdepth.glossy = 32
path.pathdepth.specular = 32
path.photongi.photon.maxdepth = 32
"""
REFERENCE = DEPTH + """
path.photongi.caustic.enabled = 0
path.lighttracing.auto = 0
path.mnee.auto = 0
path.regularization.auto = 0
"""
ZERO_CONFIG = DEPTH

# Principled-style opaque floor (add-on export: transmission 0)
DISNEY = """
scene.materials.tile.type = disney
scene.materials.tile.basecolor = 0.7 0.7 0.7
scene.materials.tile.roughness = 0.35
scene.materials.tile.specular = 0.5
scene.materials.tile.transmission = 0
"""

# floor seen through the water (lower image band, refraction-pulled)
FLOOR = np.s_[18:64, 32:96]


def render(extra, engine, seconds, scene_extra=""):
    """Returns (luminance image, resolved path.photongi.caustic.enabled)"""
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(SCENE + scene_extra)
    scn.Parse(p)
    cfg = pysuperluxcore.Properties()
    dev = f'opencl.devices.select = "{OCL_DEV}"\n' if OCL_DEV else ""
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
renderengine.seed = 7
batch.halttime = {seconds}
opencl.native.threads.count = 0
{dev}{extra}
""")
    rc = pysuperluxcore.RenderConfig(cfg, scn)
    props = rc.GetProperties()
    cache_on = (props.IsDefined("path.photongi.caustic.enabled") and
                props.Get("path.photongi.caustic.enabled").GetBool())
    ses = pysuperluxcore.RenderSession(rc)
    ses.Start()
    deadline = time.monotonic() + seconds + 300
    while not ses.HasDone():
        if time.monotonic() > deadline:
            raise TimeoutError(engine)
        time.sleep(0.5)
        ses.UpdateStats()
    ses.Stop()
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    return rgb.reshape(H, W, 3).mean(axis=2)[::-1], cache_on


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    pysuperluxcore.Init()
    ok = True
    img, _ = render(REFERENCE, "PATHOCL", 240)
    ref = img[FLOOR].mean()

    img, cache_on = render(ZERO_CONFIG, "PATHCPU", 90)
    cpu = img[FLOOR].mean()
    ok &= check(cache_on, "1a PATHCPU caustic cache auto-enabled",
                f"caustic.enabled={cache_on}")
    r = cpu / ref
    ok &= check(abs(r - 1) < 0.05, "1b PATHCPU zero-config pool floor",
                f"ratio={r:.3f}")

    img, cache_on = render(ZERO_CONFIG, "PATHOCL", 90)
    gpu = img[FLOOR].mean()
    ok &= check(cache_on, "2a PATHOCL caustic cache auto-enabled",
                f"caustic.enabled={cache_on}")
    r = gpu / ref
    ok &= check(abs(r - 1) < 0.05, "2b PATHOCL zero-config pool floor",
                f"ratio={r:.3f}")

    r = gpu / cpu
    ok &= check(abs(r - 1) < 0.04, "3 PATHOCL/PATHCPU pool floor parity",
                f"ratio={r:.3f}")

    # the debug view stops at the first hit: hide the water from the
    # camera (photons still refract through it) so the floor is that hit
    img, _ = render(ZERO_CONFIG + "path.photongi.debug.type = showcaustic\n",
                    "PATHCPU", 20,
                    DISNEY + "scene.objects.waterbox.camerainvisible = 1\n")
    m = img[FLOOR].mean()
    ok &= check(m > 0.02 * ref, "4 Disney floor receives caustic photons",
                f"showcaustic floor={m:.4f}")

    img, _ = render(REFERENCE, "PATHOCL", 240, DISNEY)
    dref = img[FLOOR].mean()
    for engine in ("PATHCPU", "PATHOCL"):
        img, _ = render(ZERO_CONFIG, engine, 90, DISNEY)
        r = img[FLOOR].mean() / dref
        ok &= check(abs(r - 1) < 0.05, f"5 {engine} zero-config Disney floor",
                    f"ratio={r:.3f}")

    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
