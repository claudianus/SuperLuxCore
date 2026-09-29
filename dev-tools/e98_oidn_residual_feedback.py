#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# E1' regression: OIDN denoiser residual -> NOISE channel feedback.
#
# FilmImplSession::ApplyOIDN() now diffs the image-pipeline channel
# before/after denoising and merges the per-pixel residual into the film
# NOISE channel (max-combine). Adaptive samplers consume that map, so
# pixels the denoiser could not clean keep receiving samples.
#
# This test:
#   1. renders a few passes of a noisy interior scene with
#      film.adaptiveerror.target set (allocates the NOISE channel)
#   2. reads the NOISE output before and after ApplyOIDN(0)
#   3. requires the post-OIDN map to be finite and to have moved (the
#      residual is real signal, not an untouched buffer)
#
# Exit code 0 = pass, 1 = fail.

import sys, time
from array import array

sys.path.insert(0, "out/build/src/pysuperluxcore/Release")
sys.path.insert(0, "pyunittests")

import pysuperluxcore


def build_session(engine):
    scene = pysuperluxcore.Scene()
    s = 5.0
    verts = [(-s, -s, -s), (s, -s, -s), (s, s, -s), (-s, s, -s),
             (-s, -s, s), (s, -s, s), (s, s, s), (-s, s, s)]
    tris = [(0, 1, 2), (0, 2, 3), (4, 6, 5), (4, 7, 6),
            (0, 4, 5), (0, 5, 1), (1, 5, 6), (1, 6, 2),
            (2, 6, 7), (2, 7, 3), (3, 7, 4), (3, 4, 0)]
    scene.DefineMesh("room", verts, tris, None, None, None, None)
    scene.DefineMesh("lamp",
                     [(-1, -1, 2), (1, -1, 2), (1, 1, 2), (-1, 1, 2)],
                     [(0, 1, 2), (0, 2, 3)], None, None, None, None)
    scene.Parse(pysuperluxcore.Properties().SetFromString("""
        scene.camera.lookat.orig = 0 0 -2
        scene.camera.lookat.target = 0.2 0.3 1
        scene.camera.fieldofview = 60
        scene.materials.matte.type = matte
        scene.materials.matte.kd = 0.7 0.7 0.7
        scene.materials.emit.type = matte
        scene.materials.emit.emission = 8 8 8
        scene.objects.room.shape = room
        scene.objects.room.material = matte
        scene.objects.lamp.shape = lamp
        scene.objects.lamp.material = emit
        """))

    cfg = pysuperluxcore.Properties().SetFromString(f"""
        renderengine.type = {engine}
        sampler.type = SOBOL
        sampler.sobol.adaptive.strength = 0.7
        film.width = 96
        film.height = 64
        film.adaptiveerror.target = 0.05
        film.adaptiveerror.halt.enable = 0
        film.imagepipelines.0.type = NOP
        film.imagepipelines.1.type = TONEMAP_LINEAR
        film.imagepipelines.1.scale = 1
        film.outputs.0.type = RGB_IMAGEPIPELINE
        film.outputs.0.index = 0
        film.outputs.0.filename = e98.png
        film.outputs.1.type = NOISE
        film.outputs.1.filename = e98-noise.hdr
        batch.haltspp = 0
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
        pysuperluxcore.RenderConfig(cfg, scene))


def read_noise(film, w, h):
    data = array('f', bytes(w * h * 4))
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.NOISE,
                        data, 0, False)
    return data


def run(engine):
    session = build_session(engine)
    try:
        session.Start()
        film = session.GetFilm()
        w, h = film.GetWidth(), film.GetHeight()

        # Phase A - write path: while the adaptive error test is still in
        # warmup the NOISE map is all-infinite; ApplyOIDN must replace it
        # with finite residuals everywhere (the denoise diff IS signal).
        deadline = time.time() + 30.0
        while time.time() < deadline:
            session.UpdateStats()
            if session.GetStats().Get(
                    "stats.renderengine.pass").GetInt() >= 2:
                break
            time.sleep(0.1)
        film.ApplyOIDN(0)
        noise = read_noise(film, w, h)
        n_finite = sum(1 for v in noise if v == v and v < float("inf"))
        assert n_finite > w * h * 0.9, \
            f"{engine}: residual wrote only {n_finite}/{w*h} finite pixels"
        print(f"[{engine}] phase A: residual populated "
              f"{n_finite}/{w*h} pixels")

        film = session.GetFilm()
        w, h = film.GetWidth(), film.GetHeight()

        # Phase B - max-merge: with a partially relaxed statistical map
        # the residual can only raise entries. Re-denoise and count
        # pixels lifted above their pre-denoise value.
        deadline = time.time() + 120.0
        while time.time() < deadline:
            session.UpdateStats()
            noise = read_noise(film, w, h)
            finite = [v for v in noise if v == v and v < float("inf")]
            if sum(1 for v in finite if v < 0.999) > len(finite) * 0.3:
                break
            time.sleep(0.5)

        before = read_noise(film, w, h)
        film.ApplyOIDN(0)
        after = read_noise(film, w, h)
        import math
        print(f"[{engine}] before: {sum(1 for v in before if math.isfinite(v))} finite, "
              f"sample={list(before[:5])}")
        print(f"[{engine}] after : {sum(1 for v in after if math.isfinite(v))} finite, "
              f"sample={list(after[:5])}")

        finite = all(v == v and abs(v) != float("inf") for v in after)
        assert finite, f"{engine}: NOISE map has NaN/Inf after ApplyOIDN"

        # The residual merge is a max()-lift: count pixels RAISED by the
        # denoise diff (concurrent adaptive-test refreshes only lower or
        # rewrite values, never produce this pattern deterministically).
        lifted = sum(1 for a, b in zip(before, after) if b > a + 1e-4)
        assert lifted >= 1, \
            f"{engine}: residual feedback lifted only {lifted} pixels"

        print(f"[{engine}] noise map updated by residual: "
              f"{lifted}/{w*h} pixels lifted, "
              f"max={max(after):.3f} mean={sum(after)/len(after):.4f} PASS")
        return True
    finally:
        session.Stop()


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
