# SPDX-License-Identifier: Apache-2.0
"""Sharp Cycles BSSRDF camera connections against independent eye transport.

Run with Blender Python, the complete isolated wheel and the identity/audit
environment variables. The existing CPU and rough-adjoint contracts run first.
These fixed-coefficient pure-LIGHTCPU contracts are not production acceptance.
"""
import ast
import hashlib
import json
import math
import os
from pathlib import Path
import runpy
import time

import numpy as np

adj = runpy.run_path(str(Path(__file__).with_name('cycles_bssrdf_adjoint_test.py')))
ns = adj['ns']
slc, props, scene = ns['slc'], ns['properties'], ns['scene']
W = H = 32
ns['BASE'].update({'film.width': W, 'film.height': H, 'native.threads.count': 8,
                   # LIGHTCPU has no primary environment-background path.
                   # Material IDs do not guarantee full subpixel coverage,
                   # even after erosion. Keep all lighting and secondary
                   # environment hits; match only this absent primary path.
                   'path.forceblackbackground.enable': True})
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR']) / 'sharp'
folder.mkdir(exist_ok=True)
hash_ns = adj['hash_ns']
# Same fixture builders and data extraction; the independent truth is the
# eye random walk, which does not use the inverse boundary/manifold solver.
tree = ast.parse(Path(__file__).with_name('cycles_bssrdf_adjoint_test.py').read_text())
for function in tree.body:
    if isinstance(function, ast.FunctionDef) and function.name in ('body', 'config', 'render', 'identities'):
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<sharp-fixture-helpers>', 'exec'), globals())


def sphere(scn, uv=False, scale=(1.,1.,1.)):
    nlat,nlon=16,32
    vertices=[(0.,0.,1.)]
    for i in range(1,nlat):
        for j in range(nlon):
            th=math.pi*i/nlat;ph=2*math.pi*j/nlon
            vertices.append((math.sin(th)*math.cos(ph),math.sin(th)*math.sin(ph),math.cos(th)))
    bottom=len(vertices);vertices.append((0.,0.,-1.))
    faces=[]
    for j in range(nlon):
        faces.append((0,1+j,1+(j+1)%nlon))
        faces.append((bottom,1+(nlat-2)*nlon+j,1+(nlat-2)*nlon+(j+1)%nlon))
    for i in range(nlat-2):
        for j in range(nlon):
            a=1+i*nlon+j;b=1+i*nlon+(j+1)%nlon;c=a+nlon;d=b+nlon
            faces.extend([(a,b,c),(b,d,c)])
    for i,(a,b,c) in enumerate(faces):
        pa,pb,pc=[np.array(vertices[v]) for v in (a,b,c)]
        if np.dot(np.cross(pb-pa,pc-pa),pa+pb+pc)<0:faces[i]=(a,c,b)
    shape={'scene.shapes.ball.type':'inlinedmesh','scene.shapes.ball.vertices':sum(vertices,()),
           'scene.shapes.ball.faces':sum(faces,()),'scene.shapes.ball.normals':sum(vertices,()),
           'scene.objects.body.shape':'ball','scene.objects.body.material':'body', 'scene.objects.body.id':432}
    if uv: shape['scene.shapes.ball.uvs']=sum([(math.atan2(y,x)/(2*math.pi)+.5,math.acos(z)/math.pi) for x,y,z in vertices],())
    if scale!=(1.,1.,1.): shape['scene.objects.body.transformation']=(scale[0],0.,0.,0.,0.,scale[1],0.,0.,0.,0.,scale[2],0.,0.,0.,0.,1.)
    scn.Parse(props(shape))
    return scn


def torus(scn):
    outer, tube, nmajor, nminor = .75, .25, 40, 20
    vertices, normals, faces = [], [], []
    for i in range(nmajor):
        phi = 2. * math.pi * i / nmajor
        for j in range(nminor):
            theta = 2. * math.pi * j / nminor
            radial = outer + tube * math.cos(theta)
            vertices.append((radial * math.cos(phi), radial * math.sin(phi), tube * math.sin(theta)))
            normals.append((math.cos(theta) * math.cos(phi), math.cos(theta) * math.sin(phi), math.sin(theta)))
    for i in range(nmajor):
        for j in range(nminor):
            a, b = i * nminor + j, ((i + 1) % nmajor) * nminor + j
            c, d = i * nminor + (j + 1) % nminor, ((i + 1) % nmajor) * nminor + (j + 1) % nminor
            faces.extend(((a, b, c), (b, d, c)))
    for i, (a, b, c) in enumerate(faces):
        pa, pb, pc = (np.asarray(vertices[v]) for v in (a, b, c))
        expected = np.asarray(normals[a]) + np.asarray(normals[b]) + np.asarray(normals[c])
        if np.dot(np.cross(pb - pa, pc - pa), expected) < 0:
            faces[i] = (a, c, b)
    scn.Parse(props({'scene.shapes.ring.type': 'inlinedmesh',
                     'scene.shapes.ring.vertices': sum(vertices, ()),
                     'scene.shapes.ring.faces': sum(faces, ()),
                     'scene.shapes.ring.normals': sum(normals, ()),
                     'scene.objects.body.shape': 'ring', 'scene.objects.body.material': 'body',
                     'scene.objects.body.id': 432}))
    return scn


def perspective(scn, target=(0., 0., 0.), origin=(0., -4., 0.)):
    scn.Parse(props({'scene.camera.type': 'perspective', 'scene.camera.lookat.orig': origin,
                     'scene.camera.lookat.target': target, 'scene.camera.up': (0., 0., 1.),
                     'scene.camera.fieldofview': 45.}))
    return scn


def interior_mask(image, material):
    mask = image['MATERIAL_ID'] == material
    result = mask.copy()
    for axis in (0, 1):
        result &= np.roll(mask, 1, axis) & np.roll(mask, -1, axis)
    result[[0, -1], :] = False
    result[:, [0, -1]] = False
    assert result.sum() > 30, (material, result.sum())
    return result


records = []

def save_metrics(complete=False):
    (folder / 'sharp-metrics.json').write_text(json.dumps({
        'native_sha256': ns['identity']['native_sha256'], 'cpu_contracts': len(ns['records']),
        'rough_adjoint_contracts': len(adj['records']), 'production_acceptance': False,
        'hybrid_or_gpu_adjoint_implemented': False, 'complete': complete, 'records': records
    }, indent=2) + '\n')


def compare(case, scn, *, seed=131, spectral=False, materials=(991,)):
    serialized = scn.ToProperties().ToString()
    (folder / (case + '-scene.scn')).write_text(serialized)
    eye = render(scn, 'PATHCPU', spectral, spp=8192, seed=seed)
    light = render(scn, 'LIGHTCPU', spectral, spp=65536, seed=seed)
    for engine, data in (('eye', eye), ('light', light)):
        np.savez_compressed(folder / (case + '-' + engine + '.npz'), **data)
        identities(data, ('body', 'floor') if len(materials) > 1 else ('body',),
                   ('body', 'floor') if len(materials) > 1 else ('body',))
    for material in materials:
        mask = interior_mask(eye, material)
        a, b = [data['RGB'][mask].mean(axis=0).astype(np.float64) for data in (eye, light)]
        error = float(np.max(np.abs(a - b) / np.maximum(a, 1e-4)))
        record = {'case': case, 'material': material, 'seed': seed, 'spectral': spectral,
                  'scene_sha256': hashlib.sha256(serialized.encode()).hexdigest(),
                  'pixels': int(mask.sum()), 'eye_rgb_mean': a.tolist(), 'light_rgb_mean': b.tolist(),
                  'max_relative_channel_error': error, 'eye_pass': eye['passes'],
                  'light_pass': light['passes'], 'passed': error < .03}
        records.append(record)
        save_metrics()
        print('BSSRDF_SHARP_CHECK', record, flush=True)
        assert error < .03, record


for name, values, camera, spectral in (
        ('flat-gray-ortho', {}, 'orthographic', False),
        ('flat-gray-perspective', {}, 'perspective', False),
        ('flat-colored', {'color': (.55, .2, .08)}, 'perspective', False),
        ('low-ior', {'ior': 1.1}, 'perspective', False),
        ('high-ior', {'ior': 3.8}, 'perspective', False),
        ('forward-hg', {'g': .5}, 'perspective', False),
        ('backward-hg', {'g': -.5}, 'perspective', False),
        ('partial-rgb', {'radius': (0., .3, .3)}, 'perspective', False),
        ('colored-spectral', {'color': (.55, .2, .08)}, 'perspective', True),
        # Most energy uses the initial diffuse escape with no HG collision.
        ('ballistic-heavy', {'color': (.02, .02, .02), 'radius': (4., 4., 4.)}, 'perspective', False)):
    scn = body(values | {'roughness': 0.})
    if camera == 'perspective': perspective(scn)
    compare(name, scn, spectral=spectral)

for name, camera, uv, scale in (
        ('smooth-no-uv-ortho', 'orthographic', False, (1., 1., 1.)),
        ('smooth-no-uv-perspective', 'perspective', False, (1., 1., 1.)),
        ('smooth-uv-perspective', 'perspective', True, (1., 1., 1.)),
        ('nonuniform-instance', 'perspective', False, (1.1, .7, 1.))):
    scn = sphere(body({'roughness': 0., 'color': (.55, .2, .08)}), uv, scale)
    if camera == 'perspective': perspective(scn)
    compare(name, scn)

# A second independent stream checks that the thin instance's no-scatter
# contribution did not disappear behind seemingly close aggregate means.
compare('nonuniform-instance-second-seed',
        perspective(sphere(body({'roughness': 0., 'color': (.55, .2, .08)}), False, (1.1, .7, 1.))), seed=817)

compare('concave-torus', perspective(torus(body({'roughness': 0., 'color': (.55, .2, .08)})),
                                            origin=(0., -4., 2.8)))

# Native sharp continuation is compared with a physically matched independent
# eye path: source -> SSS -> ordinary surface -> camera.
scn = body({'roughness': 0.})
scn.Parse(props({'scene.materials.floor.type': 'matte', 'scene.materials.floor.kd': .6,
                 'scene.materials.floor.id': 123, 'scene.objects.floor.material': 'floor',
                 'scene.lights.env.type': 'constantinfinite', 'scene.lights.env.color': (1., 1., 1.),
                 'scene.lights.env.linkgroups': 'sss-source',
                 # Parse replaces an object definition. Preserve the original
                 # cube mesh, material and numeric identity while linking it.
                 'scene.objects.body.material': 'body',
                 'scene.objects.body.shape': 'InlinedMesh-body', 'scene.objects.body.id': 432,
                 'scene.objects.body.linkgroups': 'sss-source',
                 'scene.objects.floor.linkgroups': 'sss-source', 'scene.objects.floor.linkmode': 'exclude',
                 'scene.objects.floor.vertices': (-4., -4., -1.2, 4., -4., -1.2,
                                                  4., 4., -1.2, -4., 4., -1.2),
                 'scene.objects.floor.faces': (0, 1, 2, 0, 2, 3)}))
perspective(scn, target=(0., 0., -.5), origin=(0., -4., 2.))
compare('sharp-sss-to-floor', scn, materials=(991, 123))

save_metrics(True)
print('BSSRDF_SHARP_COMPLETE', len(records), flush=True)
