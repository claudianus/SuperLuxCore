"""Verify displacement geometry, native defaults and invalid SDL data.

Run once with the released baseline and once with the candidate; the legacy
positions must be equal. Imported space controls follow Cycles displace.h.
"""
import importlib.metadata
import json
import os
from pathlib import Path

import numpy as np
import pysuperluxcore as native

assert native.Version() == importlib.metadata.version('pysuperluxcore')
folder = Path(os.environ.get('SUPERLUXCORE_AUDIT_DIR', '/tmp/slc-displacement-properties'))
folder.mkdir(parents=True, exist_ok=True)
points = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]], np.float32)
triangles = np.array([[0, 1, 2], [0, 2, 3]], np.uint32)
normals = np.tile([0, 0, 1], (4, 1)).astype(np.float32)
uvs = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
records = []


def load_positions(path):
    data = path.read_bytes()
    offset = data.index(b'end_header')
    offset = data.index(b'\n', offset) + 1
    header = data[:offset].decode('ascii')
    vertex_count = 0
    fields = []
    element = None
    types = {'float': '<f4', 'float32': '<f4', 'double': '<f8',
             'uchar': 'u1', 'uint8': 'u1', 'char': 'i1', 'int8': 'i1',
             'uint': '<u4', 'uint32': '<u4', 'int': '<i4', 'int32': '<i4',
             'short': '<i2', 'ushort': '<u2'}
    for line in header.splitlines():
        parts = line.split()
        if parts and parts[0] == 'element':
            element = parts[1]
            if element == 'vertex':
                vertex_count = int(parts[2])
        elif parts and parts[0] == 'property' and element == 'vertex':
            assert parts[1] != 'list', line
            fields.append((parts[2], types[parts[1]]))
    if 'format ascii' in header:
        values = np.loadtxt(data[offset:].decode('ascii').splitlines()[:vertex_count])
        return values[:, :3].astype(np.float32)
    assert 'format binary_little_endian' in header, header
    values = np.frombuffer(data, dtype=np.dtype(fields), count=vertex_count, offset=offset)
    return np.column_stack([values[n] for n in ('x', 'y', 'z')])


def check(name, params, delta=None, *, uv=True, identity=False, rejected=None):
    p = points.copy()
    tri = triangles.copy()
    n = normals.copy()
    uv_values = [uvs.copy()] if uv else None
    colors = [normals.copy(), np.tile([1, 0, 0], (4, 1)).astype(np.float32)]
    alphas = [np.ones(4, np.float32)]
    scene = native.Scene()
    if identity:
        p = np.vstack([p, p])
        tri = np.vstack([tri, tri + 4])
        n = np.vstack([n, n])
        uv_values = [np.vstack([uvs, uvs])]
        colors = [np.vstack([np.tile([.3, .6, .9], (4, 1)), np.tile([-.3, -.6, -.9], (4, 1))]).astype(np.float32)]
        alphas = None
    scene.DefineMeshExt('base', p, tri, n, uv_values, colors, alphas)
    if identity or any(k.startswith('map.vertexid') for k in params):
        size = len(p)
        low = (np.arange(size) % 4).astype(np.float32)
        high = np.full(size, 57005, np.float32)
        if name == 'invalid_identity_fraction': low[0] = .5
        if name == 'invalid_identity_negative': low[0] = -1
        if name == 'invalid_identity_high': high[0] = 65536
        if name == 'invalid_identity_nan': low[0] = np.nan
        scene.SetMeshVertexAOV('base', 0, low.tolist())
        scene.SetMeshVertexAOV('base', 1, high.tolist())
    props = native.Properties()
    props.SetFromString('scene.shapes.result.type = displacement\nscene.shapes.result.source = base\n')
    for key, value in params.items():
        props.Set(native.Property('scene.shapes.result.' + key, value))
    if identity:
        props.SetFromString('scene.textures.offset.type = hitpointcolor\nscene.textures.offset.index = 0\n')
    try:
        scene.Parse(props)
    except RuntimeError as error:
        assert rejected and rejected in str(error), (name, error)
        records.append({'case': name, 'passed': True, 'expected_rejection': str(error)})
        return
    assert rejected is None, name
    path = folder / (name + '.ply')
    scene.SaveMesh('result', str(path))
    actual = load_positions(path)
    assert np.allclose(actual, p + delta, rtol=0, atol=1e-6), (name, actual, p + delta)
    records.append({'case': name, 'passed': True, 'positions': actual.tolist()})


check('legacy_height', {'map': .3, 'scale': .5, 'offset': -.05}, [0, 0, .1])
check('legacy_vector', {'map': [.3, .6, .9], 'map.type': 'vector'}, [.9, .3, .6])
check('legacy_vector_no_uv', {'map': [.3, .6, .9], 'map.type': 'vector'}, uv=False,
      rejected='only with mesh having UVs')
if native.Version() != '2.11.21':
    matrix = [1.4, 0, 0, 0, 0, .8, 0, 0, 0, 0, 1.2, 0, 0, 0, 0, 1]
    base = {'map': [.3, .6, .9], 'map.type': 'vector', 'objecttoworld': matrix}
    check('object', {**base, 'map.space': 'object'}, [.3, .6, .9])
    check('world', {**base, 'map.space': 'world'}, [.3 / 1.4, .6 / .8, .9 / 1.2])
    check('tangent', {**base, 'map.space': 'tangent', 'map.normalindex': 0,
                      'map.tangentindex': 1, 'map.signindex': 0}, [.3, .9, .6])
    check('object_no_uv', {**base, 'map.space': 'object'}, [.3, .6, .9], uv=False)
    fallback = np.tile([.3, .9, .6], (4, 1))
    t = np.array([1.4, .8, 0]) / np.linalg.norm([1.4, .8, 0])
    fallback[3] = .3 * t + .9 * np.array([-t[1], t[0], 0]) + [0, 0, .6]
    check('tangent_no_attributes', {**base, 'map.space': 'tangent'}, fallback, uv=False)
    check('invalid_space', {**base, 'map.space': 'unknown'}, rejected='Unknown displacement map space')
    check('invalid_data_index', {**base, 'map.space': 'object', 'map.normalindex': 8}, rejected='Invalid displacement data index')
    ids = {'map.space': 'object', 'map.type': 'vector', 'map.vertexidlowindex': 0, 'map.vertexidhighindex': 1}
    check('original_vertex_first_corner', {**ids, 'map': 'offset', 'normalsmooth': False}, [.3, .6, .9], identity=True)
    for name in ['invalid_identity_fraction', 'invalid_identity_negative', 'invalid_identity_high', 'invalid_identity_nan']:
        check(name, {**ids, 'map': [.3, .6, .9]}, rejected='Invalid displacement vertex identity')
    check('incomplete_identity', {**base, 'map.space': 'object', 'map.vertexidlowindex': 0}, rejected='requires both data channels')
(folder / 'metrics.json').write_text(json.dumps({'version': native.Version(), 'checks': records}, indent=2) + '\n')
print('DISPLACEMENT_PROPERTIES_PASS', native.Version(), len(records), flush=True)
