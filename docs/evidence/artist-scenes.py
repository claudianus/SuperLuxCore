#!/usr/bin/env python3
"""Original procedural art direction; actual renderer output + OCIO ACES 2.0.

Requires CPython 3.13, NumPy, Pillow, OpenColorIO 2.6 and an extracted official
renderer wheel. No stock render, generative image or baked lighting is used.
Render each engine in a separate process. Assets are generated in --work.
"""
import argparse, ctypes, hashlib, importlib, json, math, os, struct, time
from pathlib import Path
import numpy as np
from PIL import Image
import PyOpenColorIO as ocio

parser=argparse.ArgumentParser()
parser.add_argument('scene',choices=['metals','transmission','orbs','prism','caustics','fur','godrays','spinner','spectral'])
parser.add_argument('--engine',default='superluxcore',choices=['superluxcore','upstream'])
parser.add_argument('--lib',required=True);parser.add_argument('--work',required=True)
parser.add_argument('--out',required=True);parser.add_argument('--spp',type=int,default=768)
parser.add_argument('--width',type=int,default=1600);parser.add_argument('--height',type=int,default=900)
parser.add_argument('--exposure',type=float,default=0);parser.add_argument('--spectral',type=int,default=0);parser.add_argument('--preview',action='store_true')
parser.add_argument('--ocio',default='/Applications/Blender.app/Contents/Resources/5.2/datafiles/colormanagement/config.ocio')
A=parser.parse_args();work=Path(A.work).resolve();work.mkdir(parents=True,exist_ok=True)
def normalize(v):return v/np.maximum(np.linalg.norm(v,axis=-1,keepdims=True),1e-12)
def ply(name,vertices,faces,smooth=True):
    path=work/(name+'.ply')
    if path.exists():return str(path)
    v=np.asarray(vertices,dtype=np.float32);f=np.asarray(faces,dtype=np.int32)
    n=np.zeros_like(v)
    if smooth:
        fn=np.cross(v[f[:,1]]-v[f[:,0]],v[f[:,2]]-v[f[:,0]])
        for j in range(3):np.add.at(n,f[:,j],fn)
        n=normalize(n)
    with path.open('w') as h:
        h.write(f'ply\nformat ascii 1.0\nelement vertex {len(v)}\nproperty float x\nproperty float y\nproperty float z\n')
        if smooth:h.write('property float nx\nproperty float ny\nproperty float nz\n')
        h.write(f'element face {len(f)}\nproperty list uchar int vertex_indices\nend_header\n')
        np.savetxt(h,np.concatenate([v,n],1) if smooth else v,fmt='%.7g')
        for face in f:h.write('3 '+' '.join(map(str,face))+'\n')
    return str(path)
def grid_faces(rows,cols):
    f=[]
    for i in range(rows):
        for j in range(cols):
            a=i*cols+j;b=i*cols+(j+1)%cols;c=((i+1)%rows)*cols+(j+1)%cols;d=((i+1)%rows)*cols+j
            f.extend([(a,b,c),(a,c,d)])
    return f
def sphere(name,center,radius,nu=80,nv=40):
    v=[];f=[]
    for i in range(nv+1):
        t=math.pi*i/nv
        for j in range(nu):
            u=2*math.pi*j/nu;v.append(np.array(center)+radius*np.array([math.sin(t)*math.cos(u),math.sin(t)*math.sin(u),math.cos(t)]))
    for i in range(nv):
        for j in range(nu):
            a=i*nu+j;b=i*nu+(j+1)%nu;c=(i+1)*nu+(j+1)%nu;d=(i+1)*nu+j;f.extend([(a,c,b),(a,d,c)])
    return ply(name,v,f)
def lathe(name,center,profile,steps=120):
    v=[]
    for r,z in profile:
        for i in range(steps):
            t=i/steps*2*math.pi;v.append([center[0]+r*math.cos(t),center[1]+r*math.sin(t),center[2]+z])
    f=[]
    for k in range(len(profile)-1):
        for j in range(steps):
            a=k*steps+j;b=k*steps+(j+1)%steps;c=(k+1)*steps+(j+1)%steps;d=(k+1)*steps+j;f.extend([(a,b,c),(a,c,d)])
    return ply(name,v,f)
def torus(name,center,R=.85,r=.18,tilt=0):
    u=np.arange(192)/192*math.tau;v=np.arange(32)/32*math.tau
    x=(R+r*np.cos(v)[None,:])*np.cos(u)[:,None];y=(R+r*np.cos(v)[None,:])*np.sin(u)[:,None];z=np.broadcast_to(r*np.sin(v)[None,:],x.shape)
    points=np.stack([x,y*np.cos(tilt)-z*np.sin(tilt),y*np.sin(tilt)+z*np.cos(tilt)],-1).reshape(-1,3)+center
    return ply(name,points,[tuple(reversed(f)) for f in grid_faces(192,32)])
def knot(name,center,scale=.5,tube=.16,angle=0):
    t=np.arange(256)/256*math.tau
    p=np.stack([(2+np.cos(3*t))*np.cos(2*t),np.sin(3*t),(2+np.cos(3*t))*np.sin(2*t)],-1)*scale
    tangent=normalize(np.roll(p,-1,0)-np.roll(p,1,0));n=normalize(np.cross(tangent,[0,1,0]));b=normalize(np.cross(tangent,n))
    u=np.arange(32)/32*math.tau;v=p[:,None,:]+tube*(n[:,None,:]*np.cos(u)[None,:,None]+b[:,None,:]*np.sin(u)[None,:,None])
    v=v.reshape(-1,3);v[:,[0,1]]=v[:,[0,1]]@np.array([[math.cos(angle),math.sin(angle)],[-math.sin(angle),math.cos(angle)]])
    return ply(name,v+center,grid_faces(256,32))
def quad(name,center,size,target=None):
    c=np.asarray(center,dtype=float);n=normalize(np.asarray(target,dtype=float)-c) if target is not None else np.array([0.,0.,1.])
    u=normalize(np.cross([0,0,1],n)) if abs(n[2])<.95 else np.array([1.,0.,0.]);v=np.cross(n,u)
    pts=[c+sx*u*size[0]/2+sy*v*size[1]/2 for sx,sy in [(-1,-1),(1,-1),(1,1),(-1,1)]]
    return ply(name,pts,[(0,1,2),(0,2,3)],False)
def cube(name,lo,hi):
    v=[[x,y,z]for z in [lo[2],hi[2]]for y in [lo[1],hi[1]]for x in [lo[0],hi[0]]]
    q=[(0,2,3,1),(4,5,7,6),(0,1,5,4),(2,6,7,3),(0,4,6,2),(1,3,7,5)]
    return ply(name,v,[(a,b,c)for a,b,c,d in q]+[(a,c,d)for a,b,c,d in q],False)
S=[]
def prop(key,value):S.append(key+' = '+str(value))
def mat(name,kind,**kw):
    prop(f'scene.materials.{name}.type',kind)
    for k,v in kw.items():prop(f'scene.materials.{name}.{k}',v)
def obj(name,material,mesh):prop(f'scene.objects.{name}.material',material);prop(f'scene.objects.{name}.ply',mesh)
def light(name,center,size,color,target=(0,0,1)):
    mat(name,'matte',kd='0 0 0',emission=color);obj(name,name,quad(name,center,size,target))
    if A.scene!='metals':prop(f'scene.objects.{name}.camerainvisible',1)
def pedestal(name,center,r=.78,h=.22):
    obj(name,'stone',lathe(name,center,[(0,0),(r-.04,0),(r,.04),(r,h-.04),(r-.04,h),(0,h)]))
def camera(origin,target,fov=38,lens=0):
    prop('scene.camera.lookat.orig',' '.join(map(str,origin)));prop('scene.camera.lookat.target',' '.join(map(str,target)));prop('scene.camera.fieldofview',fov)
    if lens:prop('scene.camera.lensradius',lens);prop('scene.camera.focaldistance',np.linalg.norm(np.asarray(origin)-target))
def studio(wide=False):
    camera((7.4,-12.5,5.4) if wide else (4.4,-7.7,3.6),(0,0,1.35),38 if wide else 36)
    mat('floor','disney',basecolor='.035 .045 .055',metallic=.15,roughness=.27,specular=.55)
    mat('stone','disney',basecolor='.035 .04 .052',roughness=.33,metallic=.18)
    mat('gold','metal2',preset='gold',uroughness=.13,vroughness=.13)
    mat('chrome','metal2',preset='silver',uroughness=.085,vroughness=.085)
    obj('floor','floor',quad('floor',(0,0,-.04),(80,80)))
    light('key',(-3.5,-3,6),(3.5,4.5),'11 9 6.8')
    light('rim',(3,1,4),(1.0,4),'2.5 11 16')
    light('strip',(-4,3,2.5),(.45,4),'14 4.0 1.4')
    prop('scene.lights.world.type','constantinfinite');prop('scene.lights.world.color','.025 .035 .05')
def glass(name,color='1 1 1',sellmeier=False):
    mat(name,'glass',kr='.98 .98 .98',kt=color,interiorior=1.52)
    if sellmeier:prop(f'scene.materials.{name}.sellmeier','N-SF10')
def hair_asset():
    path=work/'halo.hair'
    if path.exists():return str(path)
    rng=np.random.default_rng(742);count=7000;segs=8;pts=[];th=[]
    for i in range(count):
        u,v=rng.uniform(0,math.tau,2);R=.72;r=.26
        base=np.array([(R+r*math.cos(v))*math.cos(u),r*math.sin(v),(R+r*math.cos(v))*math.sin(u)+1.4])
        normal=np.array([math.cos(v)*math.cos(u),math.sin(v),math.cos(v)*math.sin(u)]);tan=np.array([-math.sin(u),0,math.cos(u)])
        length=rng.uniform(.13,.28)
        for j in range(segs+1):
            t=j/segs;pts.append(base+normal*(length*t)+tan*(.17*t*t)+[0,.05*t*t,-.07*t*t]);th.append(.005*(1-.94*t))
    header=struct.pack('<4s4I5f88s',b'HAIR',count,len(pts),6,segs,.005,0,1,1,1,b'Original SuperLuxCore homepage fiber halo')
    path.write_bytes(header+np.asarray(pts,dtype='<f4').tobytes()+np.asarray(th,dtype='<f4').tobytes());return str(path)
studio(A.scene=='metals')
if A.scene=='metals':
    for i,(x,col,rough) in enumerate([(-2.2,'.87 .55 .2',.2),(0,'.73 .78 .83',.55),(2.2,'.82 .31 .12',.82)]):
        name=f'metal{i}';mat(name,'disney',basecolor=col,metallic=1,roughness=rough,specular=.5)
        if A.engine=='superluxcore':prop(f'scene.materials.{name}.multibounce',1)
        pedestal('base'+str(i),(x,0,0));obj('sculpture'+str(i),name,knot('metalshape'+str(i),(x,0,1.45),.38,.145,(-.1+i*.16)))
elif A.scene=='transmission':
    camera((4,-7.7,3.4),(0,0,1.25),35)
    for i,(x,color)in enumerate([(-1.25,'.86 .97 1'),(1.05,'1 .58 .3')]):
        name=f'disney{i}';mat(name,'disney',basecolor=color,metallic=0,roughness=.08,specular=.5)
        if A.engine=='superluxcore':prop(f'scene.materials.{name}.transmission',1);prop(f'scene.materials.{name}.ior',1.45)
        pedestal('base'+str(i),(x,0,0),.85,.24);obj('sculpture'+str(i),name,knot('transmission'+str(i),(x,0,1.45),.38,.155,.2-i*.4))
    for j in range(9):
        mat(f'band{j}','matte',kd='.16 .25 .3',emission=f'{.4+j*.2} {.9+j*.1} 1.4');obj(f'band{j}',f'band{j}',quad(f'band{j}',(-2.5+j*.6,2.4,1.6),(.12,2.9),(0,-1,1.6)))
elif A.scene=='orbs':
    camera((4.8,-8.7,3.7),(0,0,1.55),36,.045);pedestal('base',(0,0,0),1.32,.18)
    obj('braid','chrome',knot('hero-knot',(0,0,1.48),.46,.20,.18))
    mat('jade','openpbr',basecolor='.025 .3 .19',basemetalness=0,specularroughness=.18,subsurfaceweight=.65,subsurfacecolor='.08 .6 .34',subsurfaceradius=.13,subsurfaceradiusscale='1 .55 .3')
    obj('jade','jade',sphere('jade-orb',(.12,-.28,1.36),.54));obj('gold-ring','gold',torus('hero-ring',(0,0,1.48),1.5,.055,math.pi/2))
    rng=np.random.default_rng(34)
    for i in range(30):
        mat(f'bokeh{i}','matte',kd='0 0 0',emission='24 10 3.5' if i%2 else '5 15 24')
        obj(f'bokeh{i}',f'bokeh{i}',sphere(f'bokeh{i}',(rng.uniform(-4,4),rng.uniform(3.5,6),rng.uniform(.3,4)),rng.uniform(.025,.065),24,12))
elif A.scene in ('prism','caustics'):
    glass('crystal',sellmeier=A.scene=='prism');glass('amber','.98 .83 .5')
    prop('scene.materials.stone.basecolor','.24 .27 .31');prop('scene.materials.floor.basecolor','.11 .14 .18')
    light('optical-fill',(0,4,4.5),(4,3),'6 9 14',(0,0,1))
    camera((4.2,-7.5,4.1),(0,0,.85),37);pedestal('base',(0,0,0),1.5,.14)
    if A.scene=='prism':
        for i,(x,y,h) in enumerate([(-.8,.35,2.1),(.25,.05,2.8),(.95,.7,1.6)]):
            verts=[(x+dx,y+dy,z)for z in [.17,.17+h]for dx,dy in [(-.4,-.3),(.4,-.3),(0,.45)]]
            faces=[(0,2,1),(3,4,5),(0,1,4),(0,4,3),(1,2,5),(1,5,4),(2,0,3),(2,3,5)]
            obj('prism'+str(i),'crystal',ply('crystal-spire'+str(i),verts,faces,False))
        obj('orbit','gold',torus('prism-orbit',(0,.4,1.55),1.7,.04,math.pi/2))
    else:
        obj('glass-knot','crystal',knot('caustic-knot',(0,0,1.26),.4,.20,.2));obj('amber','amber',sphere('amber',(-1.35,-.25,.46),.29))
    prop('scene.lights.focus.type','spot');prop('scene.lights.focus.position','-1.7 -1.2 5');prop('scene.lights.focus.target','0 .8 0');prop('scene.lights.focus.coneangle',28);prop('scene.lights.focus.conedeltaangle',2);prop('scene.lights.focus.gain','180 170 150')
elif A.scene=='fur':
    camera((3.7,-7.2,3.15),(0,0,1.15),34);pedestal('base',(0,0,0),1.2,.28)
    mat('fur','hairmat',model='huang',eumelanin=.18,pheomelanin=.2,eta=1.55,beta_m=.28,beta_n=.35,alpha=4)
    prop('scene.shapes.fur.type','strands');prop('scene.shapes.fur.file',hair_asset());prop('scene.shapes.fur.tessellation.type','solid');prop('scene.shapes.fur.tessellation.solid.sidecount',5)
    prop('scene.objects.fur.material','fur');prop('scene.objects.fur.shape','fur')
    obj('core','gold',torus('furcore',(0,0,1.4),.72,.23,math.pi/2))
elif A.scene=='godrays':
    camera((5,-9,4),(0,1,1.5),42)
    for name in ('key','rim','strip'):prop(f'scene.materials.{name}.emission','.04 .04 .04')
    prop('scene.lights.world.color','.004 .005 .008')
    prop('scene.volumes.fog.type','homogeneous');prop('scene.volumes.fog.absorption','.002 .003 .004');prop('scene.volumes.fog.scattering','.02 .02 .02');prop('scene.volumes.fog.asymmetry','.25 .25 .25');prop('scene.volumes.fog.multiscattering',1)
    mat('fogboundary','null');prop('scene.materials.fogboundary.volume.interior','fog');obj('fog','fogboundary',cube('fog',(-6,-4,0),(6,8,7)))
    for i in range(3):
        x=(i-1)*1.8;obj('arch'+str(i),'chrome',torus('arch'+str(i),(x,1,1.6),1.1,.10,math.pi/2))
        prop(f'scene.lights.beam{i}.type','spot');prop(f'scene.lights.beam{i}.position',f'{x-1.1} 1.5 5.8');prop(f'scene.lights.beam{i}.target',f'{x+.6} .5 0');prop(f'scene.lights.beam{i}.coneangle',13);prop(f'scene.lights.beam{i}.conedeltaangle',1.5);prop(f'scene.lights.beam{i}.gain', ['420 110 30','90 380 520','270 70 470'][i])
elif A.scene=='spinner':
    camera((4.4,-7.7,3.5),(0,0,1.3),35)
    pedestal('base',(0,0,0),1.05,.2)
    obj('sculpture','gold',knot('motion-knot',(0,0,0),.44,.13))
    for i,angle in enumerate([-.18,.18]):
        c,s=math.cos(angle),math.sin(angle);matrix=f'{c} {s} 0 0 {-s} {c} 0 0 0 0 1 0 0 0 1.6 1'
        prop(f'scene.objects.sculpture.motion.{i}.time',i);prop(f'scene.objects.sculpture.motion.{i}.transformation',matrix)
    prop('scene.camera.shutteropen',0);prop('scene.camera.shutterclose',1)
    obj('orbit','chrome',torus('kinetic-orbit',(0,0,1.6),1.55,.04,math.pi/2))
elif A.scene=='spectral':
    camera((3.4,-5.5,4.4),(0,0,.38),35)
    pedestal('base',(0,0,0),1.42,.27)
    mat('grating','diffraction',kr='.96 .96 .96',spacing=1600,orientation='radial',center='0 0 .29',roughness=.02,fillfactor=.5,blaze=0,orders=8)
    obj('disc','grating',lathe('cd-disc',(0,0,0),[(1.3,.29),(.19,.29)],180))
    obj('rim','gold',torus('cd-rim',(0,0,.28),1.32,.025))
    light('spectral-key',(-2.2,-3.2,5),(.18,3.5),'55 55 55')
    light('spectral-strip',(2.5,2.7,4),(.12,2.5),'30 30 30')

scene='\n'.join(S)+'\n';(work/(A.scene+'-'+A.engine+'.scn')).write_text(scene)
import sys
sys.path.insert(0,A.lib);lux=importlib.import_module('pyluxcore' if A.engine=='upstream' else 'pysuperluxcore');lux.Init()
module=next(Path(A.lib).glob(('pyluxcore' if A.engine=='upstream' else 'pysuperluxcore')+'/*.so'))
if A.engine=='upstream':ctypes.c_bool.in_dll(ctypes.CDLL(str(module)), '_ZN7luxrays16isOpenCLAvilableE').value=False
p=lux.Properties();p.SetFromString(scene);sc=lux.Scene();sc.Parse(p)
engine='BIDIRCPU' if A.scene in ('prism','caustics') else 'PATHCPU';sampler='METROPOLIS' if engine=='BIDIRCPU' else 'SOBOL'
config=f'''film.width = {A.width}
film.height = {A.height}
renderengine.type = {engine}
native.threads.count = 8
sampler.type = {sampler}
renderengine.seed = 42
batch.haltspp = {A.spp}
path.pathdepth.total = 10
path.spectral.enable = {A.spectral}
path.regularization.auto = 0
path.lighttracing.enable = 0
path.mnee.auto = 0
film.imagepipelines.0.0.type = {'NOP' if A.preview else 'INTEL_OIDN'}
film.imagepipelines.0.0.sharpness = 0
film.imagepipelines.0.0.prefilter.enable = 1
'''
c=lux.Properties();c.SetFromString(config);s=lux.RenderSession(lux.RenderConfig(c,sc));start=time.monotonic();s.Start()
try:
    while True:
        s.UpdateStats();spp=s.GetStats().Get('stats.renderengine.pass').GetInt()
        if spp>=A.spp:break
        if time.monotonic()-start>600:raise TimeoutError(f'{A.scene}: {spp}/{A.spp}')
        if int(time.monotonic()-start)%15==0:print(f'PROGRESS {A.scene} {spp}/{A.spp}',flush=True)
        time.sleep(.5)
    s.Pause();raw=np.empty(A.width*A.height*3,dtype=np.float32);rgb=raw.copy()
    s.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB,raw,0,True)
    s.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE,rgb,0,True)
    assert np.isfinite(raw).all() and np.isfinite(rgb).all()
    rgb=rgb.reshape(A.height,A.width,3)[::-1].copy()*np.float32(2**A.exposure)
    cfg=ocio.Config.CreateFromFile(A.ocio);transform=ocio.DisplayViewTransform(src='Linear Rec.709',display='sRGB',view='ACES 2.0')
    processor=cfg.getProcessor(transform).getDefaultCPUProcessor();processor.applyRGB(rgb)
    output=Path(A.out);output.parent.mkdir(parents=True,exist_ok=True)
    Image.fromarray(np.uint8(np.clip(rgb,0,1)*255+.5)).save(output,quality=96,subsampling=0)
    metadata={'scene':A.scene,'engine':A.engine,'version':lux.Version(),'backend':engine,'sampler':sampler,'seed':42,'hardware':'Apple M5 Pro / CPU 8 threads','resolution':[A.width,A.height],'target_spp':A.spp,'observed_spp':spp,'spectral':bool(A.spectral),'native_sha256':hashlib.sha256(module.read_bytes()).hexdigest(),'scene_sha256':hashlib.sha256(scene.encode()).hexdigest(),'config':config,'render_seconds':round(time.monotonic()-start,3),'display':{'ocio_version':ocio.__version__,'config_sha256':hashlib.sha256(Path(A.ocio).read_bytes()).hexdigest(),'input':'Linear Rec.709','display':'sRGB','view':'ACES 2.0','exposure':A.exposure,'per_image_normalization':False},'denoising':'none' if A.preview else 'Renderer INTEL_OIDN, sharpness 0; auxiliary prefilter requested, ALBEDO AOV unavailable in this recipe','raw_rgb_sha256':hashlib.sha256(raw.tobytes()).hexdigest(),'raw_rgb_max':float(raw.max()),'raw_finite':bool(np.isfinite(raw).all()),'image_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'asset_origin':'New original procedural scene rendered on 2026-10-11; no previous website image reused','scope':'Art-directed feature illustration, not speed benchmark or final unbiased convergence evidence','upstream_workaround':'Optional OpenCL discovery disabled in memory; renderer bytes unchanged' if A.engine=='upstream' else None}
    output.with_suffix(output.suffix+'.json').write_text(json.dumps(metadata,indent=2)+'\n');print(json.dumps(metadata),flush=True)
finally:s.Stop()
