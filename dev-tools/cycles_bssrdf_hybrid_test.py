# SPDX-License-Identifier: Apache-2.0
"""Experimental CPU hybrid SSS against an independent, unsuppressed eye walk.

The 50 existing CPU contracts run first. No production/GPU acceptance follows
from these fixed-coefficient caustic-partition mean checks.
"""
import ast,hashlib,json,os,runpy,time
from pathlib import Path
import numpy as np
import pysuperluxcore as slc
repo=Path(__file__).resolve().parents[1]
root=Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])/'hybrid';root.mkdir(parents=True,exist_ok=True)
ns=runpy.run_path(str(repo/'dev-tools/cycles_bssrdf_transport_test.py'))
identity=ns['identity'];props,scene=ns['properties'],ns['scene']
slc=ns['slc'];W=H=32
records=[]
def record(case,**data):
    records.append({'case':case,'passed':True,**data})
    save(False)
    print('BSSRDF_HYBRID_PASS',case,data,flush=True)
def save(complete):
    (root/'hybrid-metrics.json').write_text(json.dumps({'native_sha256':identity['native_sha256'],'cpu_contracts':len(ns['records']),'production_acceptance':False,'gpu_hybrid_implemented':False,'complete':complete,'records':records},indent=2)+'\n')
base={'film.width':W,'film.height':H,'film.imagepipelines.0.0.type':'NOP','film.filter.type':'NONE','native.threads.count':8,'renderengine.seed':131,'sampler.type':'SOBOL','batch.haltspp':0,'path.cyclesbssrdf.experimental.enable':True,'path.cyclesbssrdf.experimental.adjoint.enable':True,'path.lighttracing.enable':False,'path.lighttracing.auto':False,'path.lighttracing.only':False,'path.mnee.enable':False,'path.mnee.auto':False,'path.regularization.auto':False,'path.regularization.sigma':0.,'path.vertexconnection.enable':False,'path.photongi.caustic.enabled':False,'path.photongi.indirect.enabled':False,'path.forceblackbackground.enable':True,'path.pathdepth.total':4,'film.outputs.0.type':'RGB','film.outputs.0.filename':'unused.exr','film.outputs.1.type':'MATERIAL_ID','film.outputs.1.filename':'unused.png'}
def body(values=None, caster=True):
    values=values or {}
    scn=scene(radius=values.get('radius',(1.,1.,1.)))
    # Parse replaces the whole material. Preserve every authored coefficient
    # and the material identity explicitly, including the intended Scale1.
    scn.Parse(props({'scene.materials.body.type':values.get('kind','cyclesbssrdf'),
       'scene.materials.body.kd':values.get('color',(.45,.45,.45)),
       'scene.materials.body.radius':values.get('radius',(1.,1.,1.)),
       'scene.materials.body.scale':1.,'scene.materials.body.ior':values.get('ior',1.4),
       'scene.materials.body.roughness':values.get('roughness',0.),
       'scene.materials.body.anisotropy':values.get('g',0.),'scene.materials.body.id':991}))
    if caster:
        scn.Parse(props({'scene.materials.mirror.type':'mirror','scene.materials.mirror.kr':1.,
         'scene.objects.mirror.material':'mirror','scene.objects.mirror.vertices':
         (-8.,1.5,-8.,8.,1.5,-8.,8.,1.5,8.,-8.,1.5,8.),
         'scene.objects.mirror.faces':(0,1,2,0,2,3),'scene.objects.mirror.id':999}))
    return scn

def reject(case,scn,overrides,phrase):
    try:slc.RenderSession(slc.RenderConfig(props(base|{'renderengine.type':'PATHCPU','path.hybridbackforward.enable':True}|overrides),scn))
    except RuntimeError as error:
        assert phrase in str(error),(case,str(error));record(case,message=str(error))
    else:raise AssertionError(case+' must reject before workers')

reject('hybrid-requires-adjoint-opt-in',body(),{'path.cyclesbssrdf.experimental.adjoint.enable':False},'Experimental cyclesbssrdf')
reject('hybrid-vertex-connection-stays-gated',body(),{'path.vertexconnection.enable':True},'vertex connections')
reject('hybrid-bidir-stays-gated',body(),{'renderengine.type':'BIDIRCPU'},'Experimental cyclesbssrdf')
reject('device-hybrid-stays-gated',body(),{'renderengine.type':'PATHOCL','path.cyclesbssrdf.experimental.device.enable':True},'Experimental cyclesbssrdf')
textured=body();textured.Parse(props({'scene.textures.pattern.type':'checkerboard3d','scene.textures.pattern.texture1':.45,'scene.textures.pattern.texture2':.65,'scene.materials.body.type':'cyclesbssrdf','scene.materials.body.kd':'pattern','scene.materials.body.id':991}))
reject('hybrid-validates-reverse-textured-kernel',textured,{},'textured coefficients')
mixed=scene(nested_mix=True)
reject('hybrid-validates-reverse-mixed-kernel',mixed,{},'adjoint mixed')
slc.RenderSession(slc.RenderConfig(props(base|{'renderengine.type':'PATHCPU','path.hybridbackforward.enable':False,'path.lighttracing.enable':True}),body()))
record('canonical-lighttracing-promotion-accepted')
def render(scn,label,hybrid,partition=.8,*,adaptive=True,spectral=False,seed=131,regularization=0.):
    config=base|{'renderengine.type':'PATHCPU','path.hybridbackforward.enable':hybrid,'path.hybridbackforward.partition':partition,'path.hybridbackforward.adaptivecaustic':adaptive,'path.spectral.enable':spectral,'renderengine.seed':seed,'path.regularization.sigma':regularization}
    (root/(label+'-config.cfg')).write_text(props(config).ToString())
    ses=slc.RenderSession(slc.RenderConfig(props(config),scn));ses.Start();start=time.monotonic()
    try:
        while True:
            ses.UpdateStats();stats=ses.GetStats();eye=stats.Get('stats.renderengine.pass.eye').GetInt();light=stats.Get('stats.renderengine.pass.light').GetInt()
            if eye>=16384 and (not hybrid or partition==1. or light>=20480):break
            assert time.monotonic()-start<600,(label,eye,light)
            time.sleep(.1)
    finally:ses.Stop()
    rgb=np.empty(W*H*3,np.float32);ids=np.empty(W*H,np.uint32)
    ses.GetFilm().GetOutputFloat(slc.FilmOutputType.RGB,rgb,0,True);ses.GetFilm().GetOutputUInt(slc.FilmOutputType.MATERIAL_ID,ids,0,True)
    rgb=rgb.reshape(H,W,3);ids=ids.reshape(H,W);np.savez_compressed(root/(label+'.npz'),RGB=rgb,MATERIAL_ID=ids)
    assert np.isfinite(rgb).all()
    print('BSSRDF_HYBRID_RENDER',label,eye,light,'seconds',time.monotonic()-start,flush=True)
    return rgb,ids,eye,light
for case,values,caster,adaptive,spectral,seed in (
    ('ordinary-light-sharp',{},False,True,False,131),
    ('mirror-sharp',{},True,True,False,131),
    ('mirror-sharp-legacy-partition',{},True,False,False,131),
    ('mirror-rough',{'roughness':.4},True,True,False,131),
    ('mirror-sharp-psr',{'regularization':.03},True,True,False,131),
    ('mirror-rough-psr',{'roughness':.4,'regularization':.03},True,True,False,131),
    ('mirror-ordinary-matte-psr',{'kind':'matte','regularization':.03},True,True,False,131),
    ('mirror-colored-rgb',{'color':(.55,.2,.08),'radius':(1.,.3,.2)},True,True,False,131),
    ('mirror-colored-spectral',{'color':(.55,.2,.08),'radius':(1.,.3,.2)},True,True,True,131),
    ('mirror-partial-local',{'radius':(0.,1.,1.)},True,True,False,131),
    ('mirror-all-local',{'radius':(0.,0.,0.)},True,True,False,131),
    ('mirror-sharp-independent-seed',{},True,True,False,817)):
    scn=body(values,caster)
    (root/(case+'-scene.scn')).write_text(scn.ToProperties().ToString())
    outputs={}
    modes=[('eye',False,.8),('hybrid',True,.8)]
    if case=='mirror-sharp':modes.insert(1,('eye-hole',True,1.))
    for label,enabled,p in modes:
        outputs[label]=render(scn,case+'-'+label,enabled,p,adaptive=adaptive,spectral=spectral,seed=seed,regularization=values.get('regularization',0.))
    mask=outputs['eye'][1]==991;m=mask.copy()
    for dy in (-1,0,1):
        for dx in (-1,0,1):m&=np.roll(np.roll(mask,dy,0),dx,1)
    assert int(m.sum())>=100,(case,np.unique(outputs['eye'][1]))
    means={label:data[0][m].mean(axis=0).tolist() for label,data in outputs.items()}
    assert all(np.isfinite(v).all() for v in means.values()),(case,means)
    error=float(np.max(np.abs(np.array(means['hybrid'])-means['eye'])/np.maximum(means['eye'],1e-4)))
    detail={'pixels':int(m.sum()),'seed':seed,'adaptive':adaptive,'spectral':spectral,'regularization':values.get('regularization',0.),'means':means,'max_relative_channel_error':error,'sample_counts':{label:{'eye':data[2],'light':data[3]} for label,data in outputs.items()}}
    (root/(case+'-comparison.json')).write_text(json.dumps(detail,indent=2)+'\n')
    if 'eye-hole' in means:
        loss=1.-float(np.mean(means['eye-hole'])/np.mean(means['eye']))
        assert loss>.1,(case,loss);detail['verified_omitted_caustic_fraction']=loss
    assert error<.03,(case,error,means)
    record(case,**detail)
save(True)
print('BSSRDF_HYBRID_COMPLETE',len(records),flush=True)
