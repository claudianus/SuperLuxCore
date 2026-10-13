#!/usr/bin/env python3
"""Zero-config light pass gate (RenderConfig::ApplyAutoLightTracing).

The eye/light partition never hands a glossy vertex rougher than
path.hybridbackforward.terminalglossiness (0.3) to the light pass, so a scene
whose only non-diffuse materials are rougher than that must keep the light
pass off (its 25% task share deposited nothing: 300 tiny bulbs over rough
brass were ~30% noisier at equal time, bronze statues 31% on CPU / 14% on
Metal, 2026-10-12). A scattering medium only gives the light pass paths once
a non-diffuse surface exists (causticHasSurface): matte + fog keeps it off
(scenes/media: CPU spent 43 of 77 passes on the light pass, relRMSE 0.27 vs
0.18). MNEE/PSR keep the wider signature. Config-only test.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore as lux

BASE = """
scene.camera.lookat.orig = 0 -3 1
scene.camera.lookat.target = 0 0 0
scene.lights.sun.type = sun
scene.lights.sun.dir = 0 0 1
scene.materials.floor.type = matte
scene.objects.floor.material = floor
scene.objects.floor.vertices = -1 -1 0 1 -1 0 1 1 0 -1 1 0
scene.objects.floor.faces = 0 1 2 0 2 3
scene.objects.thing.material = thing
scene.objects.thing.vertices = -.2 0 0 .2 0 0 .2 0 .4 -.2 0 .4
scene.objects.thing.faces = 0 1 2 0 2 3
"""

CASES = [
    # (name, material lines, extra cfg, expect lt, expect mnee)
    ("diffuse only", "scene.materials.thing.type = matte", "", "-", "-"),
    ("rough metal r=.6", "scene.materials.thing.type = metal2\nscene.materials.thing.uroughness = .6\nscene.materials.thing.vroughness = .6", "", "-", "1"),
    ("rough disney metal r=.4", "scene.materials.thing.type = disney\nscene.materials.thing.metallic = 1\nscene.materials.thing.roughness = .4", "", "-", "1"),
    ("sharp metal r=.05", "scene.materials.thing.type = metal2\nscene.materials.thing.uroughness = .05\nscene.materials.thing.vroughness = .05", "", "1", "1"),
    ("glass", "scene.materials.thing.type = glass", "", "1", "1"),
    ("rough metal, terminalglossiness .8", "scene.materials.thing.type = metal2\nscene.materials.thing.uroughness = .6\nscene.materials.thing.vroughness = .6", "path.hybridbackforward.terminalglossiness = .8", "1", "1"),
    ("rough metal, pinned enable", "scene.materials.thing.type = metal2\nscene.materials.thing.uroughness = .6\nscene.materials.thing.vroughness = .6", "path.lighttracing.enable = 1", "1", "1"),
    ("matte + fog", "scene.materials.thing.type = matte\nscene.volumes.fog.type = homogeneous\nscene.volumes.fog.scattering = .1 .1 .1\nscene.world.volume.default = fog", "", "-", "1"),
    ("rough metal + fog", "scene.materials.thing.type = metal2\nscene.materials.thing.uroughness = .6\nscene.materials.thing.vroughness = .6\nscene.volumes.fog.type = homogeneous\nscene.volumes.fog.scattering = .1 .1 .1\nscene.world.volume.default = fog", "", "1", "1"),
]


def main():
    lux.Init(lambda s: None)
    fails = 0
    for name, mat, extra, want_lt, want_mnee in CASES:
        p = lux.Properties(); p.SetFromString(BASE + mat + "\n")
        sc = lux.Scene(); sc.Parse(p)
        c = lux.Properties(); c.SetFromString("renderengine.type = PATHCPU\n" + extra + "\n")
        props = lux.RenderConfig(c, sc).GetProperties()
        get = lambda k: props.Get(k).GetString() if props.IsDefined(k) else "-"
        lt, mnee = get("path.lighttracing.enable"), get("path.mnee.enable")
        ok = (lt == want_lt) and (mnee == want_mnee)
        fails += not ok
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: lt={lt} (want {want_lt}) mnee={mnee} (want {want_mnee})", flush=True)
    print(f"{fails} failure(s)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
