#!/usr/bin/env python3
"""E53 visual check (720p, AgX Punchy): measured-media SSS presets.

Two Suzanne heads on the bigmonkey studio set:
  left   openpbr subsurfacepreset = skin_light
  right  openpbr subsurfacepreset = skin_dark
plus a back rim light so the ears/nose thin regions show real
translucency. subsurfaceradius is scaled to the scene's units (the
preset's reference radius is meters; this set is ~10x larger than a
real head).

  python3.13 dev-tools/e53_sss_visual.py [gpu|cpu]
"""
import sys, os, time
from pathlib import Path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

REPO = Path(__file__).resolve().parent.parent
OUTDIR = Path("/tmp/e53_sss"); OUTDIR.mkdir(exist_ok=True)
W, H, SPP = 1280, 720, 900
OCIO = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/colormanagement/config.ocio"
ENGINE = "PATHOCL" if len(sys.argv) < 2 or sys.argv[1] == "gpu" else "PATHCPU"

# scene-unit mfp: monkey head ~5 units tall ~ 24cm -> unit ~4.4cm;
# skin ref mfp 1mm -> radius ~0.023. Slightly larger for legibility.
# Lighting reuses the studio set: ceiling mesh lights + one warm area
# sphere low behind the heads so thin regions (ears, nose) transmit.
SCENE = r"""
scene.camera.lookat = 8.0 -11.0 5.0 1.0 0.0 1.6

scene.materials.whitematte.type = matte
scene.materials.whitematte.kd = 0.55 0.55 0.57

scene.materials.whitelight.type = matte
scene.materials.whitelight.emission = 22 20 19
scene.materials.whitelight.kd = 0.0 0.0 0.0

scene.materials.skinL.type = openpbr
scene.materials.skinL.basecolor = 0.5 0.35 0.28
scene.materials.skinL.specularior = 1.4
scene.materials.skinL.specularroughness = 0.42
scene.materials.skinL.subsurfaceweight = 1.0
scene.materials.skinL.subsurfacepreset = skin_light
scene.materials.skinL.subsurfaceradius = 0.12
scene.materials.skinL.coatweight = 0.05
scene.materials.skinL.coatior = 1.4
scene.materials.skinL.coatroughness = 0.25

scene.materials.skinR.type = openpbr
scene.materials.skinR.basecolor = 0.5 0.35 0.28
scene.materials.skinR.specularior = 1.4
scene.materials.skinR.specularroughness = 0.42
scene.materials.skinR.subsurfaceweight = 1.0
scene.materials.skinR.subsurfacepreset = skin_dark
scene.materials.skinR.subsurfaceradius = 0.12
scene.materials.skinR.coatweight = 0.05
scene.materials.skinR.coatior = 1.4
scene.materials.skinR.coatroughness = 0.25

scene.objects.back.material = whitematte
scene.objects.back.ply = scenes/bigmonkey/room.ply
scene.objects.l1.material = whitelight
scene.objects.l1.ply = scenes/bigmonkey/bigmonkey-lights.ply
scene.objects.headL.material = skinL
scene.objects.headL.ply = scenes/bigmonkey/bigmonkey.ply
scene.objects.headL.transformation = 1 0 0 0  0 1 0 0  0 0 1 0  -2.0 0.4 0 1
scene.objects.headR.material = skinR
scene.objects.headR.ply = scenes/bigmonkey/bigmonkey.ply
scene.objects.headR.transformation = 0.8 0 0 0  0 0.8 0 0  0 0 0.8 0  3.4 -0.5 -0.4 1

# warm rim/backlight for translucency through thin geometry
# (hidden behind the heads: ears/nostrils carry the glow)
scene.materials.rimlight.type = matte
scene.materials.rimlight.emission = 1600 480 150
scene.materials.rimlight.kd = 0.0 0.0 0.0
scene.objects.rim.material = rimlight
scene.objects.rim.ply = scenes/cornell/sphere-mid.ply
scene.objects.rim.transformation = 0.6 0 0 0  0 0.6 0 0  0 0 0.6 0  -5.2 4.2 3.2 1
"""


def device_mask(want="METAL_GPU"):
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    m = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        m += "1" if t == want else "0"
        i += 1
    return m or None


def main():
    os.chdir(str(REPO))
    sc = pysuperluxcore.Scene()
    props = pysuperluxcore.Properties()
    props.SetFromString(SCENE)
    sc.Parse(props)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = {ENGINE}
sampler.type = SOBOL
batch.haltspp = {SPP}
path.pathdepth.total = 24
path.pathdepth.diffuse = 12
path.pathdepth.glossy = 8
path.pathdepth.specular = 8
film.imagepipelines.0.0.type = TONEMAP_OPENCOLORIO
film.imagepipelines.0.0.mode = DISPLAY_CONVERSION
film.imagepipelines.0.0.config = {OCIO}
film.imagepipelines.0.0.src = "Linear Rec.709"
film.imagepipelines.0.0.display = sRGB
film.imagepipelines.0.0.view = AgX
film.imagepipelines.0.0.look = "AgX - Punchy"
film.outputs.beauty.type = RGB_IMAGEPIPELINE
film.outputs.beauty.index = 0
film.outputs.beauty.filename = {OUTDIR}/sss_presets_{ENGINE.lower()}.png
""")
    if ENGINE == "PATHOCL":
        sel = device_mask()
        if sel:
            cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + 1800
    while True:
        ses.UpdateStats()
        p = ses.GetStats().Get("stats.renderengine.pass").GetInt()
        if p >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"stalled at {p}/{SPP}")
        time.sleep(1.0)
    ses.GetFilm().SaveOutputs()
    ses.Stop()
    print(f"saved {OUTDIR}/sss_presets_{ENGINE.lower()}.png")


if __name__ == "__main__":
    main()
