#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render native Math/Snap, optionally consuming real Blender-exported graphs.

PYTHONPATH=out/build/src/pysuperluxcore/Release python3.13 \
    dev-tools/math-snap-regression.py --gpu-devices 010
"""
import argparse
import array
import json
import math
import time

import pysuperluxcore as lux


def snap(a, b):
    return 0. if b == 0 else math.floor(a / b) * b


def native_cases():
    cases = []
    for index, (a, b) in enumerate([
        ([1.75] * 3, [1.] * 3), ([-1.25] * 3, [1.] * 3),
        ([1.75, -1.25, .5], [1., 1., 0.]),
        ([4.1, -2.1, 3.], [.5, -1., 2.]), ([3.] * 3, [0.] * 3)]):
        cases.append({"label": f"native-{index}", "output": "snapped",
                      "graph": "scene.textures.snapped.type = mathfunc\n"
                               "scene.textures.snapped.op = snap\n"
                               "scene.textures.snapped.texture1 = " + " ".join(map(str, a)) + "\n"
                               "scene.textures.snapped.texture2 = " + " ".join(map(str, b)) + "\n",
                      "expected": [snap(x, y) for x, y in zip(a, b)]})
    values = [-1.5, -1., 1.75]
    for op, evaluate in {
        "floor": math.floor, "ceil": math.ceil, "trunc": math.trunc,
        "fract": lambda value: value - math.floor(value),
        "round": lambda value: math.floor(value + .5),
    }.items():
        cases.append({"label": "native-" + op, "output": "rounded",
                      "graph": "scene.textures.rounded.type = mathfunc\n"
                               f"scene.textures.rounded.op = {op}\n"
                               "scene.textures.rounded.texture1 = " + " ".join(map(str, values)) + "\n",
                      "expected": [evaluate(value) for value in values]})
    # At float32's unit-spacing boundary, adding 0.5 rounds to the nearest
    # even representable value before floor. Normalize before emission so
    # the render tolerance measures a unit error, not huge radiance.
    for value, rounded in [(8388609., 8388610.), (-8388609., -8388608.)]:
        cases.append({"label": f"native-round-large-{value}", "output": "residual",
                      "graph": "scene.textures.rounded.type = mathfunc\n"
                               "scene.textures.rounded.op = round\n"
                               f"scene.textures.rounded.texture1 = {value}\n"
                               "scene.textures.residual.type = subtract\n"
                               "scene.textures.residual.texture1 = rounded\n"
                               f"scene.textures.residual.texture2 = {rounded}\n",
                      "expected": [0.] * 3})
    return cases


def render(case, engine, devices):
    scene = lux.Scene()
    scene.DefineMesh("plane", [(-4., -4., 0.), (4., -4., 0.), (4., 4., 0.), (-4., 4., 0.)],
                     [(0, 1, 2), (0, 2, 3)], None, None, None, None)
    props = lux.Properties()
    props.SetFromString(case["graph"] + f"""
scene.camera.lookat.orig = 0 0 3
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.objects.obj.shape = plane
scene.objects.obj.material = mat
scene.materials.mat.type = matte
scene.materials.mat.kd = 0
scene.materials.mat.emission = biased
scene.textures.biased.type = add
scene.textures.biased.texture1 = {case['output']}
scene.textures.biased.texture2 = 4
""")
    scene.Parse(props)
    # Reparse serialized scene properties: binary operands and the op must
    # survive SDL round-trip, not only an in-memory graph.
    serialized = scene.ToProperties()
    # Texture round-trip alone avoids serialized mesh filenames/resources.
    textures = lux.Properties()
    textures.SetFromString("\n".join(line for line in serialized.ToString().splitlines()
                                    if line.startswith("scene.textures.")))
    scene.Parse(textures)
    props = lux.Properties()
    props.SetFromString(f"""renderengine.type = {engine}
film.width = 32
film.height = 32
batch.haltspp = 64
sampler.type = SOBOL
opencl.devices.select = {devices}
opencl.native.threads.count = 0
path.hybridbackforward.enable = 0
path.lighttracing.enable = 0
film.outputs.0.type = RGB
film.outputs.0.filename = snap-regression.exr
""")
    session = lux.RenderSession(lux.RenderConfig(props, scene))
    session.Start()
    try:
        deadline = time.monotonic() + 120
        while not session.HasDone():
            if time.monotonic() >= deadline:
                raise TimeoutError(case["label"])
            time.sleep(.1)
            session.UpdateStats()
    finally:
        session.Stop()
    pixels = array.array("f", [0.] * (32 * 32 * 3))
    session.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB, pixels)
    expected = [v + 4 for v in case["expected"]]
    for y in range(14, 18):
        for x in range(14, 18):
            actual = pixels[3 * (y * 32 + x):3 * (y * 32 + x) + 3]
            # Rendered radiance allows small backend residuals; nearest
            # rounding changes these fixtures by at least half a step.
            assert all(math.isfinite(v) and abs(v - e) < .05 for v, e in zip(actual, expected)), (
                case["label"], engine, actual, expected)
    print(f"PASS {case['label']}/{engine}: {expected}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-devices", default="010")
    parser.add_argument("--cpu-only", action="store_true")
    parser.add_argument("--cases", help="JSON array of exported graph/output/expected cases")
    args = parser.parse_args()
    cases = native_cases()
    if args.cases:
        with open(args.cases, encoding="utf-8") as stream:
            cases += json.load(stream)
    lux.Init()
    engines = ["PATHCPU"] if args.cpu_only else ["PATHCPU", "PATHOCL"]
    for case in cases:
        for engine in engines:
            render(case, engine, args.gpu_devices)


if __name__ == "__main__":
    main()
