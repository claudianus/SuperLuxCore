# SPDX-License-Identifier: Apache-2.0
#
# E108: OpenPBR rough glass energy (white furnace).
#
# A white transmissive OpenPBR sphere (IOR 1.45) must vanish in a constant
# environment at every roughness: the GGX glass multiscatter compensation
# (1/E per interface) assumes the single-scatter lobes have exactly the GGX
# directional albedo. Masked VNDF samples scored through the lobe mixture
# and backfacing microfacets in the BTDF evaluation added energy (furnace
# 1.13-1.32 at roughness 0.8-1); an independent numpy random walk of the
# same model gives 0.905 / 0.389 single-scatter and ~1.0 compensated.
#
# Run from the repo root:
#   python3.13 dev-tools/e108_openpbr_glass_furnace.py
import sys, time, numpy as np
from pathlib import Path
REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'out/build/src/pysuperluxcore/Release')); import pysuperluxcore, math
pysuperluxcore.Init()
NU,NV=96,48
V=[];F=[];N=[]
for j in range(NV+1):
    th=math.pi*j/NV
    for i in range(NU):
        ph=2*math.pi*i/NU
        x,y,z=math.sin(th)*math.cos(ph),math.sin(th)*math.sin(ph),math.cos(th)
        V.append((x,y,z))
for j in range(NV):
    for i in range(NU):
        a=j*NU+i; b=j*NU+(i+1)%NU; c=(j+1)*NU+i; d=(j+1)*NU+(i+1)%NU
        F.append((a,c,b)); F.append((b,c,d))
VS=" ".join("%g %g %g"%v for v in V); FS=" ".join("%d %d %d"%f for f in F)
def run(mat, eng="PATHCPU"):
    scn=pysuperluxcore.Scene(); p=pysuperluxcore.Properties(); p.SetFromString(f'''
scene.camera.lookat.orig = 0 -6 0
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 0 1
scene.camera.fieldofview = 10
scene.lights.e.type = constantinfinite
scene.lights.e.color = 1 1 1
scene.lights.e.gain = 1 1 1
{"scene.materials.m.type = roughglass" if "openpbr" not in mat else ""}
scene.materials.m.kr = 1 1 1
scene.materials.m.kt = 1 1 1
scene.materials.m.interiorior = 1.45
{mat}
scene.objects.o.vertices = {VS}
scene.objects.o.faces = {FS}
scene.objects.o.normals = {VS}
scene.objects.o.material = m
'''); scn.Parse(p)
    cfg=pysuperluxcore.Properties(); cfg.SetFromString(f"film.width = 32\nfilm.height = 32\nfilm.imagepipelines.0.0.type = NOP\nrenderengine.type = {eng}\nsampler.type = SOBOL\nbatch.haltspp = 256\npath.pathdepth.total = 128\npath.pathdepth.glossy = 128\npath.pathdepth.specular = 128\npath.pathdepth.diffuse = 128\npath.hybridbackforward.enable = 0\n")
    s=pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg,scn)); s.Start()
    while True:
        s.UpdateStats()
        if s.GetStats().Get("stats.renderengine.pass").GetInt()>=256: break
        time.sleep(0.2)
    a=np.empty(32*32*3,np.float32); s.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,a,0,True); s.Stop()
    return a.reshape(32,32,3)[8:24,8:24].mean()


fails = []
for r in (0.3, 0.5, 0.8, 1.0):
    for eng in ("PATHCPU", "PATHOCL"):
        v = run(f"scene.materials.m.type = openpbr\nscene.materials.m.basemetalness = 0\nscene.materials.m.transmissionweight = 1\nscene.materials.m.specularior = 1.45\nscene.materials.m.specularroughness = {r}\n# openpbr\n", eng)
        ok = abs(v - 1.0) < 0.04
        print(f"[{'PASS' if ok else 'FAIL'}] r={r} {eng}: furnace={v:.3f}")
        if not ok:
            fails.append((r, eng))
print("PASS overall" if not fails else f"FAIL overall: {fails}")
sys.exit(1 if fails else 0)
