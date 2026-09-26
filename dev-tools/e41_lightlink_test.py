# SPDX-License-Identifier: Apache-2.0
#
# E41: Light linking (receiver-based illumination groups).
#
# Semantics:
#   scene.lights.L.linkgroups = "a,b"  -> the light only lights objects
#       whose accept mask shares a group bit; no groups = global light
#   scene.objects.O.linkgroups = "a"   -> receiver group membership
#   scene.objects.O.linkmode = include|exclude  (include = default)
#   Emissive mesh lights inherit the owning object's link groups.
#   Direct illumination only (first light-path vertex); indirect
#   bounces are unfiltered, so tests run at pathdepth 1.
#
# T1 include:     gL point light -> gL box + gL floor lit, plain box ~0
# T2 exclude:     receiver with linkmode=exclude stays dark
# T3 global:      light without groups lights everything
# T4 mesh emit:   emissive cube with linkgroups lights only gL objects
# T5 env light:   grouped constantinfinite lights only gL receivers
# T6 multi-group: light "gL,g2" links an object in group "g2"
# T7 GPU parity:  PATHOCL per-object means match PATHCPU (T1)
# T8 roundtrip:   ToProperties re-emits linkgroups/linkmode
#
# Run:
#   python3.13 dev-tools/e41_lightlink_test.py
#
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for _variant in ("Release", "Debug"):
    _p = REPO / "out/build/src/pysuperluxcore" / _variant
    if any(_p.glob("pysuperluxcore*.so")):
        sys.path.insert(0, str(_p))
        break
import pysuperluxcore

WIDTH, HEIGHT = 320, 180
SPP = 64
TASK_COUNT = 1 << 16
RENDER_TIMEOUT_S = 300
PLY = REPO / "scenes/cornell/unitcube.ply"

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def device_mask(want_type):
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
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


def translate(tx, ty, tz):
    # 16 values, column-major (Property::Get<Matrix4x4> reads columns)
    return (f"1 0 0 0  0 1 0 0  0 0 1 0  {tx} {ty} {tz} 1")


def scale_translate(sx, sy, sz, tx, ty, tz):
    return (f"{sx} 0 0 0  0 {sy} 0 0  0 0 {sz} 0  {tx} {ty} {tz} 1")


def base_scene(link_mode_a=""):
    """Two unit boxes on a floor slab; camera looks slightly down."""
    return f"""
scene.camera.type = perspective
scene.camera.lookat.orig = 0 -7 3.2
scene.camera.lookat.target = 0 0 0.7
scene.camera.fieldofview = 34

scene.materials.Box.type = matte
scene.materials.Box.kd = 0.72 0.72 0.72
scene.materials.Floor.type = matte
scene.materials.Floor.kd = 0.5 0.5 0.5
scene.materials.Emit.type = matte
scene.materials.Emit.kd = 1 1 1
scene.materials.Emit.emission = 40 40 40

# floor slab 8x8x0.1, top at z=0
scene.objects.Floor.material = Floor
scene.objects.Floor.ply = {PLY}
scene.objects.Floor.transformation = {scale_translate(8, 8, 0.1, -4, -4, -0.1)}
scene.objects.Floor.linkgroups = gL
scene.objects.Floor.id = 13

# receiver A (left), group gL
scene.objects.A.material = Box
scene.objects.A.ply = {PLY}
scene.objects.A.transformation = {translate(-1.8, 0.4, 0)}
scene.objects.A.linkgroups = gL
{link_mode_a}
scene.objects.A.id = 11

# receiver B (right), no groups
scene.objects.B.material = Box
scene.objects.B.ply = {PLY}
scene.objects.B.transformation = {translate(0.8, 0.4, 0)}
scene.objects.B.id = 12
"""


def light_point(groups=None, color="500 500 500", pos="0 -0.6 3.2"):
    g = f'scene.lights.L.linkgroups = {groups}\n' if groups else ""
    return (f'scene.lights.L.type = point\n'
            f'scene.lights.L.position = {pos}\n'
            f'scene.lights.L.color = {color}\n' + g)


def light_emitter_obj(groups):
    """Ceiling emissive cube; triangle lights inherit its link groups."""
    return (f'scene.objects.Emit.material = Emit\n'
            f'scene.objects.Emit.ply = {PLY}\n'
            f'scene.objects.Emit.transformation = '
            f'{scale_translate(1.4, 1.4, 0.15, -0.7, -0.7, 3.2)}\n'
            f'scene.objects.Emit.linkgroups = {groups}\n'
            f'scene.objects.Emit.id = 14\n')


def light_env(groups):
    return (f'scene.lights.env.type = constantinfinite\n'
            f'scene.lights.env.color = 1.5 1.5 1.5\n'
            f'scene.lights.env.linkgroups = {groups}\n')


def render(scene_props, engine, sel=None):
    scn = pysuperluxcore.Properties()
    scn.SetFromString(scene_props)
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
opencl.task.count = {TASK_COUNT}
path.pathdepth.total = 1
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = OBJECT_ID
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {SPP} spp")
        time.sleep(0.25)

    film = ses.GetFilm()
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    oid = np.empty(WIDTH * HEIGHT, dtype=np.uint32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB, rgb)
    film.GetOutputUInt(pysuperluxcore.FilmOutputType.OBJECT_ID, oid)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3), oid.reshape(HEIGHT, WIDTH)


def obj_means(rgb, oid):
    """Median luminance over pixels of each scene object id (median is
    robust against pixel-filter bleed at silhouettes)."""
    lum = rgb.mean(axis=2)
    out = {}
    for oid_val, name in ((11, "A"), (12, "B"), (13, "Floor"), (14, "Emit")):
        m = oid == oid_val
        out[name] = float(np.median(lum[m])) if m.any() else 0.0
        out[name + "_px"] = int(m.sum())
    return out


def main():
    cpu = "PATHCPU"
    lit, dark = 0.1, 0.02

    # T1: gL point light -> linked A + floor lit, unlinked B ~0
    m = obj_means(*render(base_scene() + light_point("gL"), cpu))
    record("T1 include", m["A"] > lit and m["B"] < dark and m["Floor"] > lit,
           f"A={m['A']:.3f} B={m['B']:.5f} Floor={m['Floor']:.3f}")

    # T2: A linkmode=exclude -> A dark, B dark, floor lit
    m = obj_means(*render(base_scene("scene.objects.A.linkmode = exclude\n")
                          + light_point("gL"), cpu))
    record("T2 exclude", m["A"] < dark and m["B"] < dark and m["Floor"] > lit,
           f"A={m['A']:.5f} B={m['B']:.5f} Floor={m['Floor']:.3f}")

    # T3: global light (no groups) lights everything
    m = obj_means(*render(base_scene() + light_point(None), cpu))
    record("T3 global", m["A"] > lit and m["B"] > lit and m["Floor"] > lit,
           f"A={m['A']:.3f} B={m['B']:.3f} Floor={m['Floor']:.3f}")

    # T4: emissive mesh object inherits link groups
    m = obj_means(*render(base_scene() + light_emitter_obj("gL"), cpu))
    record("T4 mesh emitter", m["A"] > lit and m["B"] < dark,
           f"A={m['A']:.3f} B={m['B']:.5f}")

    # T5: grouped environment light lights only gL receivers
    m = obj_means(*render(base_scene() + light_env("gL"), cpu))
    record("T5 env light", m["A"] > lit and m["B"] < dark and m["Floor"] > lit,
           f"A={m['A']:.3f} B={m['B']:.5f} Floor={m['Floor']:.3f}")

    # T6: light in {gL,g2} links object A in {g2} (floor stays gL)
    m = obj_means(*render(base_scene().replace(
                          "scene.objects.A.linkgroups = gL",
                          "scene.objects.A.linkgroups = g2", 1)
                          + light_point("gL,g2"), cpu))
    record("T6 multi-group", m["A"] > lit and m["B"] < dark and m["Floor"] > lit,
           f"A={m['A']:.3f} B={m['B']:.5f} Floor={m['Floor']:.3f}")

    # T7: GPU parity on the T1 scene (Metal or Vulkan device on Apple)
    sel = device_mask("METAL_GPU") or device_mask("VULKAN_GPU")
    if sel:
        try:
            mc = obj_means(*render(base_scene() + light_point("gL"), cpu))
            mg = obj_means(*render(base_scene() + light_point("gL"),
                                   "PATHOCL", sel))
            ok = (mg["A"] > lit and mg["B"] < dark and mg["Floor"] > lit
                  and abs(mg["A"] - mc["A"]) < 0.3 * mc["A"])
            record("T7 GPU parity", ok,
                   f"cpu A={mc['A']:.3f} B={mc['B']:.5f} | "
                   f"gpu A={mg['A']:.3f} B={mg['B']:.5f}")
        except Exception as e:
            record("T7 GPU parity", False, f"exception: {e}")
    else:
        record("T7 GPU parity", True, "no GPU device - skipped")

    # T8: ToProperties roundtrip re-emits linkgroups/linkmode
    scn = pysuperluxcore.Properties()
    scn.SetFromString(base_scene("scene.objects.A.linkmode = exclude\n")
                      + light_point("gL"))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)
    out = str(sc.ToProperties())
    ok = ("linkgroups" in out and "linkmode" in out
          and "gL" in out)
    record("T8 roundtrip", ok,
           f"linkgroups={'linkgroups' in out} linkmode={'linkmode' in out}")

    fails = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(fails)}/{len(results)} passed"
          + (f" - failed: {fails}" if fails else ""))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
