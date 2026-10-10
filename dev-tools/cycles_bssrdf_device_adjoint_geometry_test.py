# SPDX-License-Identifier: Apache-2.0
"""Curved/smooth device adjoint geometry against an independent CPU eye walk.

Uses the same isolated wheel and fixed 3% / 8192 eye / 65536 light gates as
cycles_bssrdf_device_adjoint_test.py. Only fixture helper definitions are
reused; that long suite is not re-executed. Existing CPU contracts run first.
These small uniform-coefficient fixtures are not production image acceptance.
"""
import ast
import hashlib
import json
import math
import os
from pathlib import Path
import runpy
import struct
import time

import numpy as np

repo = Path(__file__).resolve().parents[1]
ns = runpy.run_path(str(repo / 'dev-tools/cycles_bssrdf_transport_test.py'))
slc, props, scene = ns['slc'], ns['properties'], ns['scene']
identity = ns['identity']
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR']) / 'device-adjoint-geometry'
folder.mkdir(parents=True, exist_ok=True)
W = H = 32
center = np.s_[6:26, 6:26]
GATE, EYE_SPP, LIGHT_SPP = .03, 8192, 65536
records = []
helpers = {}


def load_helpers(filename, names):
    path = repo / 'dev-tools' / filename
    helpers[filename] = hashlib.sha256(path.read_bytes()).hexdigest()
    tree = ast.parse(path.read_text())
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
    assert {n.name for n in nodes} == set(names)
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), globals())
    return tree


hash_ns = {'struct': struct}
crypto_tree = ast.parse((repo / 'dev-tools/e40_cryptomatte_test.py').read_text())
crypto_nodes = [n for n in crypto_tree.body if isinstance(n, ast.FunctionDef) and
               n.name in ('murmur3_32', 'hash_to_float', 'name_id')]
exec(compile(ast.Module(body=crypto_nodes, type_ignores=[]), '<crypto-reference>', 'exec'), hash_ns)
tree = load_helpers('cycles_bssrdf_device_adjoint_test.py',
                    ('body', 'config', 'render', 'compare', 'identities'))
base_index = next(i for i, n in enumerate(tree.body) if isinstance(n, ast.Assign) and
                  any(isinstance(t, ast.Name) and t.id == 'BASE' for t in n.targets))
base_nodes = tree.body[base_index:base_index + 2]
assert isinstance(base_nodes[1], ast.For)
exec(compile(ast.Module(body=base_nodes, type_ignores=[]), '<shared-device-config>', 'exec'), globals())
load_helpers('cycles_bssrdf_sharp_test.py', ('sphere', 'torus', 'perspective', 'interior_mask'))


def save(complete=False):
    (folder / 'metrics.json').write_text(json.dumps({
        'native_sha256': identity['native_sha256'],
        'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'helpers': helpers, 'cpu_contracts': len(ns['records']),
        'production_acceptance': False, 'gate': GATE,
        'minimum_eye_samples': EYE_SPP, 'minimum_light_samples': LIGHT_SPP,
        'complete': complete, 'records': records}, indent=2) + '\n')


def record(case, **data):
    records.append({'case': case, 'passed': True, **data})
    save()
    print('BSSRDF_DEVICE_GEOMETRY_PASS', case, data, flush=True)


for case, scn in (
        ('smooth-orthographic', sphere(body())),
        ('smooth-uv-perspective', perspective(sphere(body(), True))),
        ('ellipsoid-perspective', perspective(sphere(body(), False, (1.1, .7, 1.)))),
        ('concave-torus-perspective', perspective(torus(body())))):
    (folder / (case + '-scene.scn')).write_text(scn.ToProperties().ToString())
    eye, eye_pass = render(scn, case + '-cpu-eye', 'PATHCPU')
    mask = interior_mask(eye, 991)
    identities(eye)
    for mode in ('off', 'on'):
        light, light_pass = render(scn, case + '-' + mode, mode=mode)
        identities(light)
        compare(case + '-' + mode, eye, light, eye_pass, light_pass, mask=mask)

save(complete=True)
print('BSSRDF_DEVICE_GEOMETRY_COMPLETE', len(records), flush=True)
