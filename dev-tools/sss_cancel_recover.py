"""Cancel a dense CPU walk, then verify a new session has a clean film."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import time
import numpy as np

ap=argparse.ArgumentParser()
ap.add_argument('--module',required=True)
ap.add_argument('--out',type=Path,required=True)
a=ap.parse_args();a.out=a.out.resolve();sys.path.insert(0,str(Path(a.module).resolve()))
import pysuperluxcore as s
s.Init();root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('fixture',root/'dev-tools/e53_sss_energy.py')
fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
os.chdir(root)
def props(text):
    p=s.Properties();p.SetFromString(text);return p
def session(size,aa):
    scene=s.Scene();scene.Parse(props(fixture.SCENE))
    return s.RenderSession(s.RenderConfig(props(f'''
renderengine.type = TILEPATHCPU
renderengine.seed = 17
sampler.type = TILEPATHSAMPLER
native.threads.count = 1
film.width = {size}
film.height = {size}
film.filter.type = NONE
film.hw.enable = 0
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = RGB
film.outputs.0.filename = unused.exr
tile.size = 8
tile.multipass.enable = 0
tilepath.sampling.aa.size = {aa}
path.spectral.enable = 0
path.lighttracing.auto = 0
path.lighttracing.enable = 0
path.hybridbackforward.enable = 0
path.mnee.auto = 0
path.clamping.variance.maxvalue = 0
'''),scene))
cancelled=session(64,64);cancelled.Start();time.sleep(.1)
assert not cancelled.HasDone()
t=time.monotonic();cancelled.Stop();stopped=time.monotonic()-t
assert not cancelled.IsStarted() and stopped<2,stopped
del cancelled
images=[]
for _ in range(2):
    renderer=session(8,4)
    try:
        renderer.Start();deadline=time.monotonic()+30
        while not renderer.HasDone():
            if time.monotonic()>deadline:raise TimeoutError()
            time.sleep(.01);renderer.UpdateStats()
        renderer.Stop();image=np.empty((8,8,3),np.float32)
        renderer.GetFilm().GetOutputFloat(s.FilmOutputType.RGB,image,0,False)
        assert np.isfinite(image).all();images.append(image)
    finally:
        if renderer.IsStarted():renderer.Stop()
assert np.array_equal(*images)
native=Path(getattr(s,'pysuperluxcore',s).__file__)
result=dict(stop_seconds=stopped,recovery_bit_exact=True,
            native_sha256=hashlib.sha256(native.read_bytes()).hexdigest())
a.out.mkdir(parents=True,exist_ok=True)
(a.out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
