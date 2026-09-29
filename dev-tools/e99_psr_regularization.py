#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# D1 regression: path-space regularization (path.regularization.*).
#
# PSR inflates microfacet alpha at secondary vertices through
# PathDepthInfo -> HitPoint.regularization -> RegularizeAlpha()
# (microfacet.h / Microfacet_RegularizeAlpha in the CL twin). The
# test renders a roughglass plate lit by a small emitter above a
# diffuse floor with sigma=0 and sigma=0.06 and asserts:
#
#   1. both renders are finite
#   2. the regularized render differs measurably (the feature is
#      actually wired end-to-end, not a no-op)
#   3. sigma=0 output lands inside a loose expected band (regression
#      anchor for the untouched default path)
#
# Exit code 0 = pass, 1 = fail.

import sys, time, math
from array import array

sys.path.insert(0, "out/build/src/pysuperluxcore/Release")
sys.path.insert(0, "pyunittests")

import pysuperluxcore


def build_scene():
    scene = pysuperluxcore.Scene()
    # floor + ceiling emitter + roughglass plate hovering over the floor
    s = 5.0
    verts = [(-s, -s, -s), (s, -s, -s), (s, s, -s), (-s, s, -s),
             (-s, -s, s), (s, -s, s), (s, s, s), (-s, s, s)]
    tris = [(0, 1, 2), (0, 2, 3), (4, 6, 5), (4, 7, 6),
            (0, 4, 5), (0, 5, 1), (1, 5, 6), (1, 6, 2),
            (2, 6, 7), (2, 7, 3), (3, 7, 4), (3, 4, 0)]
    scene.DefineMesh("room", verts, tris, None, None, None, None)
    scene.DefineMesh("lamp",
                     [(-0.7, -0.7, 4.0), (0.7, -0.7, 4.0),
                      (0.7, 0.7, 4.0), (-0.7, 0.7, 4.0)],
                     [(0, 1, 2), (0, 2, 3)], None, None, None, None)
    scene.DefineMesh("plate",
                     [(-3, -3, 0.0), (3, -3, 0.0),
                      (3, 3, 0.0), (-3, 3, 0.0)],
                     [(0, 1, 2), (0, 2, 3)], None, None, None, None)
    scene.Parse(pysuperluxcore.Properties().SetFromString("""
        scene.camera.lookat.orig = 0 -3 3.5
        scene.camera.lookat.target = 0 0 -1
        scene.camera.fieldofview = 55
        scene.materials.matte.type = matte
        scene.materials.matte.kd = 0.6 0.6 0.6
        scene.materials.emit.type = matte
        scene.materials.emit.emission = 60 60 60
        scene.materials.emit.kd = 0 0 0
        scene.materials.plate.type = roughglass
        scene.materials.plate.kt = 0.95 0.95 0.95
        scene.materials.plate.kr = 0.95 0.95 0.95
        scene.materials.plate.uroughness = 0.03
        scene.materials.plate.vroughness = 0.03
        scene.materials.plate.interiorior = 1.5
        scene.objects.room.shape = room
        scene.objects.room.material = matte
        scene.objects.lamp.shape = lamp
        scene.objects.lamp.material = emit
        scene.objects.plate.shape = plate
        scene.objects.plate.material = plate
        """))
    return scene


def build_session(engine, sigma):
    cfg = pysuperluxcore.Properties().SetFromString(f"""
        renderengine.type = {engine}
        sampler.type = SOBOL
        film.width = 96
        film.height = 96
        path.regularization.sigma = {sigma}
        film.imagepipelines.0.type = TONEMAP_LINEAR
        film.imagepipelines.0.scale = 1
        film.outputs.0.type = RGB_IMAGEPIPELINE
        film.outputs.0.index = 0
        film.outputs.0.filename = e99-{engine}-{sigma}.png
        film.outputs.1.type = RGB
        film.outputs.1.filename = e99-{engine}-{sigma}.hdr
        batch.haltspp = 32
        batch.halttime = 0
        batch.haltthreshold = -1
        """)
    if engine == "PATHOCL":
        cfg.Set(pysuperluxcore.Property("opencl.gpu.use", "1"))
        cfg.Set(pysuperluxcore.Property("opencl.cpu.use", "0"))
    else:
        cfg.Set(pysuperluxcore.Property("opencl.gpu.use", "0"))
        cfg.Set(pysuperluxcore.Property("opencl.cpu.use", "1"))
    return pysuperluxcore.RenderSession(
        pysuperluxcore.RenderConfig(cfg, build_scene()))


def render_mean(engine, sigma):
    session = build_session(engine, sigma)
    try:
        session.Start()
        film = session.GetFilm()
        w, h = film.GetWidth(), film.GetHeight()
        deadline = time.time() + 240.0
        while time.time() < deadline:
            session.UpdateStats()
            if session.GetStats().Get(
                    "stats.renderengine.pass").GetInt() >= 32:
                break
            time.sleep(0.25)
        data = array('f', bytes(w * h * 4 * 3))
        film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB,
                            data, 0, False)
        vals = list(data)
        assert all(math.isfinite(v) for v in vals), \
            f"{engine} sigma={sigma}: non-finite pixel"
        return sum(vals) / len(vals)
    finally:
        session.Stop()


def run(engine):
    off = render_mean(engine, 0.0)
    on = render_mean(engine, 0.06)
    rel = abs(on - off) / max(off, 1e-9)
    print(f"[{engine}] sigma=0 mean={off:.5f}  sigma=0.06 mean={on:.5f}  "
          f"rel={rel:.4f}")

    # Feature-active check: the blur must measurably change the image
    assert rel > 0.005, \
        f"{engine}: regularization had no measurable effect (rel={rel})"
    # Regression anchor on the untouched default path
    assert 0.01 < off < 1.0, \
        f"{engine}: sigma=0 mean out of expected band ({off})"
    print(f"[{engine}] PASS")
    return True


def main():
    ok = True
    ok &= run("PATHCPU")
    try:
        ok &= run("PATHOCL")
    except Exception as e:
        print(f"[PATHOCL] SKIP/FAIL: {e}")
        ok = False
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
