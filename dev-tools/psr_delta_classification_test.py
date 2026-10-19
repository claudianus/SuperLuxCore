#!/usr/bin/env python3
"""Validate actual CPU/GPU classification capture from the companion probe.

The captured camera proxy/static predicate is checked separately from dynamic NEE
classification. It does not execute a full light-to-camera connection. BSSRDF cases call real CPU proxy materials.
"""
import argparse
import csv
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('capture', type=Path)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
rows = list(csv.DictReader(a.capture.open()))
assert len(rows) == 44, f'Incomplete capture: {len(rows)} cases'
assert {r['material'] for r in rows} == {
    'glass', 'mirror', 'archglass', 'null', 'matte', 'roughglass',
    'mix', 'twosided', 'GenmaB-glass', 'BSSRDF-diffuse-exit',
    'BSSRDF-sharp-adjoint', 'BSSRDF-rough-adjoint'}
for material in {r['material'] for r in rows} - {'GenmaB-glass', 'BSSRDF-diffuse-exit', 'BSSRDF-sharp-adjoint', 'BSSRDF-rough-adjoint'}:
    captured = [float(r['sigma']) for r in rows if r['material'] == material]
    assert len(captured) == 4 and len(set(captured)) == 4
    assert sum(s > 0 for s in captured) == 2
    assert sum(s == 0 for s in captured) == 1
    assert sum(s < 0 for s in captured) == 1
genmab = [float(r['sigma']) for r in rows if r['material'] == 'GenmaB-glass']
assert len(genmab) == 3 and len(set(genmab)) == 3 and all(s > 0 for s in genmab)
for material in ('BSSRDF-diffuse-exit', 'BSSRDF-sharp-adjoint', 'BSSRDF-rough-adjoint'):
    assert sorted(float(r['sigma']) for r in rows if r['material'] == material) == sorted([-.02999999933, 0., .02999999933])
failures = []
for i, r in enumerate(rows):
    sigma = float(r['sigma'])
    cpu, static, gpu, gpu_static = (int(float(r[k])) for k in
                                   ('cpu_delta', 'cpu_static', 'gpu_delta', 'gpu_static'))
    expected = static and not (sigma > 0 and r['material'] in
                               ('glass', 'mirror', 'GenmaB-glass'))
    checks = {
        'actual_cpu_gpu_match': cpu == gpu,
        'cpu_material_contract': cpu == expected,
        'static_material_preserved': gpu_static == (0 if r['material'].startswith('BSSRDF-') else static),
        'static_camera_eligibility': int(float(r['gpu_camera_eligible'])) == (not static),
        'sigma_roundtrip': float(r['gpu_sigma']) == sigma,
    }
    if not all(checks.values()):
        failures.append({'index': i, 'case': r, 'checks': checks})
summary = {'passed': not failures, 'cases': len(rows), 'failures': failures,
           'scope': __doc__, 'capture': str(a.capture.resolve())}
a.out.write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps({'passed': summary['passed'], 'cases': len(rows), 'failures': len(failures)}))
raise SystemExit(0 if summary['passed'] else 1)
