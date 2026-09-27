#!/usr/bin/env python3
"""E50 showcase (720p, AgX Punchy): new shader stack in one hero frame.

Cornell-studio layout, three spheres + walls:
  left   glass + Sellmeier N-SF10     -> spectral dispersion fringes
  mid    openpbr SSS (CB'15 profile)  -> translucent jade subsurface
  right  metal2 chromium + edgetint   -> F82 warm grazing tint
  walls  velvet charlie               -> sheen rim on cloth sides
  box    disney + multibounce         -> compensated rough specular

  python3.13 dev-tools/e50_visual_showcase.py [gpu|cpu]
"""
import sys, os, time
from pathlib import Path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "out/build/src/pysuperluxcore/Release"))
import numpy as np
import pysuperluxcore

REPO = Path(__file__).resolve().parent.parent
OUTDIR = Path("/tmp/e50_showcase"); OUTDIR.mkdir(exist_ok=True)
W, H, SPP = 1280, 720, 640
OCIO = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/colormanagement/config.ocio"
ENGINE = "PATHOCL" if len(sys.argv) < 2 or sys.argv[1] == "gpu" else "PATHCPU"

SCENE = r"""
scene.camera.lookat.orig = -2.78 -8. 2.73
scene.camera.lookat.target = -2.78 2. 2.730003
scene.camera.fieldofview = 39.1463

# --- enclosure: disney dark studio box (multibounce-compensated spec) ---
scene.materials.Box.type = disney
scene.materials.Box.basecolor = 0.55 0.55 0.58
scene.materials.Box.roughness = 0.45
scene.materials.Box.metallic = 0.0
scene.materials.Box.specular = 0.8
scene.materials.Box.multibounce = 1

# --- cloth side walls: velvet + charlie sheen ---
scene.materials.Red.type = velvet
scene.materials.Red.kd = 0.42 0.04 0.03
scene.materials.Red.p1 = 5
scene.materials.Red.p2 = 40
scene.materials.Red.p3 = -0.5
scene.materials.Red.thickness = 0.1
scene.materials.Red.model = charlie
scene.materials.Red.sheenroughness = 0.35
scene.materials.Green.type = velvet
scene.materials.Green.kd = 0.03 0.30 0.06
scene.materials.Green.p1 = 5
scene.materials.Green.p2 = 40
scene.materials.Green.p3 = -0.5
scene.materials.Green.thickness = 0.1
scene.materials.Green.model = charlie
scene.materials.Green.sheenroughness = 0.35

scene.materials.Light.type = matte
scene.materials.Light.emission = 2200 2200 2200
scene.materials.Light.kd = 0.0 0.0 0.0

# --- left: sellmeier dispersion glass ---
scene.materials.SphL.type = glass
scene.materials.SphL.kt = 0.97 0.97 0.97
scene.materials.SphL.kr = 0.97 0.97 0.97
scene.materials.SphL.interiorior = 1.52
scene.materials.SphL.sellmeier = N-SF10
scene.materials.SphL.volume.interior = glassvol

# --- mid: openpbr subsurface jade (cb15 profile) ---
scene.materials.SphM.type = openpbr
scene.materials.SphM.basecolor = 0.02 0.45 0.15
scene.materials.SphM.metalness = 0.0
scene.materials.SphM.specularroughness = 0.35
scene.materials.SphM.speculariorlevel = 0.9
scene.materials.SphM.specularior = 1.45
scene.materials.SphM.subsurfaceweight = 0.7
scene.materials.SphM.subsurfacecolor = 0.0 0.30 0.08
scene.materials.SphM.subsurfaceradius = 0.5
scene.materials.SphM.subsurfaceradiusscale = 0.5 1.0 0.4

# --- right: metal2 chromium + warm edge tint ---
scene.textures.SphRfres.type = fresnelpreset
scene.textures.SphRfres.name = chromium
scene.materials.SphR.type = metal2
scene.materials.SphR.fresnel = SphRfres
scene.materials.SphR.edgetint = 0.9 0.75 0.6
scene.materials.SphR.uroughness = 0.08
scene.materials.SphR.vroughness = 0.08
scene.materials.SphR.multibounce = 1

scene.lights.env.type = constantinfinite
scene.lights.env.color = 0.5 0.6 0.7
scene.lights.env.gain = 0.12 0.12 0.12

scene.volumes.glassvol.type = clear
scene.volumes.glassvol.absorption = 0 0 0
scene.volumes.glassvol.ior = 1.52

scene.objects.Box.material = Box
scene.objects.Box.ply = scenes/cornell/box.ply
scene.objects.WallL.material = Red
scene.objects.WallL.ply = scenes/cornell/HalveRed.ply
scene.objects.WallR.material = Green
scene.objects.WallR.ply = scenes/cornell/DarkGreen.ply
scene.objects.Ceiling.material = Light
scene.objects.Ceiling.ply = scenes/cornell/Grey.ply
scene.objects.sphere-left.material = SphL
scene.objects.sphere-left.ply = scenes/cornell/sphere-left.ply
scene.objects.sphere-mid.material = SphM
scene.objects.sphere-mid.ply = scenes/cornell/sphere-mid.ply
scene.objects.sphere-right.material = SphR
scene.objects.sphere-right.ply = scenes/cornell/sphere-right.ply
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
film.imagepipelines.0.0.type = TONEMAP_OPENCOLORIO
film.imagepipelines.0.0.mode = DISPLAY_CONVERSION
film.imagepipelines.0.0.config = {OCIO}
film.imagepipelines.0.0.src = "Linear Rec.709"
film.imagepipelines.0.0.display = sRGB
film.imagepipelines.0.0.view = AgX
film.imagepipelines.0.0.look = "AgX - Punchy"
film.outputs.beauty.type = RGB_IMAGEPIPELINE
film.outputs.beauty.index = 0
film.outputs.beauty.filename = {OUTDIR}/beauty_agx_{ENGINE.lower()}.png
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
    print(f"saved {OUTDIR}/beauty_agx_{ENGINE.lower()}.png")


if __name__ == "__main__":
    main()
