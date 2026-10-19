"""Analyze actual CPU/Metal camera captures without hiding remaining gaps.

Checks epsilon, unit direction and full origin/direction/far-clip parity for
all 108 sampled cases. Historical failing captures require --observe; the
default gate requires every sampled ray pair to pass.
"""
import argparse
import csv
import json
import math
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('csv',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--observe',action='store_true');a=p.parse_args()
with a.csv.open() as f:rows=list(csv.DictReader(f))
assert len(rows)==216
report=[]
for c,g in zip(rows[::2],rows[1::2]):
    assert c['source']=='CPU' and g['source']=='GPU'
    assert all(c[k]==g[k] for k in ['camera','scenario','sample'])
    near=math.isclose(float(c['mint']),float(g['mint']),rel_tol=2e-5,abs_tol=1e-7)
    norm=lambda r:math.sqrt(sum(float(r[k])**2 for k in ['dx','dy','dz']))
    unit=abs(norm(g)-1)<5e-6
    differences=[]
    for k in ['ox','oy','oz','dx','dy','dz','maxt','time']:
        tolerance=1e-4 if k in ['ox','oy','oz'] else 3e-6
        if not math.isclose(float(c[k]),float(g[k]),rel_tol=3e-6,abs_tol=tolerance):differences.append(k)
    report.append({'camera':c['camera'],'scenario':int(c['scenario']),'sample':int(c['sample']),
                   'mint_contract_passed':near,'GPU_unit_direction_passed':unit,
                   'remaining_CPU_GPU_ray_differences':differences,'full_ray_parity':near and unit and not differences,
                   'CPU_mint':float(c['mint']),'GPU_mint':float(g['mint']),'GPU_direction_length':norm(g)})
result={'repaired_contracts_passed':all(r['mint_contract_passed'] and r['GPU_unit_direction_passed'] for r in report),
        'full_camera_parity_passed':all(r['full_ray_parity'] for r in report),'samples':len(report),
        'mint_failed':sum(not r['mint_contract_passed'] for r in report),
        'direction_norm_failed':sum(not r['GPU_unit_direction_passed'] for r in report),
        'remaining_full_parity_failed':sum(not r['full_ray_parity'] for r in report),'records':report,'scope':__doc__}
a.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ['records','scope']}),flush=True)
if not a.observe:assert result['full_camera_parity_passed'],'CPU/Metal primary camera rays differ'
