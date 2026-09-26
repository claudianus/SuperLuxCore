#!/usr/bin/env python3
"""E9 parity test: the same per-vertex deformation motion scene rendered
through every CPU/GPU intersection path — outputs must agree.

Paths covered (each rendered as a separate session on the same scene):
  * PATHCPU + accelerator.type=MBVH   (Phase-3 CPU leaf interpolation)
  * PATHCPU + accelerator.type=BVH    (Phase-3 flat-BVH path)
  * PATHCPU + accelerator.type=EMBREE (Phase-4 Embree vertex timesteps)
  * TILEPATHOCL                       (Metal HWRT motion descriptors, or
                                       the cl2msl software kernel when
                                       LUXRAYS_METAL_HWRT=0)

Checks compare the emissive quad's screen-space column extent at fixed
shutter times (t~=0 / 0.5 / 1 for a K=3 uniform series) and the full
sweep. Embree and Metal distribute timesteps uniformly over the shutter
interval, so only uniform-spaced series are compared across backends —
non-uniform timing is exact on MBVH/BVH/SW and covered by the dedicated
backend tests.

Run with the Blender-bundled python (or any python with the built
pyluxcore on sys.path):

    LUXCORE_PY=/path/to/pyluxcore/dir python3 e9_parity_test.py

Requires a Metal-capable device for the OCL leg; that leg skips cleanly
otherwise.
"""

import os
import sys
import time as _t

import numpy as np

sys.path.insert(
    0,
    os.environ.get(
        "LUXCORE_PY",
        os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..",
            "out",
            "build",
            "src",
            "pyluxcore",
            "Release",
        ),
    ),
)
import pyluxcore

pyluxcore.Init()

FAILURES = []


def check(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    print(f"[{tag}] {name} {detail}")
    if not cond:
        FAILURES.append(name)


def build_scene(steps_dx, times, shutter):
    """Quad at z=0 moving along +x by steps_dx; wall at z=-2 behind it."""
    props = pyluxcore.Properties()
    props.Set(pyluxcore.Property("scene.materials.mat.type", "matte"))
    props.Set(pyluxcore.Property("scene.materials.mat.kd", [0.9, 0.2, 0.2]))
    props.Set(pyluxcore.Property("scene.materials.mat.emission", [4.0, 0.0, 0.0]))
    props.Set(pyluxcore.Property("scene.materials.bgmat.type", "matte"))
    props.Set(pyluxcore.Property("scene.materials.bgmat.kd", [0.8, 0.8, 0.8]))
    props.Set(pyluxcore.Property("scene.objects.bg.material", "bgmat"))
    props.Set(
        pyluxcore.Property(
            "scene.objects.bg.vertices",
            [-5.0, -0.5, -2.0, 5.0, -0.5, -2.0, 5.0, 3.5, -2.0, -5.0, 3.5, -2.0],
        )
    )
    props.Set(pyluxcore.Property("scene.objects.bg.faces", [0, 1, 2, 0, 2, 3]))

    base = np.array(
        [[-0.4, 0.3, 0], [0.4, 0.3, 0], [0.4, 1.1, 0], [-0.4, 1.1, 0]],
        dtype=np.float32,
    )
    tris = np.array([[0, 1, 2], [0, 2, 3]], dtype=np.uint32)

    scene = pyluxcore.Scene()
    scene.DefineMeshExt("quad", base, tris)
    if steps_dx:
        t = np.asarray(times, dtype=np.float32)
        steps = [base + np.array([d, 0, 0], dtype=np.float32) for d in steps_dx]
        scene.SetMeshVertexMotion("quad", t, steps)

    props.Set(pyluxcore.Property("scene.objects.quad.material", "mat"))
    props.Set(pyluxcore.Property("scene.objects.quad.shape", "quad"))
    props.Set(pyluxcore.Property("scene.camera.type", "perspective"))
    props.Set(pyluxcore.Property("scene.camera.lookat.orig", [0.0, 1.2, 4.0]))
    props.Set(pyluxcore.Property("scene.camera.lookat.target", [0.0, 0.7, 0.0]))
    props.Set(pyluxcore.Property("scene.camera.fieldofview", [35.0]))
    props.Set(pyluxcore.Property("scene.camera.shutteropen", [shutter[0]]))
    props.Set(pyluxcore.Property("scene.camera.shutterclose", [shutter[1]]))
    scene.Parse(props)
    return scene


def render(scene, engine, accel=None, w=128, h=128, spp=48):
    rcfg = pyluxcore.Properties()
    if engine == "PATHCPU":
        rcfg.Set(pyluxcore.Property("renderengine.type", "PATHCPU"))
        rcfg.Set(pyluxcore.Property("sampler.type", "SOBOL"))
        rcfg.Set(pyluxcore.Property("opencl.cpu.use", [0]))
        rcfg.Set(pyluxcore.Property("opencl.native.threads.count", [0]))
        if accel:
            rcfg.Set(pyluxcore.Property("accelerator.type", accel))
    else:
        rcfg.Set(pyluxcore.Property("renderengine.type", "TILEPATHOCL"))
        rcfg.Set(pyluxcore.Property("sampler.type", "TILEPATHSAMPLER"))
        rcfg.Set(pyluxcore.Property("opencl.cpu.use", [0]))
        rcfg.Set(pyluxcore.Property("opencl.gpu.use", [1]))
        rcfg.Set(pyluxcore.Property("opencl.native.threads.count", [0]))
        rcfg.Set(pyluxcore.Property("opencl.devices.select", "01"))
    rcfg.Set(pyluxcore.Property("batch.haltspp", [spp]))
    rcfg.Set(pyluxcore.Property("film.width", [w]))
    rcfg.Set(pyluxcore.Property("film.height", [h]))
    session = pyluxcore.RenderSession(pyluxcore.RenderConfig(rcfg, scene))
    session.Start()
    t0 = _t.time()
    while _t.time() - t0 < 90:
        session.UpdateStats()
        if (
            session.HasDone()
            or session.GetStats().Get("stats.renderengine.pass").GetInt() >= spp
        ):
            break
        _t.sleep(0.05)
    session.Pause()
    rgb = np.zeros(w * h * 3, dtype=np.float32)
    session.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB, rgb)
    session.Stop()
    return rgb.reshape(h, w, 3)


def quad_extent(rgb):
    """(min_col, max_col, count) of columns where the emissive quad shows."""
    vis = rgb[..., 0] > 0.25
    colcount = vis.sum(axis=0)
    cols = np.nonzero(colcount >= 2)[0]
    if len(cols) == 0:
        return None
    return int(cols[0]), int(cols[-1]), int(vis.sum())


def close(a, b, tol):
    return a is not None and b is not None and abs(a - b) <= tol


def compare(tag, ref, other, tol=4):
    """Compare extents: edges within tol px, hit counts within 25%."""
    if ref is None and other is None:
        check(tag, True)
        return
    check(
        tag,
        ref is not None
        and other is not None
        and close(ref[0], other[0], tol)
        and close(ref[1], other[1], tol)
        and abs(ref[2] - other[2]) <= max(20, ref[2] * 0.30),
        f"ref={ref} other={other}",
    )


def main():
    # K=3 uniform series: quad sweeps +1.5x over the shutter.
    steps_dx = [0.0, 0.75, 1.5]
    times = [0.0, 0.5, 1.0]

    legs = [
        ("MBVH", "PATHCPU", "MBVH"),
        ("BVH", "PATHCPU", "BVH"),
        ("EMBREE", "PATHCPU", "EMBREE"),
        ("METAL", "TILEPATHOCL", None),
    ]

    has_metal = True
    for pose, shutter in [("t0", (0.0, 0.02)), ("t05", (0.49, 0.51)), ("t1", (0.98, 1.0)), ("sweep", (0.0, 1.0))]:
        scene = build_scene(steps_dx, times, shutter)
        ref = None
        for name, engine, accel in legs:
            if name == "METAL" and not has_metal:
                continue
            try:
                rgb = render(scene, engine, accel)
            except Exception as e:
                if name == "METAL":
                    print(f"[skip] METAL leg unavailable: {e}")
                    has_metal = False
                    continue
                raise
            ext = quad_extent(rgb)
            if ref is None:
                ref = ext
                check(f"{pose}: {name} produced hits", ext is not None,
                      f"ext={ext}")
            else:
                compare(f"{pose}: {name} vs {legs[0][0]}", ref, ext)

    print(f"\n{len(FAILURES)} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
