#!/usr/bin/env python3
"""Photographic asset scenes. Run Blender -b --python this.py -- --work DIR.

Blender stages credited CC0 scanned models and photograph-derived PBR maps;
studio-render.py renders every
pixel with the released SuperLuxCore / LuxCoreRender native CPU or Metal libraries.
No stock beauty render, generated illustration, or baked lighting is used.
"""
import argparse, json, math, re, struct, sys
from pathlib import Path
import bpy
import numpy as np
from mathutils import Vector, Matrix

P=argparse.ArgumentParser();P.add_argument('--work',required=True);P.add_argument('--only',default='')
A=P.parse_args(sys.argv[sys.argv.index('--')+1:]);ROOT=Path(A.work).resolve();ROOT.mkdir(parents=True,exist_ok=True)
TAU=math.tau; rng=np.random.default_rng(117)
S=[];objects=[];scene='';W=None; camera_data={}
def prop(k,v):S.append(k+' = '+(' '.join(map(str,v)) if isinstance(v,(tuple,list,np.ndarray)) else str(v)))
def mat(n,t='disney',**kw):
    prop('scene.materials.'+n+'.type',t)
    for k,v in kw.items():prop('scene.materials.'+n+'.'+k,v)
def assign(o,m):
    o.name=re.sub(r'[^A-Za-z0-9_-]','_',o.name);o['slc_material']=m;objects.append(o);return o
def finish(o,n,m,bevel=0,smooth=True):
    o.name=n
    if bevel:
        mod=o.modifiers.new('Machined edge','BEVEL');mod.width=bevel;mod.segments=12;mod.harden_normals=True
        wn=o.modifiers.new('Surface normals','WEIGHTED_NORMAL');wn.keep_sharp=True;wn.weight=50
    if o.type=='MESH':
        for f in o.data.polygons:f.use_smooth=smooth or bool(bevel)
    return assign(o,m)
def box(n,c,d,m,bevel=.035):
    bpy.ops.mesh.primitive_cube_add(size=1,location=c);o=bpy.context.object;o.scale=d
    bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    return finish(o,n,m,bevel,False)
def ball(n,c,s,m):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64,ring_count=32,location=c);o=bpy.context.object;o.scale=s
    bpy.ops.object.transform_apply(location=False,rotation=False,scale=True);return finish(o,n,m)
def cylinder(n,c,r,h,m,rot=(0,0,0),vertices=96,bevel=.025):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices,radius=r,depth=h,location=c,rotation=rot)
    return finish(bpy.context.object,n,m,bevel)
def torus(n,c,R,r,m,rot=(0,0,0),scale=(1,1,1)):
    bpy.ops.mesh.primitive_torus_add(major_segments=128,minor_segments=20,location=c,major_radius=R,minor_radius=r,rotation=rot)
    o=bpy.context.object;o.scale=scale;return finish(o,n,m)
def mesh(n,vs,fs,m,smooth=True):
    me=bpy.data.meshes.new(n);me.from_pydata(vs,[],fs);me.update();o=bpy.data.objects.new(n,me);bpy.context.collection.objects.link(o)
    return finish(o,n,m,0,smooth)
def tube(n,pts,r,m):
    cu=bpy.data.curves.new(n,'CURVE');cu.dimensions='3D';cu.resolution_u=12;cu.bevel_depth=r;cu.bevel_resolution=4
    sp=cu.splines.new('POLY');sp.points.add(len(pts)-1)
    for p,v in zip(sp.points,pts):p.co=(*v,1)
    o=bpy.data.objects.new(n,cu);bpy.context.collection.objects.link(o);return assign(o,m)
def text(n,body,c,size,m,rot=(math.pi/2,0,0),align='CENTER',spacing=1.15):
    cu=bpy.data.curves.new(n,'FONT');cu.body=body;cu.align_x=align;cu.size=size;cu.space_character=spacing;cu.extrude=.0015;cu.bevel_depth=.0005;cu.resolution_u=8
    o=bpy.data.objects.new(n,cu);bpy.context.collection.objects.link(o);o.location=c;o.rotation_euler=rot;return assign(o,m)
def lathe(n,c,profile,m,steps=128):
    refined=[]
    for k in range(len(profile)-1):
        p0=np.array(profile[max(0,k-1)]);p1=np.array(profile[k]);p2=np.array(profile[k+1]);p3=np.array(profile[min(len(profile)-1,k+2)])
        for t in np.arange(8)/8:
            p=.5*((2*p1)+(-p0+p2)*t+(2*p0-5*p1+4*p2-p3)*t*t+(-p0+3*p1-3*p2+p3)*t*t*t);refined.append((max(0,p[0]),p[1]))
    refined.append(profile[-1]);profile=refined
    vs=[(c[0]+r*math.cos(j/steps*TAU),c[1]+r*math.sin(j/steps*TAU),c[2]+z)for r,z in profile for j in range(steps)]
    fs=[]
    for k in range(len(profile)-1):
        for j in range(steps):
            a=k*steps+j;b=k*steps+(j+1)%steps;d=(k+1)*steps+j;e=(k+1)*steps+(j+1)%steps;fs.append((a,b,e,d))
    return mesh(n,vs,fs,m)
def disc(n,c,outer,inner,m):
    vs=[];fs=[]
    for j in range(256):
        a=j/256*TAU
        for r in (outer,inner):vs.append((c[0]+r*math.cos(a),c[1]+r*math.sin(a),c[2]))
    for j in range(256):a=2*j;b=2*((j+1)%256);fs.append((a,b,b+1,a+1))
    return mesh(n,vs,fs,m)
def area(n,c,size,power,color=(1,1,1),target=(0,0,1)):
    mat(n,'matte',kd=(0,0,0),emission=tuple(power*x for x in color))
    o=box(n,c,(size[0],size[1],.002),n,0);o.rotation_euler=(Vector(target)-o.location).to_track_quat('Z','Y').to_euler()
    prop('scene.objects.'+n+'.camerainvisible',1)
def camera(c,t,fov=35,lens=0):
    global camera_data;camera_data={'origin':c,'target':t,'fov':fov}
    prop('scene.camera.lookat.orig',c);prop('scene.camera.lookat.target',t);prop('scene.camera.fieldofview',fov)
    if lens:prop('scene.camera.lensradius',lens);prop('scene.camera.focaldistance',(Vector(c)-Vector(t)).length)

asset_records=[];material_cache={}
def image_texture(name,path,color=False,scale=(1,-1)):
    assert Path(path).exists(),path
    prop(f'scene.textures.{name}.type','imagemap');prop(f'scene.textures.{name}.file',str(path));prop(f'scene.textures.{name}.gamma',2.2 if color else 1);prop(f'scene.textures.{name}.channel','rgb')
    prop(f'scene.textures.{name}.mapping.uvscale',scale);prop(f'scene.textures.{name}.mapping.uvdelta',(0,1 if scale[1]<0 else 0))
    return name

def pbr_material(bm,aid):
    if bm.name in material_cache:return material_cache[bm.name]
    name=re.sub('[^A-Za-z0-9_]','_',bm.name);material_cache[bm.name]=name
    node=next((n for n in bm.node_tree.nodes if n.type=='BSDF_PRINCIPLED'),None)
    if node is None and any(n.type=='BSDF_GLASS'for n in bm.node_tree.nodes):
        g=next(n for n in bm.node_tree.nodes if n.type=='BSDF_GLASS');mat(name,'glass',kr=(1,1,1),kt=tuple(g.inputs['Color'].default_value[:3]),interiorior=g.inputs['IOR'].default_value);return name
    if node is None:mat(name,basecolor=(.5,.5,.5),roughness=.5);return name
    if aid=='chemistry_set'and 'glass'in name:
        mat(name,'glass',kr=(1,1,1),kt=(1,1,1),interiorior=1.47);return name
    def value(label):
        s=node.inputs.get(label)
        if s is None:return 0
        return tuple(s.default_value[:3])if hasattr(s.default_value,'__iter__')else s.default_value
    kw={'basecolor':value('Base Color'),'roughness':value('Roughness'),'metallic':value('Metallic'),'specular':value('Specular IOR Level'),'multibounce':1}
    for image_node in bm.node_tree.nodes:
        if image_node.type!='TEX_IMAGE' or not image_node.image:continue
        fp=Path(bpy.path.abspath(image_node.image.filepath));bn=fp.name.lower();tn=name+'_'+re.sub('[^a-z0-9]','_',bn)
        if '_diff_'in bn or '_basecolor_'in bn:kw['basecolor']=image_texture(tn,fp,True)
        elif '_rough_'in bn:kw['roughness']=image_texture(tn,fp)
        elif '_metal_'in bn:kw['metallic']=image_texture(tn,fp)
        elif '_nor_gl_'in bn:
            image_texture(tn,fp);prop(f'scene.textures.{tn}_normal.type','normalmap');prop(f'scene.textures.{tn}_normal.texture',tn);prop(f'scene.textures.{tn}_normal.scale',next((float(n.inputs['Strength'].default_value)for n in bm.node_tree.nodes if n.type=='NORMAL_MAP'),.5));kw['bumptex']=tn+'_normal'
        elif '_alpha_'in bn:
            image_texture(tn,fp);kw['transparency']=tn
    # Preserve the measured translucent layer of food/porcelain; no AO is
    # multiplied into the albedo. Opaque asset maps retain their full UVs.
    if value('Subsurface Weight')>0:
        kw.update(subsurfaceweight=.08,subsurfaceradius=.002,subsurfaceradiusscale=1)
    if value('Transmission Weight')>.5:
        if aid=='wine_bottles_01' and '_glass_' in name:
            # Deliberately opaque lacquered wine packaging. This artwork does
            # not establish bulk bottle transmission or liquid absorption.
            mat(name,'openpbr',basecolor=(.007,.011,.009),specularroughness=.12,specularweight=1,basemetalness=0)
            return name
        # Existing asset glass/liquid is actual transmission plus an absorbing
        # interior, rather than a diffuse texture painted to resemble glass.
        kw={k:v for k,v in kw.items()if k in ['bumptex','transparency']}
        if aid=='wine_bottles_01':kw.pop('bumptex',None) # Clean polished bottle finish.
        kw.update(kr=(1,1,1),kt=(1,1,1),interiorior=value('IOR'))
        mat(name,'glass',**kw)
        va=next((n for n in bm.node_tree.nodes if n.type=='VOLUME_ABSORPTION'),None)
        if va:
            c=va.inputs['Color'].default_value[:3];density=va.inputs['Density'].default_value
            prop(f'scene.volumes.{name}_interior.type','clear');prop(f'scene.volumes.{name}_interior.ior',value('IOR'));prop(f'scene.volumes.{name}_interior.absorption',tuple(max(0,1-x)*density for x in c));prop(f'scene.materials.{name}.volume.interior',name+'_interior')
    else:
        if aid!='brass_goblets' and scene not in ['music','textile','audio']:
            kw['basemetalness']=kw.pop('metallic');kw['specularroughness']=kw.pop('roughness');kw['specularweight']=kw.pop('specular')*2;kw.pop('multibounce',None)
            if 'subsurfaceweight'in kw:kw['subsurfacecolor']=kw['basecolor']
            mat(name,'openpbr',**kw)
        else:mat(name,**kw)
    return name

def asset(aid,c,width=None,angle=0,select=None):
    path=ROOT.parent/'assets'/aid/(aid+'.blend')
    if not path.exists():path=Path('/tmp/slc-studio-20261011/assets')/aid/(aid+'.blend')
    with bpy.data.libraries.load(str(path),link=False)as(src,dst):dst.objects=[n for n in src.objects if select is None or any(s in n for s in select)]
    obs=[o for o in dst.objects if o and o.type=='MESH' and not o.name.startswith('wdg_')]
    if not obs:raise ValueError((aid,select))
    for o in obs:bpy.context.collection.objects.link(o)
    bpy.context.view_layer.update()
    original={o:o.matrix_world.copy()for o in obs}
    points=[original[o]@Vector(v)for o in obs for v in o.bound_box];lo=Vector([min(p[i]for p in points)for i in range(3)]);hi=Vector([max(p[i]for p in points)for i in range(3)])
    anchors=[o for o in obs if 'pot'in o.name and 'potted'in aid and not any(x in o.name for x in ['dirt','leaves','pebbles','stem'])]
    anchor=[original[o]@Vector(v)for o in anchors for v in o.bound_box]if anchors else points
    origin=Vector([(min(p[i]for p in anchor)+max(p[i]for p in anchor))/2 for i in range(2)]+[min(p.z for p in anchor)])
    scale=width/(hi.x-lo.x)if width else 1
    M=Matrix.Translation(c)@Matrix.Rotation(angle,4,'Z')@Matrix.Scale(scale,4)@Matrix.Translation(-origin)
    for o in obs:
        o.parent=None;o.matrix_world=M@original[o]
        for mod in o.modifiers:
            if mod.type=='SUBSURF':mod.levels=min(2,mod.render_levels);mod.render_levels=mod.levels
        mats=[pbr_material(bm,aid)if bm else 'white'for bm in o.data.materials]
        if not mats:mats=['white']
        assign(o,mats[0]);o['slc_materials']=mats
    asset_records.append({'asset':aid,'selection':select,'scale':scale,'position':c,'rotation':angle})
    return obs

def surface(name,aid,roughness=None,uvscale=1):
    r=ROOT.parent/'assets'/aid
    color=image_texture(name+'_color',next(p for p in r.rglob('*')if '_diff_'in p.name or '_col_'in p.name),True,(uvscale,-uvscale))
    rough=image_texture(name+'_rough',next(r.rglob(aid+'_rough_*')),False,(uvscale,-uvscale))
    nor=image_texture(name+'_nor',next(r.rglob(aid+'_nor_gl_*')),False,(uvscale,-uvscale));prop(f'scene.textures.{name}_normal.type','normalmap');prop(f'scene.textures.{name}_normal.texture',nor);prop(f'scene.textures.{name}_normal.scale',.25)
    mat(name,basecolor=color,roughness=rough if roughness is None else roughness,bumptex=name+'_normal',specular=.5)

def environment(aid='studio_small_03',gain=.3,rotation=0):
    prop('scene.lights.world.type','infinite');prop('scene.lights.world.file',str(ROOT.parent/'assets'/aid/(aid+'.exr')));prop('scene.lights.world.gamma',1);prop('scene.lights.world.gain',(gain,)*3)
    a=rotation;prop('scene.lights.world.transformation',(math.cos(a),math.sin(a),0,0,-math.sin(a),math.cos(a),0,0,0,0,1,0,0,0,0,1))

def stock():
    mat('white',basecolor=(.74,.72,.66),roughness=.38);mat('black',basecolor=(.015,.017,.019),roughness=.4);mat('gold','metal2',preset='gold',uroughness=.16,vroughness=.16);mat('silver','metal2',preset='silver',uroughness=.12,vroughness=.12)
    mat('glass','glass',kr=(1,1,1),kt=(1,1,1),interiorior=1.48)

def tabletop(aid='oak_veneer_01'):
    surface('table',aid,uvscale=2);box('table',(0,0,-.04),(3,3,.08),'table',.003)

def linen(c=(0,0,.006),size=(.64,.50),angle=.5):
    surface('linen','rough_linen',uvscale=1)
    vs=[];fs=[];nu,nv=90,70
    for i in range(nu):
        for j in range(nv):
            x=(i/(nu-1)-.5)*size[0];y=(j/(nv-1)-.5)*size[1];z=.003*math.sin(x*60+y*20)+.002*math.sin(y*105)+.010*math.exp(-((x-.08)/.025)**2)
            vs.append((x,y,z))
    for i in range(nu-1):
        for j in range(nv-1):a=i*nv+j;fs.append((a,a+nv,a+nv+1,a+1))
    o=mesh('washed-linen',vs,fs,'linen');o.location=c;o.rotation_euler.z=angle
    # Correct metres per repeat on the textile rather than a flat-colour plane.
    uv=o.data.uv_layers.new()
    for loop in o.data.loops:
        v=o.data.vertices[loop.vertex_index].co;uv.data[loop.index].uv=(v.x,v.y)

def beauty():
    stock();tabletop();environment(gain=.12);camera((.61,-.97,.63),(-.01,.065,.055),32,.003)
    linen((.04,.09,.006),(.7,.62),-.3)
    asset('tea_set_01',(-.08,-.10,.015),.285,select=['plate_large_circular_03'])
    asset('croissant',(-.105,-.08,.019),.205,-.15)
    asset('tea_set_01',(.22,.065,.015),.15,select=['saucer_circular_03'])
    asset('tea_set_01',(.22,.065,.030),.106,-.20,select=['cup_small_01'])
    mat('coffee','glass',kr=(1,1,1),kt=(.13,.050,.018),interiorior=1.333);cylinder('fresh-coffee',(.22,.065,.075),.039,.002,'coffee',bevel=0)
    asset('tea_set_01',(-.08,.30,.017),.165,-.9,select=['teapot_01'])
    asset('lemon',(-.31,.16,.001),.095,.3)
    area('window',(-.7,.18,1.0),(.7,1.0),18,(1,.93,.83),(0,0,0))
    area('bounce',(.55,-.15,.8),(.5,.75),2.2,(.83,.91,1),(0,0,.08))
    box('negative-fill',(.70,.1,.4),(.02,.8,.8),'black',0);prop('scene.objects.negative-fill.camerainvisible',1)

def wall(c=(0,1,1),d=(4,.10,2)):
    surface('plaster','clay_plaster',uvscale=1);box('plaster-wall',c,d,'plaster',.003)

def perfume():
    stock();tabletop();wall((0,.70,.65),(3,.08,1.4));environment('photo_studio_loft_hall',.18)
    camera((.66,-1.10,.44),(.02,.07,.18),32,.002)
    asset('wine_bottles_01',(-.055,.065,.001),.075,-.2,select=['bordeaux'])
    asset('wine_bottles_01',(.12,.17,.001),.081,.25,select=['alsace'])
    asset('food_pomegranate_01',(.13,-.03,.001),.086,.8)
    asset('food_lime_01',(.26,.055,.001),.060,.6)
    area('long-softbox',(-.45,-.15,.75),(.15,.65),14,(1,.93,.84),(0,0,.16))
    area('glass-edge',(.30,.48,.48),(.10,.6),18,(.87,.93,1),(0,0,.16))
    prop('scene.lights.sun.type','sun');prop('scene.lights.sun.dir',(-.75,.65,1));prop('scene.lights.sun.turbidity',2.2);prop('scene.lights.sun.gain',(.0028,.0026,.0024));prop('scene.lights.sun.relsize',.35)

def audio():
    stock();surface('table','oak_veneer_01',uvscale=2);box('table',(0,0,-.02),(3,3,.04),'table',.002);environment(gain=.2)
    camera((.54,-.9,.39),(.0,.02,.14),30,.002)
    asset('brass_goblets',(-.075,0,.001),.13,-.4,select=['goblet_03'])
    asset('brass_goblets',(.10,.09,.001),.106,.65,select=['goblet_01'])
    asset('brass_vase_03',(-.23,.20,.001),.105,-.2)
    asset('food_lime_01',(.19,.06,.001),.052,.8)
    # Same albedo/metal mask/geometry/light in both renders. Only the
    # multi-scatter compensation boolean changes in the upstream reference.
    names = {n for n in material_cache.values() if n.startswith(('brass_goblets_01','brass_goblets_03'))}
    assert len(names) == 2, names
    for name in names:
        prop(f'scene.materials.{name}.roughness',.98);prop(f'scene.materials.{name}.multibounce',1)
    area('window',(-.35,.1,.65),(.55,.65),12,(1,.94,.85),(0,0,.13));area('strip',(.35,.25,.52),(.07,.65),9,(.78,.90,1),(0,0,.14))

def music():
    stock();tabletop('oak_veneer_01');environment('studio_small_03',.3);camera((.46,-.68,.67),(.0,.025,.048),35,.0015)
    asset('cassette_player',(-.11,.13,.001),.129,-.2,select=['body'])
    asset('cassette_player',(-.16,-.10,.001),.10,.23,select=['tape'])
    asset('boombox',(.07,.49,.001),.55,0)
    # Exact 120-mm CD proportions; the new shader is the feature, while the
    # surrounding scanned electronics provide a believable working context.
    mat('case',basecolor=(.018,.023,.026),roughness=.23);box('jewel-tray',(.085,-.085,.005),(.138,.146,.010),'case',.004)
    mat('cd','diffraction',kr=(.96,.96,.96),spacing=1600,orientation='radial',center=(.085,-.085,.012),roughness=.026,fillfactor=.5,blaze=0,orders=8)
    disc('optical-disc',(.085,-.085,.012),.06,.0075,'cd');disc('disc-hub',(.085,-.085,.0122),.018,.0075,'silver')
    area('key',(-.45,-.22,.72),(.24,.50),9,(1,.94,.83),(0,0,.1));area('reflection-strip',(.26,.38,.42),(.025,.3),22,(.85,.92,1),(0,0,.1))

def interior():
    stock();surface('floor','wood_floor_deck',uvscale=2);box('floor',(0,0,-.06),(10,10,.12),'floor',.002);wall((0,1.52,1.5),(8,.16,3))
    environment('kiara_6_afternoon',.65,.45);camera((2.6,-3.6,1.65),(.10,.37,.80),43,.003)
    asset('sofa_02',(-.40,.56,.001),1.807,0)
    asset('throw_pillows_01',(-.97,.42,.41),.46,.10,select=['pillow01'])
    asset('coffee_table_round_01',(.15,-.52,.001),.82)
    asset('potted_plant_02',(1.38,.58,.001),.75,.2)
    asset('potted_plant_01',(-1.66,.42,.001),.54,-.3)
    asset('antique_ceramic_vase_01',(.12,-.47,.312),.18)
    asset('binder_notebook',(-.07,-.60,.312),.17,.2,select=['closed'])
    mat('trim',basecolor=(.77,.75,.70),roughness=.48);box('skirting',(0,1.418,.06),(8,.06,.12),'trim',.005)
    for x in [-2.4,2.3]:box('pilaster'+str(x),(x,1.36,1.5),(.075,.10,3),'trim',.004)
    # Visible architectural scale and irregular real wood boards.
    box('window-return',(-2.4,.7,1.5),(.16,1.5,3),'plaster',.006)
    area('daylight',(-2.5,-1.1,2.35),(1.6,2),6,(1,.94,.83),(0,.45,.7));area('bounce',(1,-2.5,2.3),(2,2),.6,(.83,.92,1),(0,.5,.7))
    prop('scene.lights.sun.type','sun');prop('scene.lights.sun.dir',(-.8,-.3,.8));prop('scene.lights.sun.gain',(.0013,.0012,.0011));prop('scene.lights.sun.relsize',.7)

def textile():
    stock();surface('floor','wood_floor_deck',uvscale=2);box('floor',(0,0,-.04),(10,10,.08),'floor',.002);wall((0,1.5,1.5),(8,.1,3));environment('photo_studio_loft_hall',.2)
    camera((1.20,-1.85,1.25),(0,-.04,.60),35,.004)
    chair=asset('mid_century_lounge_chair',(0,.12,.001),.88,-.04)
    bpy.context.view_layer.update()
    from mathutils.bvhtree import BVHTree
    verts=[];faces=[]
    for ob in chair:
        ev=ob.evaluated_get(bpy.context.evaluated_depsgraph_get());me=ev.to_mesh();me.calc_loop_triangles();offset=len(verts);verts.extend(ob.matrix_world@v.co for v in me.vertices);faces.extend(tuple(offset+v for v in t.vertices)for t in me.loop_triangles);ev.to_mesh_clear()
    tree=BVHTree.FromPolygons(verts,faces,all_triangles=True)
    gx,gy=40,60;heights=np.empty((gx,gy));y0,y1=-.29,.04;span=y1-y0
    for ix in range(gx):
        for iy in range(gy):
            x=(ix/(gx-1)-.5)*.36;y=y0+iy/(gy-1)*span;hit=tree.ray_cast(Vector((x,y,2)),Vector((0,0,-1)))[0];heights[ix,iy]=(hit.z if hit else .45)+.003
    asset('potted_plant_02',(.93,.67,.001),.60,.4)
    mat('woolbase',basecolor=(.58,.50,.37),roughness=.75,sheen=.55,sheenroughness=.4)
    mat('fur','hairmat',model='huang',eumelanin=.01,pheomelanin=.01,roughness=.38,alpha=3)
    def height(x,y):
        fx=np.clip((x/.36+.5)*(gx-1),0,gx-1.001);fy=np.clip((y-y0)/span*(gy-1),0,gy-1.001);ix,iy=int(fx),int(fy);dx,dy=fx-ix,fy-iy
        return (1-dx)*((1-dy)*heights[ix,iy]+dy*heights[ix,iy+1])+dx*((1-dy)*heights[ix+1,iy]+dy*heights[ix+1,iy+1])
    vs=[];fs=[];nu,nv=40,60
    for i in range(nu):
        for j in range(nv):
            y=y0+j/(nv-1)*span;w=.36*(.9+.1*math.sin(y*19));x=(i/(nu-1)-.5)*w;vs.append((x,y,height(x,y)))
    for i in range(nu-1):
        for j in range(nv-1):a=i*nv+j;fs.append((a,a+nv,a+nv+1,a+1))
    mesh('sheepskin-hide',vs,fs,'woolbase')
    count,segs=50000,9;points=[];thickness=[]
    for k in range(count):
        y=rng.uniform(y0,y1);w=.36*(.9+.1*math.sin(y*19));x=rng.uniform(-w/2,w/2);phase=rng.uniform(0,TAU);length=rng.uniform(.045,.065);bend=rng.uniform(-.25,.25)
        eps=.002;normal=Vector((-(height(x+eps,y)-height(x-eps,y))/(2*eps),-(height(x,y+eps)-height(x,y-eps))/(2*eps),1)).normalized();side=normal.cross(Vector((0,1,0))).normalized();along=normal.cross(side).normalized();root=Vector((x,y,height(x,y)))
        for j in range(segs+1):
            t=j/segs;p=root+normal*(length*t)+side*(.008*math.sin(t*13+phase)*t)+along*(.006*math.cos(t*13+phase)*t+length*bend*t);points.append(tuple(p));thickness.append(.00030*(1-.80*t))
    hp=W/'sheepskin.hair';hp.write_bytes(struct.pack('<4s4I2f3f88s',b'HAIR',count,count*(segs+1),7,segs,.00016,0,1,1,1,b'Native wool groom'.ljust(88,b'\0'))+np.full(count,segs,dtype='<u2').tobytes()+np.array(points,dtype='<f4').tobytes()+np.array(thickness,dtype='<f4').tobytes())
    prop('scene.shapes.wool.type','strands');prop('scene.shapes.wool.file',str(hp));prop('scene.shapes.wool.tessellation.type','solid');prop('scene.shapes.wool.tessellation.solid.sidecount',4);prop('scene.objects.wool.shape','wool');prop('scene.objects.wool.material','fur')
    area('window',(-1.0,-.8,1.8),(.8,1.5),9,(1,.93,.84),(0,0,.5));area('fill',(.9,-.3,1.7),(.6,1.2),2,(.85,.92,1),(0,0,.5))

def jewelry():
    stock();tabletop('oak_veneer_01');linen((.055,.04,.009),(.43,.37),.28);environment(gain=.22)
    camera((.28,-.38,.48),(0,.015,.022),30,.001)
    asset('seadogs_compass',(-.085,.035,.014),.081,-.35)
    asset('round_spectacles',(.075,-.015,.015),.15,.3)
    asset('binder_notebook',(.025,.14,.020),.16,.12,select=['closed'])
    area('reflection-key',(-.3,.05,.55),(.28,.5),14,(1,.94,.83),(0,0,.02));area('edge',(.3,.22,.3),(.045,.35),9,(.87,.94,1),(0,0,.02))

def pearl():
    stock();tabletop();environment(gain=.25);camera((.51,-.84,.46),(0,.025,.13),30,.002)
    asset('brass_vase_03',(-.13,.16,.001),.09,-.2)
    asset('antique_ceramic_vase_01',(.08,.02,.001),.17,.1)
    asset('tea_set_01',(-.14,-.13,.001),.13,select=['saucer_circular_04'])
    mat('bubble','archglass',kr=(1,1,1),kt=(1,1,1),interiorior=1.333,filmthickness=470,filmior=1.333)
    for i,(c,r)in enumerate([((-.04,-.06,.18),.048),((.15,.10,.31),.035),((.23,-.05,.18),.020),((.03,-.17,.072),.024)]):ball('soap-film'+str(i),c,(r,)*3,'bubble')
    area('tall-window',(-.50,.14,.70),(.25,.60),14,(1,.95,.88),(0,0,.1));area('edge',(.28,.39,.48),(.10,.6),12,(.88,.94,1),(0,0,.1))

def architecture():
    stock();surface('stone','marble_01',uvscale=1);surface('plaster','clay_plaster',uvscale=1);surface('wood','oak_veneer_01',uvscale=2)
    environment('kiara_6_afternoon',.32,-.8);camera((5.1,-6.7,2.25),(.0,.8,.65),47)
    box('terrace',(0,1,-.10),(100,100,.20),'stone',.003)
    box('rear-wall',(0,3.2,1.65),(6.5,.18,3.3),'plaster',.008);box('roof',(0,1.8,3.30),(6.9,3.2,.22),'plaster',.008)
    for x in [-3,3]:box('column'+str(x),(x,.30,1.60),(.18,.18,3.2),'plaster',.009)
    for i in range(15):box('oak-batten'+str(i),(-2.95+i*.42,1.70,3.12),(.11,3.0,.14),'wood',.006)
    asset('mid_century_lounge_chair',(-1.45,1.80,.001),.90,.12)
    asset('coffee_table_round_01',(-.25,1.8,.001),.66)
    asset('ceramic_vase_03',(-.28,1.8,.251),.10)
    asset('potted_plant_01',(1.74,2.3,.001),.8)
    # Water is a real closed dielectric with an absorbing interior. Ripple
    # normals change refraction rather than painting a blue plane.
    mat('water','glass',kr=(1,1,1),kt=(1,1,1),interiorior=1.333)
    prop('scene.volumes.waterbody.type','clear');prop('scene.volumes.waterbody.ior',1.333);prop('scene.volumes.waterbody.absorption',(.24,.05,.025));prop('scene.materials.water.volume.interior','waterbody')
    prop('scene.textures.ripple.type','fbm');prop('scene.textures.ripple.octaves',5);prop('scene.textures.ripple.roughness',.7);prop('scene.textures.waterheight.type','scale');prop('scene.textures.waterheight.texture1','ripple');prop('scene.textures.waterheight.texture2',.002);prop('scene.materials.water.bumptex','waterheight')
    box('pool-water',(.55,-.95,.075),(3.4,2.0,.14),'water',.002)
    for y in [-2.0,.1]:box('pool-edge'+str(y),(.55,y,.12),(3.7,.16,.24),'stone',.004)
    for x in [-1.23,2.33]:box('pool-edge'+str(x),(x,-.95,.12),(.16,2.25,.24),'stone',.004)
    prop('scene.lights.sun.type','sun');prop('scene.lights.sun.dir',(-.8,-.25,.85));prop('scene.lights.sun.gain',(.0014,.0013,.0012));prop('scene.lights.sun.relsize',.7)

def atmosphere():
    stock();surface('floor','wood_floor_deck',uvscale=2);box('floor',(0,0,-.06),(8,10,.12),'floor',.002);wall((0,1.75,1.5),(7,.14,3));environment('kiara_6_afternoon',.12)
    camera((2.5,-4.15,1.6),(-.15,.1,.9),40,.004)
    asset('mid_century_lounge_chair',(-.50,.5,.001),.90,.07)
    asset('potted_plant_01',(.90,.90,.001),.75,-.25)
    asset('coffee_table_round_01',(.38,-.14,.001),.6)
    asset('binder_notebook',(.38,-.14,.229),.18,.23,select=['closed'])
    asset('tea_set_01',(.59,-.15,.229),.09,select=['cup_small_01'])
    for y in [-1.05,.45]:box('window-jamb'+str(y),(-1.65,y,1.5),(.12,.06,2.65),'black',.003)
    for z in [.18,2.82]:box('window-rail'+str(z),(-1.65,-.30,z),(.12,1.56,.06),'black',.003)
    for y,length in [(-2.06,2.0),(1.20,1.45)]:box('window-wall'+str(y),(-1.72,y,1.5),(.14,length,3),'plaster',.004)
    for i in range(8):box('window-mullion'+str(i),(-1.66,-.95+i*.21,1.50),(.06,.035,2.7),'black',.003)
    prop('scene.volumes.dust.type','homogeneous');prop('scene.volumes.dust.absorption',(.003,.003,.003));prop('scene.volumes.dust.scattering',(.012,.012,.012));prop('scene.volumes.dust.asymmetry',.35);prop('scene.volumes.dust.multiscattering',1)
    prop('scene.volumes.air.type','clear');prop('scene.volumes.air.ior',1);prop('scene.world.volume.default','air')
    mat('dust-boundary','null');prop('scene.materials.dust-boundary.volume.interior','dust');prop('scene.materials.dust-boundary.volume.exterior','air');box('room-medium',(0,0,1.5),(6,5,3),'dust-boundary',0)
    prop('scene.lights.sun.type','sun');prop('scene.lights.sun.dir',(-.8,-.1,.55));prop('scene.lights.sun.gain',(.0014,.00125,.0011));prop('scene.lights.sun.relsize',.35)

def heritage():
    stock();tabletop('oak_veneer_01');environment('photo_studio_loft_hall',.22)
    camera((.68,-1.13,.47),(0,.065,.15),33,.002)
    asset('vintage_video_camera',(-.07,.065,.001),.15,-.15)
    asset('cassette_player',(.16,-.12,.001),.10,.38,select=['tape'])
    asset('round_spectacles',(-.20,-.12,.001),.12,.20)
    asset('binder_notebook',(.19,.12,.001),.18,-.12,select=['closed'])
    area('window',(-.55,.15,.70),(.45,.70),14,(1,.94,.84),(0,0,.13));area('front-fill',(.10,-.50,.72),(.55,.60),10,(.90,.95,1),(0,0,.13));area('cool-edge',(.40,.45,.40),(.06,.50),10,(.82,.91,1),(0,0,.13))

def fan():
    # Real ceiling fan in a furnished room. Camera points upward; the PBR
    # wood blades use time-varying transforms, not a post-process blur.
    stock();wall((0,1.8,1.5),(8,.1,3));surface('wood','oak_veneer_01',uvscale=2);box('ceiling',(0,0,3.06),(8,8,.12),'plaster',.003);environment('kiara_6_afternoon',.35)
    obs=asset('ceiling_fan',(0,0,2.48),1.46)
    for o in obs:
        if 'blades'in o.name:o['fan_motion']=True
    asset('potted_plant_02',(1.2,1.1,.001),.9)
    asset('sofa_02',(-.7,1.2,.001),1.8)
    camera((1.55,-2.0,1.25),(0,0,2.6),38)
    area('window',(-2,-.5,2.0),(1.5,2),9,(1,.94,.86),(0,0,2.4))

def prism():
    # A real optical object photographed with supporting laboratory glass.
    # Short path lengths and carefully defined reflections make dispersion
    # visible without leaning on a difficult screen caustic hero shot.
    stock();tabletop();wall((0,.6,.5),(3,.08,1));environment(gain=.25)
    camera((.35,-.48,.25),(.0,.0,.064),31,.0005)
    mat('flint','glass',kr=(1,1,1),kt=(1,1,1),interiorior=1.805,sellmeier='N-SF6')
    xy=[(-.08,-.045),(.08,-.045),(0,.09)];vs=[(x,y,z)for z in [.001,.125]for x,y in xy];fs=[(2,1,0),(3,4,5),(0,1,4,3),(1,2,5,4),(2,0,3,5)];o=mesh('precision-optic',vs,fs,'flint',False)
    mod=o.modifiers.new('Optical edge polish','BEVEL');mod.width=.0008;mod.segments=3
    asset('chemistry_set',(-.22,.14,.001),.075,select=['beaker_med'])
    asset('round_spectacles',(.17,.02,.001),.13,.2)
    asset('binder_notebook',(.05,.26,.001),.17,-.2,select=['closed'])
    area('broad-key',(-.4,.1,.6),(.18,.65),12,(1,.97,.92),(0,0,.07));area('spectral-edge',(.30,.23,.35),(.012,.4),22,(1,1,1),(0,0,.07))

def export(o):
    deps=bpy.context.evaluated_depsgraph_get();e=o.evaluated_get(deps);me=e.to_mesh();me.calc_loop_triangles();M=o.matrix_world;N=M.to_3x3().inverted().transposed()
    vs=[];fs=[];face_materials=[]
    uv=me.uv_layers.active.data if me.uv_layers.active else None
    metric=o.get('slc_material')in ['table','floor','plaster','stone','wood']
    for tri in me.loop_triangles:
        poly=me.polygons[tri.polygon_index]
        for j,vi in enumerate(tri.vertices):
            v=M@me.vertices[vi].co;n=(N@(me.corner_normals[tri.loops[j]].vector if poly.use_smooth else poly.normal)).normalized();t=uv[tri.loops[j]].uv if uv else (v.x,v.y)
            if metric:
                axis=max(range(3),key=lambda k:abs(n[k]));t=(v.y,v.z)if axis==0 else ((v.x,v.z)if axis==1 else (v.x,v.y))
            vs.append((*v,*n,*t))
        fs.append((len(vs)-3,len(vs)-2,len(vs)-1))
        face_materials.append(poly.material_index)
    e.to_mesh_clear()
    for mi,material in enumerate(o.get('slc_materials',[o['slc_material']])):
        faces=[f for f,fmi in zip(fs,face_materials)if fmi==mi]if 'slc_materials'in o else fs
        if not faces:continue
        name=o.name+('_part'+str(mi)if 'slc_materials'in o else '');path=W/(name+'.ply')
        header=f'ply\nformat binary_little_endian 1.0\nelement vertex {len(vs)}\nproperty float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\nproperty float s\nproperty float t\nelement face {len(faces)}\nproperty list uchar int vertex_indices\nend_header\n'
        arr=np.zeros(len(faces),dtype=[('n','u1'),('idx','<i4',(3,))]);arr['n']=3;arr['idx']=faces
        path.write_bytes(header.encode()+np.asarray(vs,dtype='<f4').tobytes()+arr.tobytes())
        prop('scene.objects.'+name+'.ply',str(path));prop('scene.objects.'+name+'.material',material);prop('scene.objects.'+name+'.id',len(objects)+objects.index(o)+1)
    if o.name.startswith(('window-slats','shadow-slat')):prop('scene.objects.'+o.name+'.camerainvisible',1)
    if o.get('fan_motion'):
        center=o.matrix_world.translation
        for i,a in enumerate([-.08,.08]):
            R=Matrix.Translation(center)@Matrix.Rotation(a,4,'Z')@Matrix.Translation(-center)
            prop(f'scene.objects.{o.name+"_part0" if "slc_materials"in o else o.name}.motion.{i}.time',i);prop(f'scene.objects.{o.name+"_part0" if "slc_materials"in o else o.name}.motion.{i}.transformation',[R[j][k]for k in range(4)for j in range(4)])
        prop('scene.camera.shutteropen',0);prop('scene.camera.shutterclose',1)
    if o.get('motion'):
        for i,a in enumerate([-.55,.55]):
            R=Matrix.Translation((0,0,1.71))@Matrix.Rotation(a,4,'Y')@Matrix.Translation((0,0,-1.71))
            prop(f'scene.objects.{o.name+"_part0" if "slc_materials"in o else o.name}.motion.{i}.time',i);prop(f'scene.objects.{o.name+"_part0" if "slc_materials"in o else o.name}.motion.{i}.transformation',[R[j][k]for k in range(4)for j in range(4)])
        prop('scene.camera.shutteropen',0);prop('scene.camera.shutterclose',1)
    return len(fs)
SCENES={n:globals()[n]for n in ['beauty','perfume','audio','music','interior','textile','jewelry','pearl','architecture','atmosphere','heritage','fan','prism']}
for name,build in SCENES.items():
    if A.only and name not in A.only.split(','):continue
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    S=[];objects=[];scene=name;W=ROOT/name;W.mkdir(parents=True,exist_ok=True);asset_records=[];material_cache={}
    build();bpy.context.view_layer.update();triangles=sum(export(o)for o in objects)
    (W/'scene.scn').write_text('\n'.join(S)+'\n');(W/'scene.json').write_text(json.dumps({'name':name,'camera':camera_data,'objects':len(objects),'triangles':triangles,'art_direction':'Photographic real-scale PBR asset staging, October 2026','assets':asset_records},indent=2)+'\n')
    print('SCENE_READY',name,len(objects),triangles,flush=True)
