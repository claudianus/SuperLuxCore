# SPDX-License-Identifier: Apache-2.0
#
# E113: two-sided triangle emission (scene.materials.*.emission.twosided).
#
# Cycles' emission closure is two-sided (|N.w|): a mesh emitter lights
# and shows the same radiance on both faces. A horizontal emissive quad
# facing +Z hangs halfway between a white floor and ceiling (camera at
# the side, direct light only):
#
#   1  one-sided default: the floor (behind the quad) stays black
#   2  two-sided: floor == ceiling, ceiling == the one-sided ceiling
#   3  two-sided LIGHTCPU (TriangleLight::Emit) matches PATHCPU
#   4  PATHOCL matches PATHCPU (two-sided)
#   5  the camera sees the same radiance on both faces
#
# Run from the repo root:
#   python3.13 dev-tools/e113_twosided_emission_test.py

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 96, 96


def scene(twosided, look_at_quad=False):
    cam = ("scene.camera.lookat.orig = 0 -1.5 1.3\nscene.camera.lookat.target = 0 0 1\n"
           if look_at_quad else
           "scene.camera.lookat.orig = 2.8 0 1\nscene.camera.lookat.target = 0 0 1\n")
    return f"""
{cam}scene.camera.up = 0 0 1
scene.camera.fieldofview = 70
scene.materials.white.type = matte
scene.materials.white.kd = 0.8 0.8 0.8
scene.materials.em.type = matte
scene.materials.em.kd = 0 0 0
scene.materials.em.emission = 10 10 10
scene.materials.em.emission.twosided = {1 if twosided else 0}
scene.objects.floor.material = white
scene.objects.floor.vertices = -3 -3 0  3 -3 0  3 3 0  -3 3 0
scene.objects.floor.faces = 0 1 2  0 2 3
scene.objects.ceil.material = white
scene.objects.ceil.vertices = -3 -3 2  3 -3 2  3 3 2  -3 3 2
scene.objects.ceil.faces = 0 2 1  0 3 2
scene.objects.em.material = em
scene.objects.em.vertices = -0.25 -0.25 1  0.25 -0.25 1  0.25 0.25 1  -0.25 0.25 1
scene.objects.em.faces = 0 1 2  0 2 3
"""


def render(props, engine, spp=128, secs=None):
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(props)
    scn.Parse(p)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp if secs is None else 0}
path.pathdepth.total = 1
path.pathdepth.diffuse = 1
light.maxdepth = 1
path.hybridbackforward.enable = 0
path.lighttracing.enable = 0
opencl.native.threads.count = 0
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scn))
    ses.Start()
    t0 = time.monotonic()
    while True:
        ses.UpdateStats()
        if secs is not None:
            if time.monotonic() - t0 > secs:
                break
        elif ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() - t0 > 600:
            raise TimeoutError(engine)
        time.sleep(0.2)
    ses.Stop()
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    return rgb.reshape(H, W, 3).mean(axis=2)[::-1]


# side view: ceiling = top rows, floor = bottom rows
CEIL = np.s_[2:20, 20:76]
FLOOR = np.s_[76:94, 20:76]


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    pysuperluxcore.Init()
    ok = True
    one = render(scene(False), "PATHCPU")
    two = render(scene(True), "PATHCPU")
    ok &= check(one[FLOOR].mean() < 1e-4 and one[CEIL].mean() > 0.01,
                "1 one-sided default", f"ceiling {one[CEIL].mean():.4f} floor {one[FLOOR].mean():.5f}")
    r_fc = two[FLOOR].mean() / two[CEIL].mean()
    r_cc = two[CEIL].mean() / one[CEIL].mean()
    ok &= check(abs(r_fc - 1) < 0.02 and abs(r_cc - 1) < 0.02, "2 two-sided faces",
                f"floor/ceiling {r_fc:.4f}, ceiling vs one-sided {r_cc:.4f}")
    lt = render(scene(True), "LIGHTCPU", secs=20)
    r = (lt[FLOOR].mean() + lt[CEIL].mean()) / (two[FLOOR].mean() + two[CEIL].mean())
    r2 = lt[FLOOR].mean() / max(lt[CEIL].mean(), 1e-9)
    ok &= check(abs(r - 1) < 0.03 and abs(r2 - 1) < 0.05, "3 LIGHTCPU == PATHCPU",
                f"ratio {r:.4f}, floor/ceiling {r2:.4f}")
    try:
        gpu = render(scene(True), "PATHOCL")
        r = gpu.mean() / two.mean()
        r2 = gpu[FLOOR].mean() / gpu[CEIL].mean()
        ok &= check(abs(r - 1) < 0.02 and abs(r2 - 1) < 0.02, "4 PATHOCL == PATHCPU",
                    f"ratio {r:.4f}, floor/ceiling {r2:.4f}")
    except RuntimeError as e:
        print(f"[SKIP] 4 PATHOCL: {e}")
    # camera below the quad plane (z 1.3 > 1? no: above) sees the top face;
    # a second view from below sees the back face
    top = render(scene(True, True), "PATHCPU", spp=16)
    below = render(scene(True, True).replace("lookat.orig = 0 -1.5 1.3",
                                             "lookat.orig = 0 -1.5 0.7"), "PATHCPU", spp=16)
    a, b = top.max(), below.max()
    ok &= check(abs(a / b - 1) < 0.01, "5 both faces visible", f"top {a:.3f} back {b:.3f}")

    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
