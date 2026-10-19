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
p.add_argument('--tile-size',type=int,help='Override CPU tile width and height; minimum 8, sample budget unchanged')
p.add_argument('--aovs',action='store_true',help='Export raw first-camera-lobe AOVs for RGB variance diagnostics')
p.add_argument('--path-audit',action='store_true',help='Export cumulative internal-scatter LPEs without changing transport')
a=p.parse_args();assert a.spp>0 and math.isqrt(a.spp)**2==a.spp
if a.tile_size is not None and a.tile_size < 8:p.error('--tile-size must be at least 8')
if (a.aovs or a.path_audit) and a.spectral:p.error('Diagnostic output currently validates RGB only')
module=a.module.resolve();scene=a.scene.resolve();out=a.out.resolve();assert not out.exists(),out;out.mkdir(parents=True)
sys.path.insert(0,str(module));import pysuperluxcore as s
s.Init();native=Path(s.pysuperluxcore.__file__);identity=hashlib.sha256(native.read_bytes()).hexdigest();benchmark_identity_before=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
os.chdir(scene);props=s.Properties(str(scene/'render.cfg'))
for k,v in {'renderengine.type':'TILEPATHCPU','renderengine.seed':a.seed,'native.threads.count':a.threads,
            'sampler.type':'TILEPATHSAMPLER','tile.multipass.enable':False,'tilepath.sampling.aa.size':math.isqrt(a.spp),
            'batch.haltspp':0,'batch.halttime':0,'path.spectral.enable':a.spectral,
            'film.outputs.sssraycount.type':'RAYCOUNT','film.outputs.sssraycount.filename':'unused.exr',
            'path.regularization.auto':False,'path.regularization.sigma':0.,'path.regularization.halflife':0.,
            'path.filterglossy':0.,'path.clamping.cycles.direct':0.,'path.clamping.cycles.indirect':0.,
            'path.clamping.variance.maxvalue':0.}.items():props.Set(s.Property(k,v))
if a.tile_size is not None:
    for key in ['tile.size','tile.size.x','tile.size.y']:props.Set(s.Property(key,a.tile_size))
aov_names=([kind+'_'+side for kind in ['DIRECT_DIFFUSE','DIRECT_GLOSSY','INDIRECT_DIFFUSE',
            'INDIRECT_GLOSSY','INDIRECT_SPECULAR'] for side in ['REFLECT','TRANSMIT']]+['EMISSION']) if a.aovs else []
for name in aov_names:
    props.Set(s.Property('film.outputs.sssdiag'+name+'.type',name))
    props.Set(s.Property('film.outputs.sssdiag'+name+'.filename','unused.exr'))
lpe_definitions=([('all','C.*(L|E)'),('volume1plus','C.*V.*(L|E)'),
                  ('volume2plus','C.*V.*V.*(L|E)'),('volume4plus','C.*V.*V.*V.*V.*(L|E)')]
                 if a.path_audit else [])
for index,(name,expression) in enumerate(lpe_definitions):
    props.Set(s.Property('film.lpe.'+str(index)+'.expression',expression))
    props.Set(s.Property('film.lpe.'+str(index)+'.name',name))
    props.Set(s.Property('film.outputs.ssspath'+name+'.type','LPE'))
    props.Set(s.Property('film.outputs.ssspath'+name+'.index',index))
    props.Set(s.Property('film.outputs.ssspath'+name+'.filename','unused.exr'))
authored=scene/'authored-scene.json'
authored_sha=hashlib.sha256(authored.read_bytes()).hexdigest() if authored.exists() else None
(out/'render.cfg').write_text(str(props));t=time.monotonic();cpu_start=time.process_time();config=s.RenderConfig(props);renderer=s.RenderSession(config)
effective=config.GetProperties()
checked={key:effective.Get(key).GetFloat() for key in
         ['path.regularization.sigma','path.regularization.halflife','path.filterglossy',
          'path.clamping.cycles.direct','path.clamping.cycles.indirect','path.clamping.variance.maxvalue']}
assert all(v==0 for v in checked.values()),checked
(out/'effective.cfg').write_text(str(config.GetProperties()))
try:
    prep=time.monotonic();renderer.Start();preparation=time.monotonic()-prep;deadline=time.monotonic()+300
    while not renderer.HasDone():
        if time.monotonic()>deadline:raise TimeoutError('CPU SSS core screen timeout')
        time.sleep(.02);renderer.UpdateStats()
    renderer.Stop();film=renderer.GetFilm();width,height=film.GetWidth(),film.GetHeight()
    raw=np.empty((height,width,3),np.float32);film.GetOutputFloat(s.FilmOutputType.RGB,raw,0,False)
    counts=np.empty((height,width),np.uint32);film.GetOutputUInt(s.FilmOutputType.SAMPLECOUNT,counts,0,False)
    rays=np.empty((height,width),np.float32);film.GetOutputFloat(s.FilmOutputType.RAYCOUNT,rays,0,False)
    aovs={}
    for name in aov_names:
        value=np.empty_like(raw);film.GetOutputFloat(getattr(s.FilmOutputType,name),value,0,False)
        assert np.isfinite(value).all(),name
        aovs[name]=value
    lpes={}
    for index,(name,expression) in enumerate(lpe_definitions):
        value=np.empty_like(raw);film.GetOutputFloat(s.FilmOutputType.LPE,value,index,False)
        assert np.isfinite(value).all(),name
        lpes[name]=value
    elapsed=time.monotonic()-t;cpu_elapsed=time.process_time()-cpu_start
finally:
    if renderer.IsStarted():renderer.Stop()
assert np.isfinite(raw).all() and counts.min()==counts.max()==a.spp
np.save(out/'linear.npy',raw);np.save(out/'samples.npy',counts);np.save(out/'rays.npy',rays)
for name,value in aovs.items():np.save(out/('aov-'+name+'.npy'),value)
for name,value in lpes.items():np.save(out/('path-'+name+'.npy'),value)
assert hashlib.sha256(Path(__file__).read_bytes()).hexdigest()==benchmark_identity_before,'Benchmark source changed during render'
record=dict(comparison_contract='sss-unregularized-v2',authored_scene_sha256=authored_sha,benchmark_source_sha256=benchmark_identity_before,
            raw_controls=dict(denoising=False,adaptive_sampling=False),
            native_fidelity=checked,
            engine='SUPERLUXCORE',spp=a.spp,seed=a.seed,spectral=a.spectral,threads=a.threads,size=[width,height],
            tile_size_request=a.tile_size,
            seconds=elapsed,cpu_seconds=cpu_elapsed,preparation_seconds=preparation,raycount_buffer_mean=float(rays.mean(dtype=float)),native_sha256=identity,min_samples=int(counts.min()),
            max_samples=int(counts.max()),finite=True,mean_rgb=raw.mean(axis=(0,1),dtype=float).tolist(),
            timing_scope='Native config/preparation/tracing/film readback; excludes Blender, EXR and display',
            diagnostics={k:v for k,v in os.environ.items() if k.startswith('SUPERLUXCORE_SSS_')},
            scene=str(scene),pid=os.getpid())
if aov_names:
    record['aov_names']=aov_names
    record['aov_classification']='First camera lobe; not a per-collision or terminal-exit decomposition'
    record['timing_scope']+='; includes diagnostic AOV readback'
    total=sum((value.astype(np.float64) for value in aovs.values()),start=np.zeros_like(raw,dtype=np.float64))
    record['aov_reconstruction_max_abs']=float(np.max(abs(total-raw)))
    assert np.allclose(total,raw,rtol=5e-5,atol=1e-5),'AOVs do not reconstruct raw RGB'
if lpe_definitions:
    record['path_audit']=dict(lpe_definitions)
    record['timing_scope']+='; includes diagnostic LPE accumulation and readback'
    record['lpe_reconstruction_max_abs']=float(np.max(abs(lpes['all']-raw)))
    assert np.allclose(lpes['all'],raw,rtol=5e-5,atol=1e-5),'All-path LPE does not reconstruct RGB'
(out/'result.json').write_text(json.dumps(record,indent=2));print(json.dumps(record),flush=True)
