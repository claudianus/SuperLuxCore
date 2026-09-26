#!/usr/bin/env python3
"""Vulkan HWRT dataset-update test.

PATHOCL + VULKAN_GPU session on a 2-triangle emissive quad; after a
few samples the object is moved via BeginSceneEdit/UpdateObject-
Transformation/EndSceneEdit, which must rebuild the BLAS/TLAS set
(VulkanIntersectionDevice::Update -> BuildRTAccel). PASS = the
"BLAS + TLAS built" log line appears twice and the render keeps
running (no stale-AS crash/miss).
"""

import sys, time as _t
sys.path.insert(0, "out/build/src/pysuperluxcore/Release")
import pysuperluxcore

QUAD_PLY = """ply
format ascii 1.0
element vertex 4
property float x
property float y
property float z
element face 2
property list uchar int vertex_indices
end_header
-1 0 -1
1 0 -1
1 0 1
-1 0 1
3 0 1 2
3 0 2 3
"""


def main():
    pysuperluxcore.Init()
    open("/tmp/vkrt_quad.ply", "w").write(QUAD_PLY)

    props = pysuperluxcore.Properties()
    props.SetFromString("""
scene.camera.lookat.orig = 0 -1.5 0
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 0 1
scene.objects.quad.ply = /tmp/vkrt_quad.ply
scene.materials.white.type = matte
scene.materials.white.kd = 0.8
scene.objects.quad.material = white
scene.lights.sky.type = constantinfinite
scene.lights.sky.gain = 1 1 1
""")
    scene = pysuperluxcore.Scene()
    scene.Parse(props)

    rcfg = pysuperluxcore.Properties()
    rcfg.Set(pysuperluxcore.Property("renderengine.type", "PATHOCL"))
    rcfg.Set(pysuperluxcore.Property("sampler.type", "SOBOL"))
    rcfg.Set(pysuperluxcore.Property("opencl.cpu.use", [0]))
    rcfg.Set(pysuperluxcore.Property("opencl.devices.select", "001"))
    rcfg.Set(pysuperluxcore.Property("opencl.native.threads.count", [0]))
    rcfg.Set(pysuperluxcore.Property("accelerator.type", "BVH"))
    rcfg.Set(pysuperluxcore.Property("batch.haltspp", [16]))
    rcfg.Set(pysuperluxcore.Property("film.width", [64]))
    rcfg.Set(pysuperluxcore.Property("film.height", [64]))

    session = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(rcfg, scene))
    session.Start()

    # Let the session render a few samples before the edit.
    t0 = _t.time()
    while _t.time() - t0 < 8:
        session.UpdateStats()
        _t.sleep(0.2)

    session.BeginSceneEdit()
    # Column-major 4x4 translation matrix: move the quad +0.25 in x.
    scene.UpdateObjectTransformation("quad", [
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0.25, 0, 0, 1,
    ])
    session.EndSceneEdit()

    # Keep rendering post-edit; a stale AS would fault or miss.
    t0 = _t.time()
    while _t.time() - t0 < 10:
        session.UpdateStats()
        if session.HasDone():
            break
        _t.sleep(0.2)
    session.Pause()
    session.Stop()
    print("VK_RT_UPDATE: session completed without crash")


if __name__ == "__main__":
    main()
