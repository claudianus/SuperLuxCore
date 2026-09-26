# E35 visual demo: OpenPBR random-walk SSS, three albedo/mfp variants
# (jade, wax, porcelain) in a backlit showcase scene. Renders PATHCPU and
# PATHOCL at 1280x720 for visual parity inspection.
#
# Run from the repo root:  python3 dev-tools/e35_visual_demo.py
# (pysuperluxcore must be importable; the script prepends the Release module
# dir itself.)

import sys, numpy as np
sys.path.insert(0, "out/build/src/pysuperluxcore/Release")
import pysuperluxcore

CX, CY, CZ = -2.776, 2.760, 3.279
R = 0.85
def tx(x, y, z):  # column-major translate
    return f"1 0 0 0  0 1 0 0  0 0 1 0  {x-CX} {y-CY} {z-CZ} 1"

def sss_mat(name, color, radius, ior=1.45, rough=0.18):
    return f"""
scene.materials.{name}.type = openpbr
scene.materials.{name}.basecolor = {color}
scene.materials.{name}.baseweight = 1.0
scene.materials.{name}.specularweight = 1.0
scene.materials.{name}.specularior = {ior}
scene.materials.{name}.specularroughness = {rough}
scene.materials.{name}.subsurfaceweight = 1.0
scene.materials.{name}.subsurfacecolor = {color}
scene.materials.{name}.subsurfaceradius = {radius} {radius} {radius}
scene.materials.{name}.subsurfaceradiusscale = 1.0 1.0 1.0
scene.materials.{name}.subsurfaceanisotropy = 0.0
"""

SCENE = f"""
scene.camera.lookat.orig = 0.0 -9.6 2.6
scene.camera.lookat.target = 0.0 0.0 0.7
scene.camera.fieldofview = 42

scene.lights.env.type = infinite
scene.lights.env.file = scenes/luxball/leafy_knoll_2k.hdr
scene.lights.env.gain = 0.26 0.26 0.26
scene.lights.env.shift = 0.35 0.0
scene.lights.sun.type = sun
scene.lights.sun.dir = 0.25 -1.0 -0.55
scene.lights.sun.gain = 0.85 0.78 0.7
scene.lights.sun.turbidity = 4.0

scene.materials.floor.type = matte
scene.materials.floor.kd = 0.20 0.21 0.23
{sss_mat("jade", "0.15 0.48 0.28", 0.5)}
{sss_mat("wax", "0.9 0.36 0.16", 1.2)}
{sss_mat("porcelain", "0.62 0.72 0.85", 0.35, ior=1.5, rough=0.08)}

scene.objects.floor.material = floor
scene.objects.floor.ply = scenes/mnee/floor.ply
scene.objects.floor.transformation = 10 0 0 0  0 10 0 0  0 0 1 0  0 0 0 1
scene.objects.jade.material = jade
scene.objects.jade.ply = scenes/cornell/sphere-mid.ply
scene.objects.jade.transformation = {tx(-1.72, 0.0, R)}
scene.objects.wax.material = wax
scene.objects.wax.ply = scenes/cornell/sphere-mid.ply
scene.objects.wax.transformation = {tx(0.0, 0.0, R)}
scene.objects.porc.material = porcelain
scene.objects.porc.ply = scenes/cornell/sphere-mid.ply
scene.objects.porc.transformation = {tx(1.72, 0.0, R)}
"""

def render(engine, out, spp):
    props = pysuperluxcore.Properties(); props.SetFromString(SCENE)
    scene = pysuperluxcore.Scene(); scene.Parse(props)
    dev = 'opencl.devices.select = "1"' if engine == "PATHOCL" else ""
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = 1280
film.height = 720
film.imagepipelines.0.0.type = TONEMAP_LINEAR
film.imagepipelines.0.0.scale = 0.42
film.imagepipelines.0.1.type = INTEL_OIDN
film.imagepipelines.0.1.prefilter.enable = 1
film.imagepipelines.0.2.type = GAMMA_CORRECTION
film.outputs.1.type = RGB_IMAGEPIPELINE
film.outputs.1.filename = {out}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
path.pathdepth.total = 32
path.pathdepth.diffuse = 32
path.pathdepth.glossy = 16
path.pathdepth.specular = 16
{dev}
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    import time
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp: break
        time.sleep(0.5)
    ses.GetFilm().Save()
    ses.Stop()
    print("saved", out)

render("PATHCPU", "/tmp/e35_sss_cpu_final.png", 768)
render("PATHOCL", "/tmp/e35_sss_gpu_final.png", 768)
