"""ABBA PSO selection diagnostic; serial runs, private namespaces, bounded children.

macOS internal compiler caches are not erased. New entry-point names isolate
each run's pipeline identity; per-kernel archive misses and timings are retained.
This measures the selected PSO set, not end-to-end Blender startup.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT.parent / 'dev-tools/reviews/2026-10-16/metal-startup'
SOURCE = Path.home()/'Library/Caches/LuxCoreRender/metal/6e90d10d23251745.msl'
records=[]
with tempfile.TemporaryDirectory(prefix='slc-pso-abba-') as temp:
    temp=Path(temp); exe=temp/'probe'
    subprocess.run(['clang++','-std=c++17','-O2','-fno-objc-arc','-framework','Metal','-framework','Foundation',
                    str(ROOT/'dev-tools/review_metal_pso_probe.mm'),'-o',str(exe)],check=True,timeout=30)
    for label,all_kernels in [('A1',1),('B1',0),('B2',0),('A2',1)]:
        admission=subprocess.run(['ps','-axo','pid,ppid,%cpu,rss,command'],check=True,capture_output=True,text=True).stdout
        (OUT/(label+'-admission.txt')).write_text(admission)
        blockers=[line for line in admission.splitlines() if ('Blender.app/Contents/MacOS/Blender' in line
            or 'MTLCompilerService' in line or 'mds_stores' in line) and float(line.split(None,4)[2])>30]
        if blockers: raise RuntimeError('resource contention: '+str(blockers))
        prefix='Probe'+uuid.uuid4().hex[:12]+'_'
        msl='#include <metal_stdlib>\n#include <metal_atomic>\n\n'+re.sub(
            r'\b(AdvancePaths_[A-Za-z0-9_]+|Film_Clear|InitSeed|Init)\b',lambda m:prefix+m[0],SOURCE.read_text())
        path=temp/'source.metal'; path.write_text(msl)
        air=temp/'source.air'; lib=temp/'source.metallib'
        t=time.monotonic()
        with (OUT/(label+'-front-end.log')).open('w') as log:
            subprocess.run(['xcrun','metal','-x','metal','-std=macos-metal2.4','-ffast-math','-c',str(path),'-o',str(air)],
                stdout=log,stderr=subprocess.STDOUT,check=True,timeout=45)
            subprocess.run(['xcrun','metallib',str(air),'-o',str(lib)],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=20)
        prep=time.monotonic()-t
        print('RUN',label,'kernels',19 if all_kernels else 14,flush=True)
        with (OUT/(label+'-pso.jsonl')).open('w') as log, (OUT/(label+'-pso-stderr.log')).open('w') as err:
            run=subprocess.run([str(exe),str(lib),prefix,str(all_kernels),label],stdout=log,stderr=err,timeout=240)
        rows=[json.loads(line) for line in (OUT/(label+'-pso.jsonl')).read_text().splitlines()]
        assert run.returncode==0 and all(r['ok'] for r in rows)
        row={'label':label,'all_kernels':bool(all_kernels),'prefix':prefix,'front_end_s':prep,
             'source_msl_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
             'namespaced_msl_sha256':hashlib.sha256(msl.encode()).hexdigest(),**rows[-1]}
        records.append(row); (OUT/'pso-abba.json').write_text(json.dumps(records,indent=2)+'\n')
        print(row,flush=True)
        for item in (path,air,lib): item.unlink()
