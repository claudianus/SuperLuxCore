"""Real Metal smoke for optional handlers after reachable-only PSO selection."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import numpy as np

sys.path.insert(0,os.environ['SLC_REVIEW_MODULE_ROOT'])
import pysuperluxcore as slc
slc.Init()
OUT=Path(os.environ['SLC_REVIEW_OUT']);OUT.mkdir(parents=True,exist_ok=True)
identity=json.loads((OUT/'candidate-identity.json').read_text())
assert hashlib.file_digest(Path(slc.pysuperluxcore.__file__).open('rb'),'sha256').hexdigest()==identity['installed_native_sha256']
def props(text):
    p=slc.Properties();p.SetFromString(text);return p
desc=slc.GetOpenCLDeviceDescs()
mask=''.join('1' if desc.Get(n+'.type').GetString()=='METAL_GPU' else '0' for n in desc.GetAllUniqueSubNames('opencl.device'))
assert '1' in mask
records=[]
for label,extra in [('visibility','lightstrategy.type = RESTIR_DI\nlightstrategy.restir.visibility.enable = 1'),
                    ('GI','path.restir.gi.enable = 1'),('PT','path.restir.pt.enable = 1')]:
    scene=slc.Scene();scene.Parse(props('''
scene.camera.lookat.orig = 0 -4 2
scene.camera.lookat.target = 0 0 0.6
scene.camera.up = 0 0 1
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.6 0.6 0.6
scene.objects.floor.material = floor
scene.objects.floor.vertices = -10 -10 0 10 -10 0 10 10 0 -10 10 0
scene.objects.floor.faces = 0 1 2 0 2 3
scene.objects.wall.material = floor
scene.objects.wall.vertices = -2 1 0 2 1 0 2 1 3 -2 1 3
scene.objects.wall.faces = 0 1 2 0 2 3
scene.lights.env.type = constantinfinite
scene.lights.env.color = 0.1 0.1 0.1
scene.lights.key.type = point
scene.lights.key.position = 0 -1 2
scene.lights.key.color = 3 3 3
'''))
    cfg=props(f'''
renderengine.type = PATHOCL
opencl.devices.select = {mask}
opencl.native.threads.count = 0
opencl.task.count = 8192
film.width = 64
film.height = 64
film.imagepipelines.0.0.type = NOP
sampler.type = RANDOM
renderengine.seed = 42
batch.haltspp = 8
path.lighttracing.auto = 0
path.hybridbackforward.enable = 0
path.mnee.enable = 0
path.mnee.auto = 0
path.guiding.enable = 0
path.photongi.indirect.enabled = 0
path.photongi.caustic.enabled = 0
{extra}
''')
    session=slc.RenderSession(slc.RenderConfig(cfg,scene));started=False
    try:
        print('OPTIONAL',label,flush=True);session.Start();started=True
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            session.UpdateStats()
            if session.GetStats().Get('stats.renderengine.pass').GetInt()>=8:break
            time.sleep(.02)
        else:raise TimeoutError(label+' made no progress')
        data=np.zeros(64*64*3,np.float32)
        session.GetFilm().GetOutputFloat(slc.FilmOutputType.RGB_IMAGEPIPELINE,data,0,True)
        assert np.isfinite(data).all() and 0.001<float(data.mean())<100
        stats=session.GetStats()
        devices={n:stats.Get(n).GetString() for n in stats.GetAllNames()
                 if n.startswith('stats.renderengine.devices.') and n.endswith('.type')}
        assert any(value=='METAL_GPU' for value in devices.values()),devices
        assert not any(value=='NATIVE_THREAD' for value in devices.values()),devices
        records.append({'case':label,'finite':True,'mean':float(data.mean()),'status':'passed',
            'native_sha256':identity['installed_native_sha256'],'devices':devices})
        (OUT/'optional-gpu.json').write_text(json.dumps(records,indent=2)+'\n')
    finally:
        if started:session.Stop()
print(json.dumps(records),flush=True)
