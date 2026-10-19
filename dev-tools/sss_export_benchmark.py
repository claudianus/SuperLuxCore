"""Render the exact exported artist scene directly for fast CPU SSS screening.

Core wall includes native configuration, preparation, tracing and film readback;
it excludes Blender/EXR/display work and must not be called Blender wall time.
The final artist-facing gate still uses stock Blender and model-specific refs.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time
import numpy as np

p=argparse.ArgumentParser();p.add_argument('--module',type=Path,required=True)
p.add_argument('--scene',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--spp',type=int,default=144);p.add_argument('--seed',type=int,default=17)
p.add_argument('--threads',type=int,default=4);p.add_argument('--spectral',action='store_true')
a=p.parse_args();assert a.spp>0 and math.isqrt(a.spp)**2==a.spp
module=a.module.resolve();scene=a.scene.resolve();out=a.out.resolve();assert not out.exists(),out;out.mkdir(parents=True)
sys.path.insert(0,str(module));import pysuperluxcore as s
s.Init();native=Path(s.pysuperluxcore.__file__);identity=hashlib.sha256(native.read_bytes()).hexdigest()
os.chdir(scene);props=s.Properties(str(scene/'render.cfg'))
for k,v in {'renderengine.type':'TILEPATHCPU','renderengine.seed':a.seed,'native.threads.count':a.threads,
            'sampler.type':'TILEPATHSAMPLER','tile.multipass.enable':False,'tilepath.sampling.aa.size':math.isqrt(a.spp),
            'batch.haltspp':0,'batch.halttime':0,'path.spectral.enable':a.spectral,
            'film.outputs.sssraycount.type':'RAYCOUNT','film.outputs.sssraycount.filename':'unused.exr'}.items():props.Set(s.Property(k,v))
(out/'render.cfg').write_text(str(props));t=time.monotonic();cpu_start=time.process_time();config=s.RenderConfig(props);renderer=s.RenderSession(config)
try:
    prep=time.monotonic();renderer.Start();preparation=time.monotonic()-prep;deadline=time.monotonic()+300
    while not renderer.HasDone():
        if time.monotonic()>deadline:raise TimeoutError('CPU SSS core screen timeout')
        time.sleep(.02);renderer.UpdateStats()
    renderer.Stop();film=renderer.GetFilm();width,height=film.GetWidth(),film.GetHeight()
    raw=np.empty((height,width,3),np.float32);film.GetOutputFloat(s.FilmOutputType.RGB,raw,0,False)
    counts=np.empty((height,width),np.uint32);film.GetOutputUInt(s.FilmOutputType.SAMPLECOUNT,counts,0,False)
    rays=np.empty((height,width),np.float32);film.GetOutputFloat(s.FilmOutputType.RAYCOUNT,rays,0,False)
    elapsed=time.monotonic()-t;cpu_elapsed=time.process_time()-cpu_start
finally:
    if renderer.IsStarted():renderer.Stop()
assert np.isfinite(raw).all() and counts.min()==counts.max()==a.spp
np.save(out/'linear.npy',raw);np.save(out/'samples.npy',counts);np.save(out/'rays.npy',rays)
record=dict(engine='SUPERLUXCORE',spp=a.spp,seed=a.seed,spectral=a.spectral,threads=a.threads,size=[width,height],
            seconds=elapsed,cpu_seconds=cpu_elapsed,preparation_seconds=preparation,raycount_buffer_mean=float(rays.mean(dtype=float)),native_sha256=identity,min_samples=int(counts.min()),
            max_samples=int(counts.max()),finite=True,mean_rgb=raw.mean(axis=(0,1),dtype=float).tolist(),
            timing_scope='Native config/preparation/tracing/film readback; excludes Blender, EXR and display',
            diagnostics={k:v for k,v in os.environ.items() if k.startswith('SUPERLUXCORE_SSS_')},
            scene=str(scene),pid=os.getpid())
(out/'result.json').write_text(json.dumps(record,indent=2));print(json.dumps(record),flush=True)
