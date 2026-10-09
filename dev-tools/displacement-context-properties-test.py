"""Cycles true-displacement shader_setup_from_displace wi=N contract.
Expected offsets derive from original world-space smooth normals and explicit
Object/World vector displacement units, independently of exporter behavior.
"""
import importlib.metadata
import json
import os
from pathlib import Path

import numpy as np
import pysuperluxcore as native

assert native.Version() == importlib.metadata.version('pysuperluxcore')


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
        elif parts and parts[0] == 'property' and (element == 'vertex'):
            assert parts[1] != 'list', line
            fields.append((parts[2], types[parts[1]]))
    if 'format ascii' in header:
        values = np.loadtxt(data[offset:].decode('ascii').splitlines()[:vertex_count])
        return values[:, :3].astype(np.float32)
    assert 'format binary_little_endian' in header, header
    values = np.frombuffer(data, dtype=np.dtype(fields), count=vertex_count, offset=offset)
    return np.column_stack([values[n] for n in ('x', 'y', 'z')])

r = Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])
r.mkdir(parents=True, exist_ok=True)
records = []
uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
tri = np.array([[0, 2, 1], [0, 3, 2]], np.uint32)
p = np.array([[-1, 0, -1], [1, 0, -1], [1, 0, 1], [-1, 0, 1]], np.float32)
matrices = {
    'identity': np.eye(4),
    'rotated': np.array([[0, -1, 0, 2], [1, 0, 0, -3], [0, 0, 1, 4], [0, 0, 0, 1.0]]),
    'nonuniform': np.array([[1.4, 0, 0, 2], [0, 0.8, 0, -3], [0, 0, 1.2, 4], [0, 0, 0, 1.0]]),
}
for smooth in ('plane', 'varying'):
    n = np.tile([0, 1, 0], (4, 1)).astype(np.float32)
    if smooth == 'varying':
        n = np.array([[0.1, 1, 0.2], [-0.2, 1, 0.1], [0.1, 1, -0.2], [-0.1, 1, 0.2]], np.float32)
        n /= np.linalg.norm(n, axis=1)[:, None]
    for tag, m in matrices.items():
        world_n = n @ np.linalg.inv(m[:3, :3])
        world_n /= np.linalg.norm(world_n, axis=1)[:, None]
        for space in ('object', 'world'):
            name = f'{smooth}_{tag}_{space}'
            scene = native.Scene()
            scene.DefineMeshExt('base', p.copy(), tri.copy(), n.copy(), [uv.copy()])
            props = native.Properties()
            props.SetFromString(f'''
scene.textures.dir.type = hitpoint
scene.textures.dir.channel = incoming
scene.shapes.result.type = displacement
scene.shapes.result.source = base
scene.shapes.result.map = dir
scene.shapes.result.map.type = vector
scene.shapes.result.map.space = {space}
''')
            props.Set(native.Property('scene.shapes.result.objecttoworld', m.T.ravel().tolist()))
            scene.Parse(props)
            f = r / (name + '.ply')
            scene.SaveMesh('result', str(f))
            delta = load_positions(f) - p
            expected = world_n if space == 'object' else world_n @ np.linalg.inv(m[:3, :3]).T
            error = float(np.max(np.abs(delta - expected)))
            records.append({'case': name, 'passed': error < 2e-06,
                            'max_abs_error': error, 'observed': delta.tolist(),
                            'expected': expected.tolist()})
legacy = []
for smooth in ('plane', 'varying'):
    n = np.tile([0, 1, 0], (4, 1)).astype(np.float32)
    if smooth == 'varying':
        n = np.array([[0.1, 1, 0.2], [-0.2, 1, 0.1], [0.1, 1, -0.2], [-0.1, 1, 0.2]], np.float32)
        n /= np.linalg.norm(n, axis=1)[:, None]
    for tag in ('identity', 'rotated'):
        name = 'legacy_' + smooth + '_' + tag
        scene = native.Scene()
        scene.DefineMeshExt('base', p.copy(), tri.copy(), n.copy(), [uv.copy()])
        props = native.Properties()
        props.SetFromString('''
scene.textures.dir.type = hitpoint
scene.textures.dir.channel = incoming
scene.shapes.result.type = displacement
scene.shapes.result.source = base
scene.shapes.result.map = dir
scene.shapes.result.map.type = vector
''')
        props.Set(native.Property('scene.shapes.result.objecttoworld', matrices[tag].T.ravel().tolist()))
        scene.Parse(props)
        f = r / (name + '.ply')
        scene.SaveMesh('result', str(f))
        positions = load_positions(f)
        baseline = os.environ.get('SUPERLUXCORE_CONTEXT_BASELINE_DIR')
        same = None
        if baseline:
            same = bool(np.array_equal(positions, load_positions(Path(baseline) / (name + '.ply'))))
            assert same, name
        legacy.append({'case': name, 'positions': positions.tolist(), 'baseline_equal': same})
result = {'version': native.Version(), 'checks': records, 'legacy': legacy,
          'passed': sum(x['passed'] for x in records), 'total': len(records),
          'blender_source': '9e2066aef7ef7e20c142ad7bd3303138a4304c93'}
(r / 'metrics.json').write_text(json.dumps(result, indent=2) + '\n')
print('INCOMING_CONTEXT', result['passed'], result['total'], flush=True)
assert all(x['passed'] for x in records), result
