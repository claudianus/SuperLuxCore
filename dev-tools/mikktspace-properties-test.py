"""Verify the native Mikk basis, mirrored orientation and input boundaries."""
import json
import os
from pathlib import Path

import numpy as np
import pysuperluxcore as native

points = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]], np.float32)
triangles = np.array([[0, 1, 2]], np.uint32)
normals = np.tile([0, 0, 1], (1, 3, 1)).astype(np.float32)
uv = np.array([[[0, 0], [1, 0], [0, 1]]], np.float32)
records = []


def basis(name, coords, direction, sign, tolerance=1e-5, **kwargs):
    before = [value.copy() for value in (points, triangles, normals)]
    tangents, signs = native.ComputeMikkTangents(points, triangles, normals, coords, **kwargs)
    assert tangents.shape == (1, 3, 3) and signs.shape == (1, 3)
    assert np.allclose(tangents, direction, atol=tolerance) and np.all(signs == sign), (tangents, signs)
    assert all(np.array_equal(a, b) for a, b in zip(before, (points, triangles, normals)))
    records.append({'case': name, 'passed': True})


basis('uv_basis', uv, [1, 0, 0], 1)
mirrored = uv.copy()
mirrored[:, :, 0] *= -1
basis('mirrored_orientation', mirrored, [-1, 0, 0], -1)
basis('flat_uses_geometry_normal', uv, [1, 0, 0], 1, smooth=[False])
# Even +Z moves by one half step after octahedral quantization. Bound this by
# one full 16-bit step rather than requiring the unquantized tangent exactly.
basis('packed_cycles_normal', uv, [1, 0, 0], 1, tolerance=2 / 65535, cycles_normal_precision=True)
for name, changed, message in [
        ('bad_points_shape', {'points': np.zeros((3, 2))}, 'shape'),
        ('bad_corner_shape', {'normals': np.zeros((3, 3))}, 'normals'),
        ('out_of_range_vertex', {'triangles': [[0, 1, 3]]}, 'index'),
        ('negative_vertex', {'triangles': np.array([[0, 1, -1]], np.int32)}, 'index'),
        ('nonfinite_position', {'points': np.full((3, 3), np.nan)}, 'finite'),
        ('nonfinite_normal', {'normals': np.full((1, 3, 3), np.inf)}, 'finite'),
        ('bad_uv_shape', {'uv': np.zeros((3, 2))}, 'UVs'),
        ('nonfinite_uv', {'uv': np.full((1, 3, 2), np.nan)}, 'finite'),
        ('bad_smoothing_shape', {'smooth': [[True]]}, 'smoothing')]:
    args = {'points': points, 'triangles': triangles, 'normals': normals, 'uv': uv, **changed}
    try:
        native.ComputeMikkTangents(**args)
    except RuntimeError as error:
        assert message in str(error), (name, error)
    else:
        raise AssertionError(name)
    records.append({'case': name, 'passed': True})
folder = Path(os.environ.get('SUPERLUXCORE_AUDIT_DIR', '/tmp/slc-mikk-properties'))
folder.mkdir(parents=True, exist_ok=True)
(folder / 'metrics.json').write_text(json.dumps({'version': native.Version(), 'checks': records}, indent=2) + '\n')
print('MIKK_PROPERTIES_PASS', native.Version(), len(records), flush=True)
