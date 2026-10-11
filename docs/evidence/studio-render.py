#!/usr/bin/env python3
"""Render studio-scenes.py output with actual native renderer + OCIO ACES 2.0."""
import argparse, ctypes, hashlib, importlib, json, os, re, sys, time
from pathlib import Path
import numpy as np
from PIL import Image
import PyOpenColorIO as ocio
P=argparse.ArgumentParser();P.add_argument('scene');P.add_argument('--work',required=True);P.add_argument('--lib',required=True);P.add_argument('--out',required=True);P.add_argument('--engine',choices=['superluxcore','upstream'],default='superluxcore');P.add_argument('--variant',default='beauty');P.add_argument('--spp',type=int,default=512);P.add_argument('--width',type=int,default=1920);P.add_argument('--height',type=int,default=1200);P.add_argument('--exposure',type=float,default=0);P.add_argument('--deadline',type=int,default=1200);P.add_argument('--threads',type=int,default=8);P.add_argument('--sampler',choices=['SOBOL','METROPOLIS']);P.add_argument('--backend',choices=['PATHCPU','BIDIRCPU','PATHOCL']);P.add_argument('--spectral',type=int,choices=[0,1]);P.add_argument('--raw-cache');A=P.parse_args()
work=Path(A.work)/A.scene;source=(work/'scene.scn').read_text();base_sha=hashlib.sha256(source.encode()).hexdigest();overrides=[]
if A.engine=='upstream' or A.variant=='baseline':
    if A.scene=='music':
        overrides=['scene.materials.cd.type = metal2','scene.materials.cd.preset = silver','scene.materials.cd.uroughness = .026','scene.materials.cd.vroughness = .026']
    if A.scene=='audio':
        names=re.findall(r'^scene\.materials\.(brass_goblets_0[13](?:_\d+)?)\.type = disney$',source,re.M)
        assert len(names)==2,names
        overrides=[f'scene.materials.{name}.multibounce = 0' for name in names]
    if A.scene=='perfume':overrides=['scene.materials.bottle.type = disney','scene.materials.bottle.basecolor = 1 1 1','scene.materials.bottle.roughness = .02','scene.materials.bottle.specular = .5']
    if A.scene=='textile':overrides=['scene.materials.fur.type = glossy2','scene.materials.fur.kd = .63 .40 .18','scene.materials.fur.ks = .12 .12 .12','scene.materials.fur.uroughness = .36','scene.materials.fur.vroughness = .36']
if A.variant=='sss-off':
    overrides=['scene.materials.croissant.subsurfaceweight = 0','scene.materials.tea_set_01.subsurfaceweight = 0']
if A.variant=='film-off':overrides=['scene.materials.bubble.filmthickness = 0']
if A.variant=='glass':overrides=['scene.materials.bottle.type = glass','scene.materials.bottle.kr = 1 1 1','scene.materials.bottle.kt = 1 1 1','scene.materials.bottle.interiorior = 1.48']
source+='\n'+'\n'.join(overrides)+'\n'
sys.path.insert(0,A.lib);lux=importlib.import_module('pyluxcore' if A.engine=='upstream' else 'pysuperluxcore');lux.Init();lux.SetLogHandler(None)  # Keep progress/errors, avoid verbose native teardown logs.
native=next(Path(A.lib).glob(('pyluxcore' if A.engine=='upstream' else 'pysuperluxcore')+'/*.so'))
if A.engine=='upstream'or A.backend=='PATHOCL':ctypes.c_bool.in_dll(ctypes.CDLL(str(native)),'_ZN7luxrays16isOpenCLAvilableE').value=False
p=lux.Properties();p.SetFromString(source);sc=lux.Scene();sc.Parse(p)
backend=A.backend or 'PATHCPU';sampler=A.sampler or ('METROPOLIS' if backend=='BIDIRCPU' else 'SOBOL')
config=f'''film.width = {A.width}
film.height = {A.height}
film.noiseestimation.warmup = 100000
film.noiseestimation.step = 100000
renderengine.type = {backend}
native.threads.count = {A.threads}
sampler.type = {sampler}
renderengine.seed = 42
path.pathdepth.total = 12
path.maxdepth = 12
light.maxdepth = 12
path.russianroulette.depth = 6
path.spectral.enable = {A.spectral if A.spectral is not None else 0}
path.mnee.auto = 0
path.regularization.auto = 0
path.lighttracing.enable = 0
film.imagepipelines.0.0.type = INTEL_OIDN
film.imagepipelines.0.0.sharpness = 0
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.filename = ignored.png
film.outputs.1.type = OBJECT_ID
film.outputs.1.filename = ignored-id.png
film.outputs.2.type = MATERIAL_ID
film.outputs.2.filename = ignored-material.png
film.outputs.3.type = ALBEDO
film.outputs.3.filename = ignored-albedo.exr
film.outputs.4.type = AVG_SHADING_NORMAL
film.outputs.4.filename = ignored-normal.exr
'''
if backend=='PATHOCL':config+='opencl.devices.select = 10\nopencl.cpu.use = 0\nopencl.gpu.use = 1\n'
if A.engine=='superluxcore' and A.scene=='interior':
    config+='film.outputs.5.type = CRYPTOMATTE_OBJECT\nfilm.outputs.5.filename = ignored-crypto.exr\nfilm.outputs.6.type = CRYPTOMATTE_MATERIAL\nfilm.outputs.6.filename = ignored-crypto-material.exr\n'
if backend=='PATHCPU' and A.scene in ['perfume','prism','architecture']:
    config+='path.hybridbackforward.enable = 1\npath.hybridbackforward.partition = .8\npath.hybridbackforward.adaptivecaustic = 1\n'
region=None;sampled_pixels=A.width*A.height
c=lux.Properties();c.SetFromString(config);session=lux.RenderSession(lux.RenderConfig(c,sc));start=time.monotonic();session.Start();last=-1
cfgpath=Path('/Applications/Blender.app/Contents/Resources/5.2/datafiles/colormanagement/config.ocio');cfg=ocio.Config.CreateFromFile(str(cfgpath));cpu=cfg.getProcessor(ocio.DisplayViewTransform(src='Linear Rec.709',display='sRGB',view='ACES 2.0')).getDefaultCPUProcessor()
def save_rgb(rgb,path,display=True):
    rgb=rgb.reshape(A.height,A.width,3)[::-1].copy()
    if display:rgb*=np.float32(2**A.exposure);cpu.applyRGB(rgb)
    Image.fromarray(np.uint8(np.clip(rgb,0,1)*255+.5)).save(path,quality=97,subsampling=0)
try:
    while True:
        session.UpdateStats();spp=int(session.GetStats().Get('stats.renderengine.total.samplecount').GetDouble()/sampled_pixels) if region else session.GetStats().Get('stats.renderengine.pass').GetInt();elapsed=time.monotonic()-start
        if spp>=A.spp:break
        if elapsed>A.deadline:raise TimeoutError(f'{A.scene} {A.variant}: {spp}/{A.spp} after {elapsed:.1f}s')
        if int(elapsed/20)!=last:last=int(elapsed/20);print(f'PROGRESS {A.scene} {A.variant} {spp}/{A.spp} {elapsed:.0f}s',flush=True)
        time.sleep(.4)
    session.Pause();session.UpdateStats();film=session.GetFilm();raw=np.empty(A.width*A.height*3,np.float32);rgb=raw.copy()
    film.GetOutputFloat(lux.FilmOutputType.RGB,raw,0,True);film.GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE,rgb,0,True);assert np.isfinite(raw).all()and np.isfinite(rgb).all()
    out=Path(A.out);out.parent.mkdir(parents=True,exist_ok=True);save_rgb(rgb,out)
    extras=[]
    if A.width>=1280 and A.variant=='beauty' and A.engine=='superluxcore':
        # Actual renderer outputs. These are marked as technical passes, not beauty renders.
        save_rgb(raw,out.with_name(out.stem+'-raw.jpg'));extras.append(out.stem+'-raw.jpg')
        for typ,label in [('AVG_SHADING_NORMAL','normal'),('ALBEDO','albedo')]:
            data=np.empty(A.width*A.height*3,np.float32);film.GetOutputFloat(getattr(lux.FilmOutputType,typ),data,0,True)
            if label=='normal':data=data*.5+.5
            save_rgb(data,out.with_name(out.stem+'-'+label+'.jpg'),display=False);extras.append(out.stem+'-'+label+'.jpg')
        for typ,label in [('OBJECT_ID','objects'),('MATERIAL_ID','materials')]:
            ids=np.empty(A.width*A.height,np.uint32);film.GetOutputUInt(getattr(lux.FilmOutputType,typ),ids,0,True)
            col=np.stack([((ids*73+61)%251)/255,((ids*137+19)%251)/255,((ids*191+83)%251)/255],1).astype(np.float32);col[ids==0xffffffff]=0
            save_rgb(col,out.with_name(out.stem+'-'+label+'.jpg'),display=False);extras.append(out.stem+'-'+label+'.jpg')
        if A.scene=='interior':
            for typ,label in [('CRYPTOMATTE_OBJECT','crypto-objects'),('CRYPTOMATTE_MATERIAL','crypto-materials')]:
                data=np.empty(A.width*A.height*12,np.float32);film.GetOutputFloat(getattr(lux.FilmOutputType,typ),data,0,True);pairs=data.reshape(-1,6,2);ids=pairs[:,:,0].copy().view(np.uint32);coverage=pairs[:,:,1]
                colors=np.stack([((ids*73+61)%251)/255,((ids*137+19)%251)/255,((ids*191+83)%251)/255],-1).astype(np.float32);col=(colors*coverage[:,:,None]).sum(1)
                save_rgb(col,out.with_name(out.stem+'-'+label+'.jpg'),display=False);extras.append(out.stem+'-'+label+'.jpg')
    record={'scene':A.scene,'variant':A.variant,'engine':A.engine,'version':lux.Version(),'backend':backend,'sampler':sampler,'seed':42,'resolution':[A.width,A.height],'target_spp':A.spp,'observed_spp':spp,'render_seconds':round(time.monotonic()-start,2),'native_sha256':hashlib.sha256(native.read_bytes()).hexdigest(),'base_scene_sha256':base_sha,'scene_sha256':hashlib.sha256(source.encode()).hexdigest(),'image_sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'raw_finite':True,'raw_max':float(raw.max()),'overrides':overrides,'configuration':config,'display':{'input':'Linear Rec.709','display':'sRGB','view':'ACES 2.0','ocio_version':ocio.__version__,'config_sha256':hashlib.sha256(cfgpath.read_bytes()).hexdigest(),'exposure':A.exposure,'per_image_normalization':False},'denoise':'Native OIDN with renderer ALBEDO and AVG_SHADING_NORMAL auxiliaries','technical_outputs':extras,'upstream_workaround':'Optional OpenCL discovery disabled in memory; native renderer bytes unmodified' if A.engine=='upstream'or backend=='PATHOCL'else None}
    if region:record['rendered_region_top_left']=region;record['spp_normalization']='Native total sample count divided by active region pixels; full-frame camera and 64-pixel denoiser guard band'
    if A.raw_cache:np.savez_compressed(A.raw_cache,raw=raw.reshape(A.height,A.width,3),denoised=rgb.reshape(A.height,A.width,3))
    out.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n');print('RENDER_COMPLETE',A.scene,A.variant,out,spp,flush=True)
finally:session.Stop()
