#!/usr/bin/env python3
"""불투명 표면 뒤의 카메라 매질을 Beer–Lambert 독립식으로 검증한다."""
import math
import sys
import time
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore as lux


def render(engine, camera_z, boundary_z):
    scene = lux.Scene()
    props = lux.Properties()
    props.SetFromString(f"""
scene.camera.lookat.orig = 0 0 {camera_z}
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 0.5
scene.volumes.fog.type = clear
scene.volumes.fog.absorption = 0.4 0.4 0.4
scene.materials.boundary.type = null
scene.materials.boundary.volume.interior = fog
scene.objects.fog.material = boundary
scene.objects.fog.vertices = -2 -2 {boundary_z} 2 -2 {boundary_z} 2 2 {boundary_z} -2 2 {boundary_z} -2 -2 3 2 -2 3 2 2 3 -2 2 3
scene.objects.fog.faces = 0 2 1 0 3 2 4 5 6 4 6 7 0 1 5 0 5 4 1 2 6 1 6 5 2 3 7 2 7 6 3 0 4 3 4 7
scene.materials.emitter.type = matte
scene.materials.emitter.kd = 0 0 0
scene.materials.emitter.emission = 1 1 1
scene.objects.emitter.material = emitter
scene.objects.emitter.vertices = -2 -2 0.001 2 -2 0.001 2 2 0.001 -2 2 0.001
scene.objects.emitter.faces = 0 1 2 0 2 3
""")
    scene.Parse(props)
    props = lux.Properties()
    props.SetFromString(f"""
renderengine.type = {engine}
film.width = 64
film.height = 64
film.filter.type = NONE
film.imagepipelines.0.0.type = NOP
batch.haltspp = 16
sampler.type = SOBOL
opencl.devices.select = 010
opencl.native.threads.count = 0
path.lighttracing.enable = 0
path.mnee.enable = 0
path.hybridbackforward.enable = 0
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
    rgb = np.empty(64 * 64 * 3, dtype=np.float32)
    session.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    value = rgb.reshape(64, 64, 3)[24:40, 24:40].mean()
    # 광선 시작점의 클리핑만큼 흡수 거리가 짧아진다.
    distance = max(0., min(camera_z - .001, 3.) - max(.001, boundary_z))
    expected = math.exp(-.4 * distance)
    assert np.isfinite(rgb).all() and abs(value - expected) < .003, (engine, camera_z, boundary_z, value, expected)
    print(f"PASS {engine} camera={camera_z:g} boundary={boundary_z:g}: {value:.6f} expected={expected:.6f}", flush=True)


lux.Init()
for engine in ("PATHCPU", "PATHOCL"):
    for camera_z, boundary_z in ((1., 0.), (1., -.2), (5., 0.), (1., 2.)):
        render(engine, camera_z, boundary_z)
