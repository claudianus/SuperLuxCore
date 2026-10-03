#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check normal-dependent bump against an independent differential oracle.

Requires numpy and pysuperluxcore on PYTHONPATH. Use --gpu-devices to select
an isolated GPU, or --cpu-only. The default mask selects Metal on Apple.
"""
import argparse
import array
import time

import numpy as np
import pysuperluxcore as lux


CORNERS = np.array([[1.2, 0., 1.6], [0., 3., 4.], [0., 0., 7.]])
SAMPLE_DISTANCE = .001


def normalize(v):
    return v / np.linalg.norm(v)


def expected_normal(scale):
    # The centre ray hits barycentrics (1/4, 1/4, 1/2). Interpolation uses
    # raw normals; derivatives use normalized, transformed corner normals.
    sign = -1 if np.prod(scale) < 0 else 1
    n = sign * normalize(np.array([.25, .25, .5]) @ CORNERS / scale)
    corners = np.array([sign * normalize(c / scale) for c in CORNERS])
    du, dv = corners[1] - corners[0], corners[2] - corners[0]
    pu = np.cross(n, np.cross(np.array([2., 0., 0.]) * scale, n))
    pv = np.cross(n, np.cross(np.array([1., 2., 0.]) * scale, n))
    u, v = SAMPLE_DISTANCE / np.linalg.norm(pu), SAMPLE_DISTANCE / np.linalg.norm(pv)
    # shadingnormal's scalar value is its X component.
    hu = (normalize(n + u * du)[0] - n[0]) / u
    hv = (normalize(n + v * dv)[0] - n[0]) / v
    bumped = normalize(np.cross(pu + hu * n, pv + hv * n))
    return -bumped if np.dot(bumped, n) < 0 else bumped


def render(scale, engine, devices):
    scene = lux.Scene()
    scene.DefineMesh("plane", [(-1., -1., 0.), (1., -1., 0.), (0., 1., 0.)],
                     [(0, 1, 2)], [tuple(c) for c in CORNERS],
                     [(0., 0.), (1., 0.), (0., 1.)], None, None)
    sign = -1 if np.prod(scale) < 0 else 1
    text = f"""scene.camera.lookat.orig = 0 0 {3 * sign}
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 5
scene.materials.mat.type = matte
scene.materials.mat.kd = 0.5
scene.materials.mat.bumptex = normal
scene.materials.mat.bumpsamplingdistance = {SAMPLE_DISTANCE}
scene.textures.normal.type = shadingnormal
scene.objects.obj.shape = plane
scene.objects.obj.material = mat
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1 1 1
"""
    if scale != (1, 1, 1):
        matrix = [scale[0], 0, 0, 0, 0, scale[1], 0, 0, 0, 0, scale[2], 0, 0, 0, 0, 1]
        text += "scene.objects.obj.transformation = " + " ".join(map(str, matrix)) + "\n"
    props = lux.Properties()
    props.SetFromString(text)
    scene.Parse(props)
    props = lux.Properties()
    props.SetFromString(f"""renderengine.type = {engine}
film.width = 128
film.height = 128
sampler.type = SOBOL
batch.haltspp = 8
opencl.devices.select = {devices}
opencl.native.threads.count = 0
film.outputs.0.type = SHADING_NORMAL
film.outputs.0.filename = bump-normal.exr
""")
    session = lux.RenderSession(lux.RenderConfig(props, scene))
    session.Start()
    try:
        deadline = time.monotonic() + 120
        while not session.HasDone():
            if time.monotonic() >= deadline:
                raise TimeoutError(f"{scale}/{engine} rendering did not finish")
            time.sleep(.1)
            session.UpdateStats()
    finally:
        session.Stop()
    buf = array.array("f", [0.] * (128 * 128 * 3))
    session.GetFilm().GetOutputFloat(lux.FilmOutputType.SHADING_NORMAL, buf)
    normal = np.asarray(buf).reshape(128, 128, 3)[63:65, 63:65].mean(axis=(0, 1))
    expected = expected_normal(scale)
    error = np.max(np.abs(normal - expected))
    # Small allowance for distinct subpixel sample locations around the
    # analytic centre ray, not transport noise or a backend-derived oracle.
    assert np.all(np.isfinite(normal)) and error < .002, (scale, engine, normal, expected)
    print(f"PASS {scale}/{engine}: normal={normal} expected={expected} error={error:.6g}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-devices", default="010")
    parser.add_argument("--cpu-only", action="store_true")
    args = parser.parse_args()
    lux.Init()
    engines = ["PATHCPU"] if args.cpu_only else ["PATHCPU", "PATHOCL"]
    for scale in [(1, 1, 1), (.5, 1, 2), (-.5, 1, 2)]:
        for engine in engines:
            render(scale, engine, args.gpu_devices)


if __name__ == "__main__":
    main()
