"""A transmitted NEE ray must leave its incident absorbing medium.

Two slabs have identical optical thickness and BSDFs. Moving an exterior
point light from one to two metres, with inverse-square compensated power,
must not add Beer attenuation outside the slab. A point source cannot be
hit by continuation rays, so this isolates next-event estimation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

import numpy as np

ap=argparse.ArgumentParser()
ap.add_argument('--module',required=True)
ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--engine',choices=['TILEPATHCPU','TILEPATHOCL'],default='TILEPATHCPU')
a=ap.parse_args()
sys.path.insert(0,str(Path(a.module).resolve()))
import pysuperluxcore as s
s.Init()
mask=None
if a.engine=='TILEPATHOCL':
    devices=s.GetOpenCLDeviceDescs()
    mask=''.join('1' if devices.Get(n+'.type').GetString()=='METAL_GPU' else '0'
                 for n in devices.GetAllUniqueSubNames('opencl.device'))
    assert '1' in mask,'No Metal GPU'
a.out.mkdir(parents=True,exist_ok=True)
def props(text):
    p=s.Properties();p.SetFromString(text);return p
rows=[]
for absorption in (0.,1.):
    for distance in (1.,2.):
        text=f'''
scene.camera.lookat.orig = 0 0 -1
scene.camera.lookat.target = 0 0 1
scene.camera.up = 0 1 0
scene.camera.autovolume.enable = 0
scene.camera.fieldofview = .1
scene.volumes.slab.type = clear
scene.volumes.slab.ior = 1.5
scene.volumes.slab.absorption = {absorption} {absorption} {absorption}
scene.materials.entry.type = null
scene.materials.entry.volume.interior = slab
scene.materials.exit.type = mattetranslucent
scene.materials.exit.kr = 0 0 0
scene.materials.exit.kt = 1 1 1
scene.materials.exit.uroughness = .3
scene.materials.exit.vroughness = .3
scene.materials.exit.volume.interior = slab
scene.objects.entry.material = entry
scene.objects.entry.vertices = -10 -10 0  -10 10 0  10 10 0  10 -10 0
scene.objects.entry.faces = 0 1 2 0 2 3
scene.objects.exit.material = exit
scene.objects.exit.vertices = -10 -10 .1  10 -10 .1  10 10 .1  -10 10 .1
scene.objects.exit.faces = 0 1 2 0 2 3
scene.lights.point.type = point
scene.lights.point.position = 0 0 {distance+.1}
scene.lights.point.gain = {distance**2} {distance**2} {distance**2}
'''
        scene=s.Scene();scene.Parse(props(text))
        cfg=props('''
renderengine.type = TILEPATHCPU
renderengine.seed = 17
sampler.type = TILEPATHSAMPLER
native.threads.count = 1
film.width = 8
film.height = 8
film.filter.type = NONE
film.hw.enable = 0
tile.size = 8
tile.multipass.enable = 0
tilepath.sampling.aa.size = 16
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = RGB
film.outputs.0.filename = unused.exr
path.spectral.enable = 0
path.lighttracing.auto = 0
path.lighttracing.enable = 0
path.hybridbackforward.enable = 0
path.mnee.auto = 0
path.pathdepth.total = 8
path.clamping.variance.maxvalue = 0
''')
        cfg.Set(s.Property('renderengine.type',a.engine))
        if mask:
            cfg.SetFromString(f'''opencl.devices.select = {mask}
opencl.native.threads.count = 0
opencl.task.count = 16384
''')
        ses=s.RenderSession(s.RenderConfig(cfg,scene))
        try:
            ses.Start();deadline=time.monotonic()+30
            while not ses.HasDone():
                if time.monotonic()>deadline:raise TimeoutError()
                time.sleep(.01);ses.UpdateStats()
            ses.Stop()
            image=np.empty((8,8,3),np.float32)
            ses.GetFilm().GetOutputFloat(s.FilmOutputType.RGB,image,0,False)
            assert np.isfinite(image).all()
            mean=float(image.mean(dtype=np.float64))
            rows.append(dict(absorption=absorption,distance=distance,mean=mean))
        finally:
            if ses.IsStarted():ses.Stop()
for i in (0,2):
    ratio=rows[i+1]['mean']/rows[i]['mean']
    rows[i+1]['far_near_ratio']=ratio
native=Path(getattr(s,'pysuperluxcore',s).__file__)
result=dict(engine=a.engine,native_sha256=hashlib.sha256(native.read_bytes()).hexdigest(),cases=rows)
(a.out/'result.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result),flush=True)
assert abs(rows[1]['far_near_ratio']-1)<1e-4,rows
assert abs(rows[3]['far_near_ratio']-1)<1e-4,rows
assert abs(rows[2]['mean']/rows[0]['mean']-np.exp(-.1))<1e-4,rows
