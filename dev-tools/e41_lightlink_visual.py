#!/usr/bin/env python3
"""Light linking 720p visual check: one image with a gL-linked point light
(A + floor lit, B unlit) and one with a global light for contrast."""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "out/build/src/pysuperluxcore/Release"))
import numpy as np
import pysuperluxcore
from e41_lightlink_test import base_scene
from PIL import Image

LIGHT = ("scene.lights.L.type = point\n"
         "scene.lights.L.position = 0 -0.6 3.2\n"
         "scene.lights.L.color = 500 500 500\n"
         "scene.lights.L.gain = 1 1 1\n")


def render(scene_props, width=1280, height=720, spp=256):
    scn = pysuperluxcore.Properties()
    scn.SetFromString(scene_props)
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {width}
film.height = {height}
renderengine.type = PATHCPU
sampler.type = SOBOL
batch.haltspp = {spp}
path.pathdepth.total = 4
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = OBJECT_ID
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + 600
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError("render stalled")
        time.sleep(0.5)
    film = ses.GetFilm()
    rgb = np.empty(width * height * 3, dtype=np.float32)
    oid = np.empty(width * height, dtype=np.uint32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB, rgb)
    film.GetOutputUInt(pysuperluxcore.FilmOutputType.OBJECT_ID, oid)
    ses.Stop()
    return rgb.reshape(height, width, 3), oid.reshape(height, width)


def main():
    for tag, extra in (("linked", "scene.lights.L.linkgroups = gL\n"),
                       ("global", "")):
        scene = base_scene() + LIGHT + extra
        rgb, oid = render(scene)
        img = np.clip(rgb, 0, 1) ** (1 / 2.2)
        path = f"/tmp/superluxcore_lightlink_{tag}_720p.png"
        Image.fromarray((img * 255).astype(np.uint8)).save(path)
        lum = rgb.mean(axis=2)
        for oid_val, name in ((11, "A"), (12, "B"), (13, "Floor")):
            m = oid == oid_val
            print(f"  {tag} {name}: median={np.median(lum[m]):.4f}")
        print(f"saved {path}")


if __name__ == "__main__":
    main()
