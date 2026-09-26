# SPDX-License-Identifier: Apache-2.0
#
# E35: random-walk SSS albedo parametrization (scene.volumes.*.sssalbedo /
# .sssmfp on homogeneous volumes, and the OpenPBR subsurface* shortcut).
#
# A glass sphere with matched IOR (interiorior = exteriorior = 1, Fresnel
# F = 0 -> pure transmission) wraps an albedo-parametrized interior volume
# in a white furnace. With mfp << sphere radius (mfp=0.05, r/mfp ~ 17) the
# geometry is in the half-space regime and the camera-side radiance is the
# medium's diffuse reflectance, which the d'Eon inversion drives to the
# requested surface albedo A (verified against an independent Monte-Carlo
# replica of this exact camera/sphere geometry). Mean of the center crop
# must be ~ A.
#
# Also checks: anisotropy g != 0 keeps the same albedo, the raw
# absorption/scattering coefficient mode is unaffected, OpenPBR's
# subsurface_weight path produces the same reflectance, and CPU/GPU
# parity.
#
# Run from the repo root:
#   python3.13 dev-tools/e35_sss_albedo.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Release"))
import pyluxcore

WIDTH, HEIGHT = 320, 240
SPP = 128
RENDER_TIMEOUT_S = 900
# PATHOCL device mask; length must match the enumerated device count
# (1 device -> "1"). Override with E35_OCL_DEV on multi-GPU machines.
OCL_DEV = os.environ.get("E35_OCL_DEV", "1")


def render(scene, engine, seed=17):
    cfg = pyluxcore.Properties()
    # NOP pipeline: the default AutoLinearToneMap normalizes image mean and
    # would hide reflectance differences in a white furnace. Deep pathdepth
    # for the random walk: volume scatter vertices are DIFFUSE events, so
    # the per-type depth must be raised too (defaults: diffuse/glossy 4).
    extra = f'opencl.devices.select = "{OCL_DEV}"' if engine == "PATHOCL" else ""
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.pathdepth.total = 256
path.pathdepth.diffuse = 256
path.pathdepth.glossy = 64
path.pathdepth.specular = 64
{extra}
""")
    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
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
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def center_stat(img):
    # Sphere fills the frame; use the inner 60% to skip grazing-incidence
    # rim pixels (physically darker) and the silhouette edge.
    h, w = img.shape[:2]
    crop = img[h // 5:4 * h // 5, w // 5:4 * w // 5]
    lum = crop.mean(axis=2)
    return lum.mean(), np.percentile(lum, 5), np.percentile(lum, 95)


def parse(props_str):
    props = pyluxcore.Properties()
    props.SetFromString(props_str)
    os.chdir(str(REPO))
    scene = pyluxcore.Scene()
    scene.Parse(props)
    return scene


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


def sss_furnace(albedo, mfp=0.05, g=0.0, engine="PATHCPU"):
    return render(parse(f"""{CAMERA}
scene.volumes.sss.type = homogeneous
scene.volumes.sss.sssalbedo = {albedo} {albedo} {albedo}
scene.volumes.sss.sssmfp = {mfp} {mfp} {mfp}
scene.volumes.sss.asymmetry = {g} {g} {g}
scene.volumes.sss.multiscattering = 1
scene.volumes.sss.phase = hg
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = 1.0
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.volume.interior = sss
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
{FURNACE}"""), engine)


def raw_volume_furnace(engine="PATHCPU"):
    # Regression: explicit coefficient mode (sigma_s=5, sigma_a=0 -> a
    # non-absorbing medium) must be unaffected by the new parametrization
    # and return ~unit reflectance.
    return render(parse(f"""{CAMERA}
scene.volumes.vol.type = homogeneous
scene.volumes.vol.absorption = 0.0 0.0 0.0
scene.volumes.vol.scattering = 5.0 5.0 5.0
scene.volumes.vol.asymmetry = 0.0 0.0 0.0
scene.volumes.vol.multiscattering = 1
scene.volumes.vol.phase = hg
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = 1.0
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.volume.interior = vol
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
{FURNACE}"""), engine)


def openpbr_furnace(albedo, radius=0.05, ior=1.0, engine="PATHCPU"):
    # OpenPBR end-to-end: subsurface_weight=1 makes the parser auto-create
    # the albedo-parametrized interior volume. ior=1.0 keeps the dielectric
    # interface index-matched so the furnace measures the SSS albedo alone;
    # at ior=1.5 specular Fresnel/TIR trapping is physical and must be
    # compared against a glass boundary (glass_furnace), not against A.
    return render(parse(f"""{CAMERA}
scene.materials.ball.type = openpbr
scene.materials.ball.basecolor = 1.0 1.0 1.0
scene.materials.ball.baseweight = 1.0
scene.materials.ball.specularweight = 0.0
scene.materials.ball.specularior = {ior}
scene.materials.ball.roughness = 1.0
scene.materials.ball.subsurfaceweight = 1.0
scene.materials.ball.subsurfacecolor = {albedo} {albedo} {albedo}
scene.materials.ball.subsurfaceradius = {radius} {radius} {radius}
scene.materials.ball.subsurfaceradiusscale = 1.0 1.0 1.0
scene.materials.ball.subsurfaceanisotropy = 0.0
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
{FURNACE}"""), engine)


def glass_furnace(albedo, ior, mfp=0.05, engine="PATHCPU"):
    # Reference: same SSS volume behind a glass boundary at a given IOR.
    return render(parse(f"""{CAMERA}
scene.volumes.sss.type = homogeneous
scene.volumes.sss.sssalbedo = {albedo} {albedo} {albedo}
scene.volumes.sss.sssmfp = {mfp} {mfp} {mfp}
scene.volumes.sss.asymmetry = 0.0 0.0 0.0
scene.volumes.sss.multiscattering = 1
scene.volumes.sss.phase = hg
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = {ior}
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.volume.interior = sss
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
{FURNACE}"""), engine)


def expect(name, img, target, tol=0.15):
    mean, p5, p95 = center_stat(img)
    rel = abs(mean - target) / target
    print(f"{name}: center mean {mean:.4f} p5 {p5:.4f} p95 {p95:.4f} "
          f"(target ~{target})")
    if not np.isfinite(img).all():
        print(f"FAIL: NaN/inf in {name}")
        sys.exit(1)
    if rel > tol:
        print(f"FAIL: {name} reflectance {mean:.4f} != {target} "
              f"(rel {rel:.2f})")
        sys.exit(1)
    return mean


def main():
    # Albedo round-trip: measured diffuse reflectance ~ requested A.
    for a in (1.0, 0.8, 0.5, 0.3):
        expect(f"sss furnace A={a}", sss_furnace(a), a)

    # Anisotropy: g is folded into alpha, so the same surface albedo must
    # come out of a forward-scattering medium.
    expect("sss furnace A=0.5 g=0.6", sss_furnace(0.5, g=0.6), 0.5)

    # Raw coefficient mode regression (non-absorbing medium -> ~1).
    expect("raw sigma_s=5 sigma_a=0", raw_volume_furnace(), 1.0, tol=0.10)

    # OpenPBR implicit volume end-to-end, index-matched interface: the
    # furnace measures the requested surface albedo directly.
    expect("openpbr sss A=0.5 ior=1.0", openpbr_furnace(0.5, ior=1.0), 0.5)

    # OpenPBR dielectric interface at ior=1.5: specular Fresnel + TIR
    # trapping is physical; parity with the same SSS volume behind glass.
    o = center_stat(openpbr_furnace(0.5, ior=1.5))[0]
    g = center_stat(glass_furnace(0.5, 1.5))[0]
    print(f"openpbr sss ior=1.5 {o:.4f} vs glass ref {g:.4f} "
          f"(rel {abs(o - g) / g:.2f})")
    if abs(o - g) / g > 0.10:
        print("FAIL: openpbr ior=1.5 deviates from glass reference")
        sys.exit(1)

    # CPU/GPU parity on the albedo-parametrized path.
    cpu = sss_furnace(0.5, engine="PATHCPU")
    gpu = sss_furnace(0.5, engine="PATHOCL")
    cl, gl = cpu.mean(axis=2), gpu.mean(axis=2)
    mask = cl > 1e-3
    ratio = np.abs(cl[mask] - gl[mask]) / np.maximum(cl[mask], gl[mask])
    print(f"parity: mean rel. error {ratio.mean():.4f}, "
          f"p95 {np.percentile(ratio, 95):.4f}")
    if ratio.mean() > 0.15:
        print("FAIL: CPU/GPU parity out of tolerance")
        sys.exit(1)

    # CPU/GPU parity through the OpenPBR lobe mixture (interface tint,
    # TIR fallback and the implicit SSS volume all live in the material
    # kernel, not the volume, so they need their own parity check).
    for ior in (1.0, 1.5):
        oc = center_stat(openpbr_furnace(0.5, ior=ior, engine="PATHCPU"))[0]
        og = center_stat(openpbr_furnace(0.5, ior=ior, engine="PATHOCL"))[0]
        rel = abs(oc - og) / oc
        print(f"openpbr parity ior={ior}: cpu {oc:.4f} gpu {og:.4f} "
              f"(rel {rel:.2f})")
        if rel > 0.10:
            print(f"FAIL: openpbr CPU/GPU parity out of tolerance")
            sys.exit(1)

    print("PASS")


if __name__ == "__main__":
    main()
