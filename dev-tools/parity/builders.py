# SPDX-License-Identifier: Apache-2.0
#
# API-built parity scenes (geometry that cannot be expressed as .scn
# text: per-vertex motion keys, shared-mesh instancing). Kept separate
# from the e9 motion tests so the harness imports no module with
# environment side effects.

import numpy as np


def build_vertex_motion():
    """Emissive quad sweeping +x through the shutter (5 motion keys)
    over a static matte wall, plus a second instance of the same
    animated mesh (blue emitter, transform -1.8x - column-major matrix)
    visible in frame. Exercises vertex-motion interpolation and
    shared-shape instancing on every accel path (CPU embree, OpenCL
    swept MBVH, Metal HWRT)."""
    import pysuperluxcore

    props = pysuperluxcore.Properties()
    props.SetFromString("""
scene.materials.mat.type = matte
scene.materials.mat.kd = 0.9 0.2 0.2
scene.materials.mat.emission = 4.0 0.0 0.0
scene.materials.bgmat.type = matte
scene.materials.bgmat.kd = 0.8 0.8 0.8
scene.materials.mat2.type = matte
scene.materials.mat2.kd = 0.2 0.2 0.9
scene.materials.mat2.emission = 0.0 2.0 3.0
scene.objects.bg.material = bgmat
scene.objects.bg.vertices = -5 -0.5 -2  5 -0.5 -2  5 3.5 -2  -5 3.5 -2
scene.objects.bg.faces = 0 1 2 0 2 3
scene.objects.quad.material = mat
scene.objects.quad.shape = quad
scene.objects.quad2.material = mat2
scene.objects.quad2.shape = quad
scene.objects.quad2.transformation = 1 0 0 0  0 1 0 0  0 0 1 0  -1.8 0.35 0 1
scene.camera.type = perspective
scene.camera.lookat.orig = 0 1.2 4.0
scene.camera.lookat.target = 0 0.7 0
scene.camera.fieldofview = 35
scene.camera.shutteropen = 0.0
scene.camera.shutterclose = 1.0
""")

    base = np.array([[-0.4, 0.3, 0], [0.4, 0.3, 0],
            [0.4, 1.1, 0], [-0.4, 1.1, 0]], dtype=np.float32)
    tris = np.array([[0, 1, 2], [0, 2, 3]], dtype=np.uint32)

    scene = pysuperluxcore.Scene()
    scene.DefineMeshExt("quad", base, tris)
    times = np.linspace(0.0, 1.0, 5, dtype=np.float32)
    steps = [base + np.array([d, 0, 0], dtype=np.float32)
            for d in np.linspace(0.0, 1.2, 5)]
    scene.SetMeshVertexMotion("quad", times, steps)
    scene.Parse(props)
    return scene


def _strand_points():
    # Two strands along +y, 3 control points each (e9_strand geometry)
    return np.array([
            [-0.4, -0.6, 0.0], [-0.4, 0.0, 0.0], [-0.4, 0.6, 0.0],
            [-0.4, -0.6, 0.15], [-0.4, 0.0, 0.15], [-0.4, 0.6, 0.15]],
            dtype=np.float32)


def build_strand_motion():
    """Two emissive strands (hair) with deformation motion over the
    shutter. 'solid' tessellation keeps the primitive identical on
    every backend, so unlike Metal-native curve primitives this is a
    true same-image parity scene."""
    import pysuperluxcore

    props = pysuperluxcore.Properties()
    props.SetFromString("""
scene.materials.mat.type = matte
scene.materials.mat.kd = 0.9 0.2 0.2
scene.materials.mat.emission = 3.0 0.0 0.0
scene.materials.bgmat.type = matte
scene.materials.bgmat.kd = 0.8 0.8 0.8
scene.objects.bg.material = bgmat
scene.objects.bg.vertices = -5 -1 -2  5 -1 -2  5 1.5 -2  -5 1.5 -2
scene.objects.bg.faces = 0 1 2 0 2 3
scene.objects.hair.material = mat
scene.objects.hair.shape = hair
scene.camera.type = perspective
scene.camera.lookat.orig = 0 0.3 3.0
scene.camera.lookat.target = 0 0 0
scene.camera.fieldofview = 40
scene.camera.shutteropen = 0.0
scene.camera.shutterclose = 1.0
""")

    base = _strand_points()
    moved = base.copy()
    moved[:, 0] += 0.8

    scene = pysuperluxcore.Scene()
    scene.DefineStrands("hair", 2, len(base), [tuple(p) for p in base],
            2, 0.03, 0.0, (0.8, 0.2, 0.2), None, "solid", 4, 0.0, 4,
            False, False, False)
    scene.SetStrandsVertexMotion("hair",
            np.array([0.0, 1.0], dtype=np.float32), [base, moved])
    scene.Parse(props)
    return scene
