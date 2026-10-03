#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render baked-reflection normals on CPU and a GPU without hybrid CPU workers.

PYTHONPATH=out/build/src/pysuperluxcore/Release python3.13 \
    dev-tools/shading-handedness-regression.py --gpu-devices 010

The GPU device mask is configurable; 010 selects Metal on this Apple host.
"""
import argparse
import array
import math
import time

import pysuperluxcore as lux


def render(engine, kind, object_reflection, devices):
    scene = lux.Scene()
    # Positions and normals already have an X reflection baked into them.
    scene.DefineMesh(
        "plane", [(4., -4., 0.), (-4., -4., 0.), (-4., 4., 0.), (4., 4., 0.)],
        [(0, 1, 2), (0, 2, 3)], [(-.6, 0., .8)] * 4,
        [(0., 0.), (1., 0.), (1., 1.), (0., 1.)], None, None)
    reflection = [-1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1.]
    scene.SetMeshAppliedTransformation("plane", reflection)
    transform = [1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1.]
    if object_reflection:
        transform[0] = -.5
        transform[10] = 2.
    text = f"""scene.camera.lookat.orig = 0 0 {3 if object_reflection else -3}
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 40
scene.materials.mat.type = matte
scene.materials.mat.kd = 0.5
scene.objects.obj.shape = plane
scene.objects.obj.material = mat
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1 1 1
"""
    if kind == "instance":
        text += "scene.objects.obj.transformation = " + " ".join(map(str, transform)) + "\n"
    elif kind == "motion":
        for step in range(2):
            transform[12] = step * .1
            text += f"scene.objects.obj.motion.{step}.time = {step}\n"
            text += f"scene.objects.obj.motion.{step}.transformation = " + " ".join(map(str, transform)) + "\n"
    props = lux.Properties()
    props.SetFromString(text)
    scene.Parse(props)
    props = lux.Properties()
    props.SetFromString(f"""renderengine.type = {engine}
film.width = 32
film.height = 32
sampler.type = SOBOL
batch.haltspp = 8
opencl.devices.select = {devices}
film.outputs.0.type = SHADING_NORMAL
film.outputs.0.filename = shading-normal.exr
""")
    if engine == "PATHOCL":
        props.Set(lux.Property("opencl.native.threads.count", 0))
    session = lux.RenderSession(lux.RenderConfig(props, scene))
    session.Start()
    try:
        deadline = time.monotonic() + 120
        while not session.HasDone():
            if time.monotonic() > deadline:
                raise TimeoutError(f"{kind}/{engine} rendering did not finish")
            time.sleep(.1)
            session.UpdateStats()
    finally:
        session.Stop()
    buf = array.array("f", [0.] * (32 * 32 * 3))
    session.GetFilm().GetOutputFloat(lux.FilmOutputType.SHADING_NORMAL, buf)
    expected = [.6, 0., -.8]
    if object_reflection:
        length = math.sqrt(1.2 ** 2 + .4 ** 2)
        expected = [1.2 / length, 0., .4 / length]
    for y in range(14, 18):
        for x in range(14, 18):
            normal = buf[3 * (y * 32 + x):3 * (y * 32 + x) + 3]
            assert all(math.isfinite(n) and abs(n - e) < 1e-5
                       for n, e in zip(normal, expected)), (kind, engine, object_reflection, normal, expected)
    print(f"PASS {kind}/{engine} object_reflection={object_reflection}: {expected}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-devices", default="010")
    parser.add_argument("--cpu-only", action="store_true")
    args = parser.parse_args()
    lux.Init()
    engines = ["PATHCPU"] if args.cpu_only else ["PATHCPU", "PATHOCL"]
    for kind, reflected in [("static", False), ("instance", False), ("motion", False),
                            ("instance", True), ("motion", True)]:
        for engine in engines:
            render(engine, kind, reflected, args.gpu_devices)


if __name__ == "__main__":
    main()
