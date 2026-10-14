# SPDX-License-Identifier: Apache-2.0
#
# E116: zero-config caustics through several glass interfaces. A matte
# floor under a stack of thin glass slabs, lit by an area light above
# (light -> 2N interfaces -> floor -> 2N interfaces -> eye). With
# caustic.auto on, the eye side cuts the light hit the cache replaces,
# so every photon chain the photon tracer drops renders dark: the
# photon depth (path.photongi.photon.maxdepth, core default 4 = three
# delta interfaces before the landing vertex) must follow the auto
# cache, not the legacy indirect-cache default. The Blender add-on
# exports 16; Python/SDL users got 4.
#
#   1  one slab (2 interfaces): zero-config floor matches the reference
#   2  three slabs (6 interfaces): zero-config floor matches the reference
#   3  caustic.auto raises an unset photon depth, keeps an explicit one
#   4  PATHOCL three slabs: zero-config floor matches the reference
#
# Run from the repo root:
#   python3.13 dev-tools/e116_multi_interface_caustic_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 96, 96
OCL_DEV = os.environ.get("E116_OCL_DEV", "")


def box(name, x0, x1, y0, y1, z0, z1, mat):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    return (f"scene.objects.{name}.material = {mat}\n"
            f"scene.objects.{name}.vertices = " +
            " ".join("%g %g %g" % p for p in v) + "\n" +
            f"scene.objects.{name}.faces = " +
            " ".join("%d %d %d" % t for t in f) + "\n")


BASE = """
scene.camera.lookat.orig = 0 0 2.6
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 28
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.7 0.7 0.7
scene.materials.glass.type = glass
scene.materials.glass.kr = 1 1 1
scene.materials.glass.kt = 1 1 1
scene.materials.glass.interiorior = 1.5
scene.materials.light.type = matte
scene.materials.light.kd = 0 0 0
scene.materials.light.emission = 20 20 20
scene.objects.lamp.material = light
scene.objects.lamp.vertices = -0.7 -0.7 3.2  0.7 -0.7 3.2  0.7 0.7 3.2  -0.7 0.7 3.2
scene.objects.lamp.faces = 0 2 1  0 3 2
scene.objects.ground.material = floor
scene.objects.ground.vertices = -3 -3 0  3 -3 0  3 3 0  -3 3 0
scene.objects.ground.faces = 0 1 2  0 2 3
"""


def scene(slabs):
    s = BASE
    for i in range(slabs):
        z = 0.3 + 0.25 * i
        s += box(f"slab{i}", -0.9, 0.9, -0.9, 0.9, z, z + 0.05, "glass")
    return s


# Eye side deep enough for the reference to see the light through every
# slab; the photon depth is left to the engine (that is what is tested).
DEPTH = """
path.pathdepth.total = 32
path.pathdepth.diffuse = 8
path.pathdepth.glossy = 32
path.pathdepth.specular = 32
"""
REFERENCE = DEPTH + """
path.photongi.caustic.enabled = 0
path.lighttracing.auto = 0
path.mnee.auto = 0
path.regularization.auto = 0
"""
ZERO_CONFIG = DEPTH

# floor straight below the camera, under every slab
FLOOR = np.s_[30:66, 30:66]


def config(extra, engine, seconds):
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
    return cfg


def parse(slabs):
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(scene(slabs))
    scn.Parse(p)
    return scn


def resolved(slabs, extra):
    props = pysuperluxcore.RenderConfig(
        config(extra, "PATHCPU", 1), parse(slabs)).GetProperties()
    key = "path.photongi.photon.maxdepth"
    return props.Get(key).GetInt() if props.IsDefined(key) else None


def render(slabs, extra, engine, seconds):
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(
        config(extra, engine, seconds), parse(slabs)))
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
    return float(rgb.reshape(H, W, 3).mean(axis=2)[::-1][FLOOR].mean())


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    pysuperluxcore.Init(lambda s: None)
    ok = True
    refs = {}
    for slabs, tag in ((1, "1"), (3, "2")):
        refs[slabs] = render(slabs, REFERENCE, "PATHOCL", 90)
        got = render(slabs, ZERO_CONFIG, "PATHCPU", 40)
        ratio = got / refs[slabs]
        ok &= check(abs(ratio - 1) < 0.05,
                    f"{tag} PATHCPU {slabs} slab(s) zero-config floor",
                    f"ratio={ratio:.3f} (ref {refs[slabs]:.4f})")

    unset = resolved(3, ZERO_CONFIG)
    explicit = resolved(3, ZERO_CONFIG + "path.photongi.photon.maxdepth = 5\n")
    ok &= check((unset or 0) >= 16 and explicit == 5,
                "3 caustic.auto photon depth",
                f"unset->{unset}, explicit 5->{explicit}")

    got = render(3, ZERO_CONFIG, "PATHOCL", 40)
    ratio = got / refs[3]
    ok &= check(abs(ratio - 1) < 0.05, "4 PATHOCL 3 slabs zero-config floor",
                f"ratio={ratio:.3f}")

    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
