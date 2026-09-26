# SPDX-License-Identifier: Apache-2.0
#
# E22: Metal native curve primitives — geometric parity gate.
#
# Strands meshes carry Catmull-Rom curve data next to their triangle
# tessellation. On Metal HWRT the leaf AS is built from curve primitives
# (MTLAccelerationStructureCurveGeometryDescriptor); every other path
# uses the tessellated triangles. The two are different primitives:
# the curve renders the true round tube while `solid` tessellation is an
# inscribed N-gon (face distance r*cos(pi/n), default sidecount 3 -> 0.5r)
# and `ribbon` is a flat quad strip.
#
# So exact pixel parity is impossible by construction. What this test
# gates is the *correctness* of the curve AS and shading path:
#
#   T1 coverage: ALPHA mask of the native-curve render must be a near-
#      perfect subset of a high-sidecount (16-gon) tessellated render —
#      no phantom hits, no missing silhouette. The 16-gon's inradius
#      (r*cos(11.25deg)=0.98r) is within 2% of the true tube radius.
#   T2 shading: mean luminance of the native render within 15% of the
#      tessellated reference on the same scene (residual delta is the
#      radial-vs-facet normal difference; see metal_curve_design.md).
#   T3 fallback: LUXRAYS_METAL_CURVES=0 must take the triangle path and
#      match the OpenCL tessellated render closely (same primitive).
#   T4 all outputs finite.
#
# The Metal legs skip cleanly on hosts without a METAL_GPU device.

import os
import sys
import time

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(
    0,
    os.environ.get(
        "LUXCORE_PY",
        os.path.join(REPO, "out", "build", "src", "pyluxcore", "Release"),
    ),
)
import pyluxcore

WIDTH = 320
HEIGHT = 240
SPP = 48
RENDER_TIMEOUT_S = 300
TASK_COUNT = 65536

SCENE = f"""
scene.camera.lookat.orig = -0.8 1.3 0.6
scene.camera.lookat.target = 0.0 0.0 0.4
scene.camera.up = 0 1 0
scene.camera.screenwindow = -0.2 0.2 -0.15 0.15
scene.materials.hair_mat.type = matte
scene.materials.hair_mat.kd = 0.75 0.65 0.2
scene.shapes.hair_shape.type = strands
scene.shapes.hair_shape.file = {REPO}/scenes/strands/straight.hair
scene.shapes.hair_shape.tessellation.type = solidadaptive
scene.shapes.hair_shape.tessellation.solid.sidecount = 16
scene.shapes.hair_shape.tessellation.adaptive.maxdepth = 12
scene.shapes.hair_shape.tessellation.adaptive.error = 0.02
scene.objects.hair_obj.shape = hair_shape
scene.objects.hair_obj.material = hair_mat
scene.lights.skyl.type = sky2
scene.lights.skyl.gain = 0.00003 0.00003 0.00003
scene.lights.sunl.type = sun
scene.lights.sunl.dir = -0.3 -0.5 0.8
scene.lights.sunl.gain = 0.0003 0.0003 0.0003
"""

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def device_mask(want_type):
    """Build an opencl.devices.select mask enabling only `want_type`."""
    descs = pyluxcore.GetOpenCLDeviceDescs()
    mask = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        mask += "1" if t == want_type else "0"
        i += 1
    return mask or None


def render(engine="PATHOCL", sel=None, spp=SPP):
    scn = pyluxcore.Properties()
    scn.SetFromString(SCENE)
    scene = pyluxcore.Scene()
    scene.Parse(scn)

    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = 17
opencl.task.count = {TASK_COUNT}
film.outputs.1.type = ALPHA
film.outputs.1.filename = e22_alpha.exr
film.outputs.2.type = INDIRECT_DIFFUSE
film.outputs.2.filename = e22_id.exr
""")
    if sel:
        cfg.Set(pyluxcore.Property("opencl.devices.select", sel))

    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {spp} spp")
        time.sleep(0.3)

    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    alpha = np.empty(WIDTH * HEIGHT, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.ALPHA, alpha, 1, True)
    idiff = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pyluxcore.FilmOutputType.INDIRECT_DIFFUSE, idiff, 0, True)
    ses.Stop()
    lum = (rgb.reshape(HEIGHT, WIDTH, 3)
           @ np.array([0.2126, 0.7152, 0.0722], np.float32))
    return lum, alpha.reshape(HEIGHT, WIDTH), idiff.reshape(-1, 3)


def main():
    pyluxcore.Init()

    mtl_mask = device_mask("METAL_GPU")
    ocl_mask = device_mask("OPENCL_GPU")

    if mtl_mask is None:
        print("no METAL_GPU device - skipping Metal legs")
    else:
        # Native curves on Metal
        mtl_lum, mtl_al, mtl_id = render(sel=mtl_mask)
        # Triangle-tessellation fallback on Metal
        os.environ["LUXRAYS_METAL_CURVES"] = "0"
        try:
            mtl_tri_lum, mtl_tri_al, mtl_tri_id = render(sel=mtl_mask)
        finally:
            del os.environ["LUXRAYS_METAL_CURVES"]

        # Tessellated reference on OpenCL (same primitive as fallback)
        ocl_lum, ocl_al, ocl_id = render(sel=ocl_mask)

        mtl_hit = mtl_al > 0.5
        tri_hit = mtl_tri_al > 0.5

        mtl_only = (mtl_hit & ~tri_hit).mean()
        tri_only = (tri_hit & ~mtl_hit).mean()
        inter = (mtl_hit & tri_hit).mean()
        record(
            "T1.coverage-subset",
            mtl_only < 0.01 and inter > 0.9 * mtl_hit.mean() and tri_only < 0.02,
            f"mtl cover={mtl_hit.mean():.4f} tri cover={tri_hit.mean():.4f} "
            f"mtl_only={mtl_only:.4f} tri_only={tri_only:.4f} inter={inter:.4f}",
        )

        ratio = mtl_lum.mean() / max(mtl_tri_lum.mean(), 1e-9)
        record(
            "T2.shading-mean",
            0.85 < ratio < 1.15,
            f"MTL curve={mtl_lum.mean():.4f} MTL tri={mtl_tri_lum.mean():.4f} "
            f"ratio={ratio:.3f} (gate 0.85-1.15)",
        )

        ratio_fb = mtl_tri_lum.mean() / max(ocl_lum.mean(), 1e-9)
        cover_fb = abs(mtl_tri_al.mean() - ocl_al.mean())
        record(
            "T3.fallback-parity",
            0.95 < ratio_fb < 1.05 and cover_fb < 0.01,
            f"MTL tri={mtl_tri_lum.mean():.4f} OCL tri={ocl_lum.mean():.4f} "
            f"ratio={ratio_fb:.3f} (gate 0.95-1.05) "
            f"alpha diff={cover_fb:.4f} (gate 0.01)",
        )

        record(
            "T4.finite",
            np.isfinite(mtl_lum).all() and np.isfinite(mtl_tri_lum).all()
            and np.isfinite(ocl_lum).all(),
            "all outputs finite",
        )

        # T5: bounce rays off curve hits must produce indirect light.
        # A degenerate curve shading frame (zero tangent -> NaN frame ->
        # NaN continuation ray) silently kills every secondary bounce and
        # INDIRECT_DIFFUSE collapses (~20x deficit observed). The round
        # tube vs 16-gon geometry only explains a few percent, so a wide
        # 0.7-1.4 gate still catches the collapse.
        id_ratio = mtl_id.mean() / max(mtl_tri_id.mean(), 1e-9)
        record(
            "T5.indirect-diffuse",
            0.7 < id_ratio < 1.4 and np.isfinite(mtl_id).all(),
            f"MTL curve ID={mtl_id.mean():.5f} MTL tri ID={mtl_tri_id.mean():.5f} "
            f"ratio={id_ratio:.3f} (gate 0.7-1.4)",
        )

    fails = [n for n, ok in results if not ok]
    print(f"\n{'ALL PASS' if not fails else 'FAILURES: ' + str(fails)} "
          f"({len(results) - len(fails)}/{len(results)})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
