#!/usr/bin/env python3
"""공유 입력 재정의가 기존 복합 텍스처와 CPU·GPU 렌더에 반영되는지 검사한다."""
import math
import sys
import time
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'out/build/src/pysuperluxcore/Release'))
import pysuperluxcore as lux
lux.Init()
for engine in ('PATHCPU','PATHOCL'):
    scene=lux.Scene(); props=lux.Properties(); props.SetFromString('''
scene.camera.lookat.orig = 0 0 3
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 1
scene.textures.leaf.type = constfloat1
scene.textures.leaf.value = 0.125
scene.textures.scale.type = scale
scene.textures.scale.texture1 = leaf
scene.textures.scale.texture2 = 2
scene.textures.add.type = add
scene.textures.add.texture1 = scale
scene.textures.add.texture2 = 0.1
scene.textures.subtract.type = subtract
scene.textures.subtract.texture1 = add
scene.textures.subtract.texture2 = 0.1
scene.textures.divide.type = divide
scene.textures.divide.texture1 = subtract
scene.textures.divide.texture2 = 2
scene.textures.dot.type = dotproduct
scene.textures.dot.texture1 = divide
scene.textures.dot.texture2 = 1 0 0
scene.textures.power.type = power
scene.textures.power.base = dot
scene.textures.power.exponent = 2
scene.textures.unary.type = mathfunc
scene.textures.unary.op = cos
scene.textures.unary.texture1 = scale
scene.textures.binary.type = mathfunc
scene.textures.binary.op = max
scene.textures.binary.texture1 = power
scene.textures.binary.texture2 = unary
scene.textures.normal.type = normalmap
scene.textures.normal.texture = leaf
scene.textures.fresnel.type = fresnelcolor
scene.textures.fresnel.kr = leaf
scene.textures.result.type = makefloat3
scene.textures.result.texture1 = power
scene.textures.result.texture2 = unary
scene.textures.result.texture3 = binary
scene.materials.emitter.type = matte
scene.materials.emitter.kd = 0
scene.materials.emitter.emission = result
scene.objects.plane.material = emitter
scene.objects.plane.vertices = -2 -2 0 2 -2 0 2 2 0 -2 2 0
scene.objects.plane.faces = 0 1 2 0 2 3
'''); scene.Parse(props)
    config=lux.Properties(); config.SetFromString(f'''
renderengine.type = {engine}
film.width = 32
film.height = 32
film.filter.type = NONE
film.imagepipelines.0.0.type = NOP
batch.haltspp = 8
sampler.type = SOBOL
opencl.devices.select = 010
opencl.native.threads.count = 0
path.lighttracing.enable = 0
path.mnee.enable = 0
''')
    for value in (.125,.25,.5):
        update=lux.Properties();update.SetFromString(f'scene.textures.leaf.type = constfloat1\nscene.textures.leaf.value = {value}')
        scene.Parse(update)
        session=lux.RenderSession(lux.RenderConfig(config,scene));session.Start()
        try:
            deadline=time.monotonic()+180
            while not session.HasDone():
                if time.monotonic()>deadline:raise TimeoutError(engine)
                time.sleep(.1);session.UpdateStats()
        finally:session.Stop()
        buf=np.empty(32*32*3,dtype=np.float32);session.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE,buf,0,True)
        actual=buf.reshape(32,32,3)[12:20,12:20].mean((0,1));expected=np.array((value**2,math.cos(2*value),max(value**2,math.cos(2*value))))
        assert np.isfinite(actual).all() and np.max(np.abs(actual-expected))<.0001,(engine,value,actual,expected)
        print('PASS 재정의',engine,value,actual,'기대값',expected,flush=True)
