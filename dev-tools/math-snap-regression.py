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
    for op, a, b, expected in [
        ("max", [1.5, -1., -.5], [1., 0., -2.], [1.5, 0., -.5]),
        ("min", [1.5, -1., -.5], [1., 0., -2.], [1., -1., -2.]),
        ("lessequal", [0., .125, .25], [0., .125, .125], [1., 1., 0.]),
    ]:
        cases.append({"label": "native-" + op, "output": "compared",
                      "graph": "scene.textures.compared.type = mathfunc\n"
                               f"scene.textures.compared.op = {op}\n"
                               "scene.textures.compared.texture1 = " + " ".join(map(str, a)) + "\n"
                               "scene.textures.compared.texture2 = " + " ".join(map(str, b)) + "\n",
                      "expected": expected})
    for op, a, b, expected in [
        ("max", -.75, .25, .0625), ("lessequal", .125, .125, 1.),
        ("min", -.75, .25, .5625),
    ]:
        cases.append({"label": "native-float-" + op, "output": "powered",
                      "graph": "scene.textures.compared.type = mathfunc\n"
                               f"scene.textures.compared.op = {op}\n"
                               f"scene.textures.compared.texture1 = {a}\n"
                               f"scene.textures.compared.texture2 = {b}\n"
                               "scene.textures.powered.type = power\n"
                               "scene.textures.powered.base = compared\n"
                               "scene.textures.powered.exponent = 2\n",
                      "expected": [expected] * 3})
    for float_consumer in (False, True):
        graph = ("scene.textures.grow.type = mathfunc\n"
                 "scene.textures.grow.op = exp\n"
                 "scene.textures.grow.texture1 = 1000\n"
                 "scene.textures.nan.type = subtract\n"
                 "scene.textures.nan.texture1 = grow\n"
                 "scene.textures.nan.texture2 = grow\n"
                 "scene.textures.compared.type = mathfunc\n"
                 "scene.textures.compared.op = lessequal\n"
                 "scene.textures.compared.texture1 = nan\n"
                 "scene.textures.compared.texture2 = 0\n")
        if float_consumer:
            graph += ("scene.textures.powered.type = power\n"
                      "scene.textures.powered.base = compared\n"
                      "scene.textures.powered.exponent = 1\n")
        cases.append({"label": f"native-nan-compare-float-{int(float_consumer)}",
                      "output": "powered" if float_consumer else "compared",
                      "graph": graph, "expected": [0.] * 3})
    cases.append({"label": "native-divide-hdr-spectrum", "output": "quotient",
                  "graph": "scene.textures.quotient.type = divide\n"
                           "scene.textures.quotient.texture1 = -3e38 3e38 0\n"
                           "scene.textures.quotient.texture2 = -3e38 3e38 1\n",
                  "expected": [1., 1., 0.]})
    cases.append({"label": "native-divide-hdr-float", "output": "powered",
                  "graph": "scene.textures.quotient.type = divide\n"
                           "scene.textures.quotient.texture1 = 3e38\n"
                           "scene.textures.quotient.texture2 = 3e38\n"
                           "scene.textures.powered.type = power\n"
                           "scene.textures.powered.base = quotient\n"
                           "scene.textures.powered.exponent = 1\n",
                  "expected": [1.] * 3})
    cases.append({"label": "native-divide-tiny-spectrum", "output": "quotient",
                  "graph": "scene.textures.quotient.type = divide\n"
                           "scene.textures.quotient.texture1 = 3e-38 1e-38 1e-45\n"
                           "scene.textures.quotient.texture2 = 3e-38 1e-38 1e-45\n",
                  "expected": [1.] * 3})
    cases.append({"label": "native-divide-tiny-float", "output": "powered",
                  "graph": "scene.textures.quotient.type = divide\n"
                           "scene.textures.quotient.texture1 = 1e-45\n"
                           "scene.textures.quotient.texture2 = 1e-45\n"
                           "scene.textures.powered.type = power\n"
                           "scene.textures.powered.base = quotient\n"
                           "scene.textures.powered.exponent = 1\n",
                  "expected": [1.] * 3})
    cases.append({"label": "native-divide-tiny-signed-spectrum", "output": "quotient",
                  "graph": "scene.textures.quotient.type = divide\n"
                           "scene.textures.quotient.texture1 = -1e-38 -1e-45 0\n"
                           "scene.textures.quotient.texture2 = 1e-38 -1e-45 1e-45\n",
                  "expected": [-1., 1., 0.]})
    for float_consumer in (False, True):
        graph = ("scene.textures.quotient.type = divide\n"
                 "scene.textures.quotient.texture1 = 1\n"
                 "scene.textures.quotient.texture2 = -0\n")
        output = "quotient"
        if float_consumer:
            graph += ("scene.textures.powered.type = power\n"
                      "scene.textures.powered.base = quotient\n"
                      "scene.textures.powered.exponent = 1\n")
            output = "powered"
        cases.append({"label": f"native-divide-zero-{int(float_consumer)}",
                      "output": output, "graph": graph, "expected": [0.] * 3})
    return cases


def render(case, engine, devices):
    scene = lux.Scene()
    colours = [tuple(value) for value in case["reference_colours"]] if "reference_colours" in case else None
    scene.DefineMesh("plane", [(-4., -4., 0.), (4., -4., 0.), (4., 4., 0.), (-4., 4., 0.)],
                     [(0, 1, 2), (0, 2, 3)], None, None, colours, None,
                     case.get("mesh_transform"))
    props = lux.Properties()
    output = case["output"]
    output_sdl = " ".join(map(str, output)) if isinstance(output, (list, tuple)) else str(output)
    graph = case["graph"]
    if "reference_colours" in case:
        # Independent Blender-derived vertex values interpolate over the
        # same surface, without assumptions about camera/filter sampling.
        graph += ("scene.textures.reference.type = hitpointcolor\n"
                  "scene.textures.residual.type = subtract\n"
                  f"scene.textures.residual.texture1 = {output_sdl}\n"
                  "scene.textures.residual.texture2 = reference\n"
                  "scene.textures.amplified.type = scale\n"
                  "scene.textures.amplified.texture1 = residual\n"
                  "scene.textures.amplified.texture2 = 1000\n")
        output_sdl = "amplified"
    target = case.get("target", [0., 0., 0.])
    camera_target = " ".join(map(str, target))
    camera_origin = " ".join(map(str, [target[0], target[1], target[2] + 3.]))
    props.SetFromString(graph + f"""
scene.camera.lookat.orig = {camera_origin}
scene.camera.lookat.target = {camera_target}
scene.camera.up = 0 1 0
scene.objects.obj.shape = plane
scene.objects.obj.material = mat
scene.materials.mat.type = matte
scene.materials.mat.kd = 0
scene.materials.mat.emission = biased
scene.textures.biased.type = add
scene.textures.biased.texture1 = {output_sdl}
scene.textures.biased.texture2 = 4
""")
    if "object_transform" in case:
        props.Set(lux.Property("scene.objects.obj.transformation", case["object_transform"]))
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
        for transform in case.get("update_transforms", []):
            session.BeginSceneEdit()
            scene.UpdateObjectTransformation("obj", transform)
            session.EndSceneEdit()
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
            assert all(math.isfinite(v) and abs(v - e) < .05
                       for v, e in zip(actual, expected)), (case["label"], engine, actual, expected)
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
