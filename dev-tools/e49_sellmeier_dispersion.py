# SPDX-License-Identifier: Apache-2.0
#
# E49: Sellmeier 3-term dispersion (scene.materials.*.sellmeier /
# .sellmeierb/.sellmeierc) across glass, roughglass, disney, openpbr.
#
# Under a uniform white environment, a non-dispersive glass sphere is
# achromatic (every pixel sees the full spectrum); a dispersive one
# refracts each sampled wavelength on its own path -> strong chromatic
# fringing. Checks:
#   1. Sellmeier glass sphere: per-pixel saturation >> 0 (dispersion),
#      image mean ~1 (still transmits the environment, i.e. the real
#      coefficient IOR drives refraction - not a black/incorrect index).
#   2. Cauchy (cauchyb) scenes unchanged + CPU/GPU parity.
#   3. Explicit B/C coefficients equal the named preset.
#   4. CPU/GPU parity for sellmeier glass, roughglass, disney, openpbr.
#
# Run from the repo root:
#   python3.13 dev-tools/e49_sellmeier_dispersion.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = 96
RENDER_TIMEOUT_S = 900
OCL_DEV = os.environ.get("E49_OCL_DEV", "")

CAMERA = """
scene.camera.lookat.orig = -2.78 1.6 3.28
scene.camera.lookat.target = -2.78 2.76 3.28
scene.camera.fieldofview = 45
"""

ENV = """
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1.0 1.0 1.0
scene.lights.env.gain = 1.0 1.0 1.0
"""


def render(props_str, engine, seed=17):
    cfg = pysuperluxcore.Properties()
    extra = f'opencl.devices.select = "{OCL_DEV}"' if OCL_DEV else ""
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
{extra}
""")
    props = pysuperluxcore.Properties()
    props.SetFromString(props_str)
    os.chdir(str(REPO))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
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
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def sphere_scene(mat_props):
    return f"""{CAMERA}
{mat_props}
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
{ENV}"""


def chroma(img):
    """Per-pixel saturation stats over lit pixels."""
    lum = img.mean(axis=2) + 1e-6
    sat = (img.max(axis=2) - img.min(axis=2)) / lum
    lit = img.mean(axis=2) > 0.2
    return sat[lit].mean(), (sat > 0.15).mean()


def parity(name, props, tol=0.15):
    cpu = render(props, "PATHCPU")
    gpu = render(props, "PATHOCL")
    if not np.isfinite(cpu).all() or not np.isfinite(gpu).all():
        print(f"FAIL: NaN/inf in {name}")
        sys.exit(1)
    cl, gl = cpu.mean(axis=2), gpu.mean(axis=2)
    mask = (cl > 1e-3) & (gl > 1e-3)
    if not mask.any():
        print(f"FAIL: {name} rendered black")
        sys.exit(1)
    ratio = np.abs(cl[mask] - gl[mask]) / np.maximum(cl[mask], gl[mask])
    print(f"{name}: cpu mean {cl[mask].mean():.4f} gpu {gl[mask].mean():.4f} "
          f"rel.mean {ratio.mean():.4f} p95 {np.percentile(ratio, 95):.4f}")
    if ratio.mean() > tol:
        print(f"FAIL: {name} CPU/GPU parity out of tolerance")
        sys.exit(1)
    return cpu


def main():
    # 1. Dispersion is physically active: strong chromatic fringing vs a
    #    plain glass control, while still transmitting the environment
    #    (mean ~1 - not black, not tinted to zero).
    sell = sphere_scene("""
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = 1.8
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.sellmeier = N-SF6
""")
    plain = sphere_scene("""
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = 1.8
scene.materials.ball.exteriorior = 1.0
""")

    img_sell = parity("glass sellmeier N-SF6", sell)
    img_plain = render(plain, "PATHCPU")
    sat_sell, frac_sell = chroma(img_sell)
    sat_plain, frac_plain = chroma(img_plain)
    print(f"sellmeier: mean {img_sell.mean():.3f} sat {sat_sell:.3f} "
          f"frac {frac_sell:.3f} | plain: sat {sat_plain:.3f} "
          f"frac {frac_plain:.3f}")

    if sat_sell < 0.05 or frac_sell < 0.2:
        print("FAIL: Sellmeier dispersion produces no chromatic fringing")
        sys.exit(1)
    if img_sell.mean() < 0.5 * img_plain.mean():
        print("FAIL: Sellmeier glass loses the transmitted environment "
              "(wrong effective index)")
        sys.exit(1)

    # 2. Cauchy backward compatibility (CPU/GPU parity)
    cauchy = sphere_scene("""
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = 1.8
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.cauchyb = 0.012
""")
    img_cauchy = parity("glass cauchy", cauchy)
    sat_c, frac_c = chroma(img_cauchy)
    if sat_c < 0.05:
        print("FAIL: Cauchy dispersion broke")
        sys.exit(1)

    # 3. Explicit B/C coefficients equal the named preset
    sellBC = sphere_scene("""
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = 1.8
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.sellmeierb = 1.72448482 0.390104889 1.04572858
scene.materials.ball.sellmeierc = 0.0134871947 0.0569318095 118.557185
""")
    img_bc = parity("glass sellmeier B/C", sellBC)
    # The two renderings are statistically identical; compare color stats
    diff = np.abs(img_bc - img_sell)
    print(f"preset-vs-explicit: mean abs diff {diff.mean():.4f}")
    if diff.mean() > 0.05 * img_sell.mean():
        print("FAIL: named preset vs explicit B/C diverge")
        sys.exit(1)

    # 4. Roughglass + Disney + OpenPBR Sellmeier parity
    parity("roughglass sellmeier", sphere_scene("""
scene.materials.ball.type = roughglass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = 1.5
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.uroughness = 0.05
scene.materials.ball.vroughness = 0.05
scene.materials.ball.sellmeier = N-SF10
"""))
    parity("disney sellmeier", sphere_scene("""
scene.materials.ball.type = disney
scene.materials.ball.basecolor = 0.8 0.8 0.9
scene.materials.ball.roughness = 0.1
scene.materials.ball.transmission = 1.0
scene.materials.ball.ior = 1.5
scene.materials.ball.sellmeier = N-SF6
"""))
    parity("openpbr sellmeier", sphere_scene("""
scene.materials.ball.type = openpbr
scene.materials.ball.basecolor = 0.9 0.9 0.9
scene.materials.ball.specularroughness = 0.1
scene.materials.ball.specularior = 1.5
scene.materials.ball.transmissionweight = 1.0
scene.materials.ball.sellmeier = N-SF10
"""))

    print("PASS")


if __name__ == "__main__":
    main()
