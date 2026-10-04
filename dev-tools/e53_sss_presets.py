# SPDX-License-Identifier: Apache-2.0
#
# E53: OpenPBR measured-media subsurface presets
# (scene.materials.*.subsurfacepreset).
#
# A preset fills the albedo-parametrized SSS defaults (color / radius /
# radiusscale) from the measured-media table (Jensen'01, converted by
# dev-tools/gen_sss_presets.py); explicitly defined properties always
# win. Verified:
#   1. scene-level: preset produces the exact implicit-volume params of
#      the explicit equivalent, incl. the preset-only case with no
#      explicit subsurfaceradius (wantSSSVol path),
#   2. explicit subsurfacecolor/radius override preset defaults,
#   3. unknown preset names fail at parse time,
#   4. all presets produce sane volumes (0<albedo<1, mfp>0),
#   5. render sanity + CPU/GPU parity on a preset render.
#
# Run from the repo root:
#   python3.13 dev-tools/e53_sss_presets.py
#
# Env: SUPERLUXCORE_BACKENDS=cpu,opencl,metal (leg subset, default all);
#      E53_OCL_DEV overrides the PATHOCL opencl.devices.select mask.

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = 256
RENDER_TIMEOUT_S = 900
OCL_DEV = os.environ.get("E53_OCL_DEV", "")

BACKENDS = {b.strip() for b in
        os.environ.get("SUPERLUXCORE_BACKENDS", "cpu,opencl,metal").split(",")
        if b.strip()}

# Engine-side table values for skin_dark (gen_sss_presets.py output)
SKIN_DARK = {"albedo": (0.468349, 0.288709, 0.191387),
             "scale": (1.0, 0.735238, 0.518121), "radius": 0.00129534}

ALL_PRESETS = ("skin_dark", "skin_light", "chicken", "apple", "potato",
               "marble", "cream", "whole_milk", "skim_milk", "ketchup")

CAMERA = """
scene.camera.lookat.orig = -2.78 1.6 3.28
scene.camera.lookat.target = -2.78 2.76 3.28
scene.camera.fieldofview = 45
"""

FURNACE = """
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1.0 1.0 1.0
scene.lights.env.gain = 1.0 1.0 1.0
"""

OBJ = """
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
"""


def parse(props_str):
    props = pysuperluxcore.Properties()
    props.SetFromString(props_str)
    os.chdir(str(REPO))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return scene


def sss_mat(extra_props=""):
    return f"""{CAMERA}
scene.materials.ball.type = openpbr
scene.materials.ball.basecolor = 0.5 0.5 0.5
scene.materials.ball.specularior = 1.4
scene.materials.ball.specularroughness = 0.4
scene.materials.ball.subsurfaceweight = 1.0
{extra_props}
{OBJ}
{FURNACE}"""


def vec3(props, key):
    p = props.Get(key)
    assert p, f"missing property {key}"
    vals = " ".join(p.GetString(i) for i in range(p.GetSize())).split()
    return tuple(float(v) for v in vals)


def implicit_sss(scene):
    """Extract (albedo, per-channel mfp) of the material's implicit
    interior SSS volume from the serialized scene."""
    tp = scene.ToProperties()
    vol_name = tp.Get("scene.materials.ball.volume.interior").GetString(0)
    albedo = vec3(tp, f"scene.volumes.{vol_name}.sssalbedo")
    assert tp.Get(f"scene.volumes.{vol_name}.sssprofile").GetString(0) == "cb15"
    mfp_ref = tp.Get(f"scene.volumes.{vol_name}.sssmfp").GetString(0)
    try:
        # literal float list
        mfp = tuple(float(v) for v in mfp_ref.split())
    except ValueError:
        # texture reference: scale = texture1 (radius) x texture2 (scale),
        # constfloatN = .value
        ttype = tp.Get(f"scene.textures.{mfp_ref}.type").GetString(0)
        if ttype == "scale":
            t1 = vec3(tp, f"scene.textures.{mfp_ref}.texture1")
            t2 = vec3(tp, f"scene.textures.{mfp_ref}.texture2")
            t1 += (t1[0],) * (3 - len(t1))
            t2 += (t2[0],) * (3 - len(t2))
            mfp = tuple(a * b for a, b in zip(t1, t2))
        else:
            mfp = vec3(tp, f"scene.textures.{mfp_ref}.value")
    if len(mfp) == 1:
        mfp *= 3
    return albedo, mfp


def expect_sss(name, sss, albedo, mfp, tol=1e-4):
    a, d = sss
    ea = np.asarray(albedo, dtype=float)
    ed = np.asarray(mfp, dtype=float)
    print(f"{name}: albedo {a} mfp {d}")
    assert np.allclose(a, ea, rtol=tol, atol=tol), \
        f"{name}: albedo {a} != {albedo}"
    assert np.allclose(d, ed, rtol=tol, atol=tol), \
        f"{name}: mfp {d} != {mfp}"


def main():
    # ---- scene-level checks (exact, no MC noise) --------------------
    skin = [f"scene.materials.ball.subsurfacecolor = {' '.join(map(str, SKIN_DARK['albedo']))}",
            f"scene.materials.ball.subsurfaceradius = {SKIN_DARK['radius']}",
            f"scene.materials.ball.subsurfaceradiusscale = {' '.join(map(str, SKIN_DARK['scale']))}"]
    explicit_sss = implicit_sss(parse(sss_mat("\n".join(skin))))
    preset_sss = implicit_sss(parse(
        sss_mat("scene.materials.ball.subsurfacepreset = skin_dark")))
    mfp_ref = tuple(SKIN_DARK["radius"] * s for s in SKIN_DARK["scale"])
    expect_sss("skin_dark explicit", explicit_sss, SKIN_DARK["albedo"], mfp_ref)
    expect_sss("skin_dark preset", preset_sss, SKIN_DARK["albedo"], mfp_ref)

    # explicit color override wins over preset albedo; radius override
    # rescales the mfp
    ovr = implicit_sss(parse(sss_mat(
        "scene.materials.ball.subsurfacepreset = skin_dark\n"
        "scene.materials.ball.subsurfacecolor = 0.1 0.5 0.5\n"
        "scene.materials.ball.subsurfaceradius = 0.1")))
    expect_sss("preset+override", ovr, (0.1, 0.5, 0.5),
               tuple(0.1 * s for s in SKIN_DARK["scale"]))

    # unknown preset must fail at parse time
    try:
        parse(sss_mat("scene.materials.ball.subsurfacepreset = unobtanium"))
        print("FAIL: unknown preset did not raise")
        sys.exit(1)
    except Exception as e:
        assert "subsurfacepreset" in str(e) or "unobtanium" in str(e)
        print(f"unknown preset correctly rejected: {e}")

    # every preset parses and yields a sane volume
    for name in ALL_PRESETS:
        a, d = implicit_sss(parse(sss_mat(
            f"scene.materials.ball.subsurfacepreset = {name}")))
        assert all(0.0 < c < 1.0 for c in a), f"{name}: bad albedo {a}"
        assert all(x > 0 for x in d), f"{name}: bad mfp {d}"
        print(f"preset {name}: albedo {a} mfp {d}")

    # ---- render checks ----------------------------------------------
    engines = []
    if "cpu" in BACKENDS:
        engines.append(("cpu", "PATHCPU", None))
    for name, dtype in (("opencl", "OPENCL_GPU"), ("metal", "METAL_GPU")):
        if name in BACKENDS:
            pysuperluxcore.Init()
            descs = pysuperluxcore.GetOpenCLDeviceDescs()
            mask, i = "", 0
            while True:
                try:
                    t = descs.Get(f"opencl.device.{i}.type").GetString()
                except Exception:
                    break
                mask += "1" if t == dtype else "0"
                i += 1
            if mask:
                engines.append((name, "PATHOCL", mask))
    if not engines:
        print("SKIP: no backends requested/available")
        return
    ref_name, ref_engine, ref_sel = engines[0]

    def render(scene, engine, sel):
        cfg = pysuperluxcore.Properties()
        dev = sel if sel is not None else OCL_DEV
        extra = f'opencl.devices.select = "{dev}"' if dev else ""
        cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 17
path.pathdepth.total = 256
path.pathdepth.diffuse = 256
path.pathdepth.glossy = 64
path.pathdepth.specular = 64
{extra}
""")
        ses = pysuperluxcore.RenderSession(
            pysuperluxcore.RenderConfig(cfg, scene))
        ses.Start()
        deadline = time.monotonic() + RENDER_TIMEOUT_S
        while True:
            ses.UpdateStats()
            if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
                break
            if time.monotonic() > deadline:
                ses.Stop()
                raise TimeoutError(f"render stalled ({engine})")
            time.sleep(0.5)
        rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
        ses.GetFilm().GetOutputFloat(
            pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
        ses.Stop()
        return rgb.reshape(HEIGHT, WIDTH, 3)

    def center(img):
        h, w = img.shape[:2]
        return img[h // 5:4 * h // 5, w // 5:4 * w // 5]

    img = center(render(parse(
        sss_mat("scene.materials.ball.subsurfacepreset = skin_dark")),
        ref_engine, ref_sel))
    cmean = img.mean(axis=(0, 1))
    print(f"skin_dark render center rgb: {cmean}")
    if not np.isfinite(img).all():
        print("FAIL: NaN/inf in preset render")
        sys.exit(1)
    # apparent albedo ~0.47/0.29/0.19 + specular lift; red must dominate
    if not (0.2 < cmean[0] < 0.9 and cmean[0] > cmean[1] > cmean[2]):
        print("FAIL: preset render does not look like skin_dark albedo")
        sys.exit(1)

    for name, engine, sel in engines[1:]:
        gpu = center(render(parse(
            sss_mat("scene.materials.ball.subsurfacepreset = skin_dark")),
            engine, sel))
        ratio = np.abs(img - gpu) / np.maximum(np.maximum(img, gpu), 1e-3)
        ratio = ratio[np.isfinite(ratio)]
        # Per-pixel error at this spp is SSS sampling noise; gate on
        # 8x8 block averages and the per-channel means
        h, w = img.shape[0] // 8 * 8, img.shape[1] // 8 * 8
        blk = lambda im: im[:h, :w].reshape(h // 8, 8, w // 8, 8, 3).mean((1, 3))
        cb, gb = blk(img), blk(gpu)
        bratio = np.abs(cb - gb) / np.maximum(np.maximum(cb, gb), 1e-3)
        cm, gm = img.mean((0, 1)), gpu.mean((0, 1))
        chan = np.max(np.abs(cm - gm) / np.maximum(cm, 1e-3))
        print(f"preset parity {name}-vs-{ref_name}: mean rel. error "
              f"{ratio.mean():.4f}, p95 {np.percentile(ratio, 95):.4f}, "
              f"block {bratio.mean():.4f}, channel means {cm} vs {gm}")
        if bratio.mean() > 0.075 or chan > 0.05:
            print(f"FAIL: {name}/{ref_name} parity out of tolerance")
            sys.exit(1)

    print("PASS")


if __name__ == "__main__":
    main()
