#!/usr/bin/env python3
"""독립적인 높이 기울기로 범프 합성의 CPU·Metal 결과를 검증한다."""
import array
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore as lux


def render(engine, operation):
    scene = lux.Scene()
    props = lux.Properties()
    props.SetFromString(f"""
scene.camera.lookat.orig = 0 0 3
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 5
scene.objects.plane.vertices = -10 -10 0 10 -10 0 10 10 0 -10 10 0
scene.objects.plane.faces = 0 1 2 0 2 3
scene.objects.plane.material = mat
scene.materials.mat.type = matte
scene.materials.mat.kd = 0.5
scene.materials.mat.bumptex = combined
scene.materials.mat.bumpsamplingdistance = 0.001
scene.textures.position.type = hitpoint
scene.textures.position.channel = worldpos
scene.textures.a.type = dotproduct
scene.textures.a.texture1 = position
scene.textures.a.texture2 = 7 0 0
scene.textures.b.type = dotproduct
scene.textures.b.texture1 = position
scene.textures.b.texture2 = 0 3 0
scene.textures.combined.type = {operation}
scene.textures.combined.texture1 = a
scene.textures.combined.texture2 = b
scene.textures.combined.amount = 0.25
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1 1 1
""")
    scene.Parse(props)
    props = lux.Properties()
    props.SetFromString(f"""
renderengine.type = {engine}
film.width = 64
film.height = 64
sampler.type = SOBOL
batch.haltspp = 8
opencl.devices.select = 010
opencl.native.threads.count = 0
path.lighttracing.enable = 0
path.mnee.enable = 0
film.outputs.0.type = SHADING_NORMAL
film.outputs.0.filename = /tmp/superluxcore-bump-normal.exr
""")
    session = lux.RenderSession(lux.RenderConfig(props, scene))
    session.Start()
    try:
        deadline = time.monotonic() + 180
        while not session.HasDone():
            if time.monotonic() > deadline:
                raise TimeoutError(engine)
            time.sleep(.1)
            session.UpdateStats()
    finally:
        session.Stop()
    buf = array.array("f", [0.] * (64 * 64 * 3))
    session.GetFilm().GetOutputFloat(lux.FilmOutputType.SHADING_NORMAL, buf)
    normal = np.asarray(buf).reshape(64, 64, 3)[30:34, 30:34].mean((0, 1))
    # 높이 h(x,y)의 해석적 기울기로 기대 법선을 계산한다.
    slopes = {"add": (7., 3.), "subtract": (7., -3.), "mix": (5.25, .75)}
    dx, dy = slopes[operation]
    expected = np.array([-dx, -dy, 1.])
    expected /= np.linalg.norm(expected)
    error = np.max(np.abs(normal - expected))
    assert np.isfinite(normal).all() and error < .005, (engine, operation, normal, expected)
    print(f"PASS {engine}/{operation}: error={error:.6f}, normal={normal}", flush=True)


lux.Init()
for engine in ("PATHCPU", "PATHOCL"):
    for operation in ("add", "subtract", "mix"):
        render(engine, operation)
