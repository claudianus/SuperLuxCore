# SPDX-License-Identifier: Apache-2.0
#
# E109: PhotonGI caustic cache + hybrid back/forward must not delete the
# depth-0 caustics the adaptive partition leaves to the eye path.
#
# A white room lit by a downward ceiling panel; a box with a polished
# metal2 top (roughness 0 -> GLOSSY event, glossiness ~0) reflects the
# panel onto the ceiling (PSR off: Cycles Filter Glossy 0 keeps the lobe
# near-delta). Eye path: camera -> ceiling (D) -> metal (G) ->
# panel. Under hybrid the caustic cache is not queried at depth 0 (the
# light pass owns that receiver), and the adaptive partition keeps this
# class on the eye side (a near-zero glossiness lobe always connects).
# PhotonGICache::IsDirectLightHitVisible() used to cut the light hit
# anyway (diffuse depth > 0, nearly-specular last bounce) - nothing
# estimated the caustic and the ceiling patch rendered at ~0.6x (Cycles
# 003 Cornell box, mirror caustic on the ceiling).
#
#   1  PATHCPU hybrid + caustic cache matches plain PT on the ceiling
#   2  PATHOCL hybrid + caustic cache matches plain PT on the ceiling
#   3  PATHCPU caustic cache alone (no hybrid) stays within the cache's
#      density-estimation bias of plain PT
#
# Run from the repo root:
#   python3.13 dev-tools/e109_pgic_hybrid_caustic_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 96, 64
OCL_DEV = os.environ.get("E109_OCL_DEV", "")


def box(name, x0, x1, y0, y1, z0, z1, mat, inward=False):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    if inward:
        f = [(a, c, b) for a, b, c in f]
    return (f"scene.objects.{name}.material = {mat}\n"
            f"scene.objects.{name}.vertices = " +
            " ".join("%g %g %g" % p for p in v) + "\n" +
            f"scene.objects.{name}.faces = " +
            " ".join("%d %d %d" % t for t in f) + "\n")


SCENE = """
scene.camera.lookat.orig = 0 -3.5 1
scene.camera.lookat.target = 0 0 1.6
scene.camera.up = 0 0 1
scene.camera.fieldofview = 60
scene.materials.white.type = matte
scene.materials.white.kd = 0.7 0.7 0.7
scene.materials.light.type = matte
scene.materials.light.kd = 0 0 0
scene.materials.light.emission = 10 10 10
scene.textures.fr.type = fresnelcolor
scene.textures.fr.kr = 0.9 0.9 0.9
scene.materials.mir.type = metal2
scene.materials.mir.fresnel = fr
scene.materials.mir.uroughness = 0
scene.materials.mir.vroughness = 0
scene.materials.mir.distribution = ggx
scene.objects.lamp.material = light
scene.objects.lamp.vertices = -0.3 -0.3 1.99  0.3 -0.3 1.99  0.3 0.3 1.99  -0.3 0.3 1.99
scene.objects.lamp.faces = 0 2 1  0 3 2
""" + box("room", -2, 2, -4, 2, 0, 2, "white", inward=True) + \
    box("mbox", 0.4, 1.2, -0.4, 0.4, 0, 0.8, "mir")

PLAIN = """
path.hybridbackforward.enable = 0
path.lighttracing.enable = 0
"""
HYBRID_PGIC = """
path.hybridbackforward.enable = 1
path.hybridbackforward.adaptivecaustic = 1
path.photongi.caustic.enabled = 1
path.photongi.caustic.updatespp = 0
"""
PGIC_ONLY = PLAIN + """
path.photongi.caustic.enabled = 1
path.photongi.caustic.updatespp = 0
"""

# ceiling patch the metal top lights (1.3-1.7x the white-box radiance)
CEIL = np.s_[16:32, 64:96]


def render(extra, engine, spp):
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(SCENE)
    scn.Parse(p)
    cfg = pysuperluxcore.Properties()
    dev = f'opencl.devices.select = "{OCL_DEV}"\n' if OCL_DEV else ""
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
path.pathdepth.total = 8
path.pathdepth.diffuse = 8
path.pathdepth.glossy = 8
path.pathdepth.specular = 8
path.regularization.auto = 0
opencl.task.count = 65536
opencl.native.threads.count = 0
{dev}{extra}
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
        time.sleep(0.3)
    ses.Stop()
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    return rgb.reshape(H, W, 3).mean(axis=2)[::-1]


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    pysuperluxcore.Init(lambda msg: None)
    ok = True
    ref = render(PLAIN, "PATHCPU", 256)[CEIL].mean()

    r = render(HYBRID_PGIC, "PATHCPU", 256)[CEIL].mean() / ref
    ok &= check(abs(r - 1) < 0.05, "1 PATHCPU hybrid+pgic ceiling caustic",
                f"ratio={r:.3f} (was ~0.6)")

    try:
        r = render(HYBRID_PGIC, "PATHOCL", 256)[CEIL].mean() / ref
        ok &= check(abs(r - 1) < 0.06, "2 PATHOCL hybrid+pgic ceiling caustic",
                    f"ratio={r:.3f}")
    except RuntimeError as e:
        print(f"[SKIP] 2 PATHOCL: {e}")

    r = render(PGIC_ONLY, "PATHCPU", 256)[CEIL].mean() / ref
    ok &= check(abs(r - 1) < 0.08, "3 PATHCPU pgic-only ceiling caustic",
                f"ratio={r:.3f}")

    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
