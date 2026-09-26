#!/usr/bin/env python3
"""LPE 720p visual check: luxball-hdr (infinite env light) decomposed
into camera-visible env (CE), direct diffuse (C<RD>E), indirect GI
(C<RD><RD>+E), glossy-reflected env (C<GR>E) and the total (C.*E).
Beauty goes through the AgX Punchy OCIO pipeline; LPE channels are
saved as EXR and previewed with a shared-exposure sRGB preview."""
import sys, os, time
from pathlib import Path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "out/build/src/pysuperluxcore/Release"))
import numpy as np
import pysuperluxcore
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
OUTDIR = Path("/tmp/e42_lpe_visual"); OUTDIR.mkdir(exist_ok=True)
W, H, SPP = 1280, 720, 256
OCIO = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/colormanagement/config.ocio"

LPES = (("CE", "env"), ("C<RD>E", "direct"), ("C<RD><RD>+E", "indirect"),
        ("C<RS>.*E", "specular"), ("C.*E", "total"))


def device_mask(want="METAL_GPU"):
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    m = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        m += "1" if t == want else "0"
        i += 1
    return m or None


def main():
    sc = pysuperluxcore.Scene()
    sc.Parse(pysuperluxcore.Properties(str(REPO / "scenes/luxball/luxball-hdr.scn")))

    lpe_cfg = "".join(f"film.lpe.{i}.expression = {e}\nfilm.lpe.{i}.name = {n}\n"
                      for i, (e, n) in enumerate(LPES))
    lpe_out = "".join(f"film.outputs.lpe{i}.type = LPE\nfilm.outputs.lpe{i}.index = {i}\n"
                      f"film.outputs.lpe{i}.filename = {OUTDIR}/lpe_{n}.exr\n"
                      for i, (e, n) in enumerate(LPES))

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = PATHOCL
sampler.type = SOBOL
batch.haltspp = {SPP}
opencl.task.count = 1048576
{lpe_cfg}
film.imagepipelines.0.0.type = TONEMAP_OPENCOLORIO
film.imagepipelines.0.0.mode = DISPLAY_CONVERSION
film.imagepipelines.0.0.config = {OCIO}
film.imagepipelines.0.0.src = "Linear Rec.709"
film.imagepipelines.0.0.display = sRGB
film.imagepipelines.0.0.view = AgX
film.imagepipelines.0.0.look = "AgX - Punchy"
film.outputs.beauty.type = RGB_IMAGEPIPELINE
film.outputs.beauty.index = 0
film.outputs.beauty.filename = {OUTDIR}/beauty_agx.png
{lpe_out}
""")
    sel = device_mask()
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + 900
    while True:
        ses.UpdateStats()
        p = ses.GetStats().Get("stats.renderengine.pass").GetInt()
        if p >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"stalled at {p}/{SPP}")
        time.sleep(1.0)

    film = ses.GetFilm()
    film.SaveOutputs()  # beauty PNG + LPE EXRs

    beauty = np.empty(W * H * 3, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, beauty, 0, True)
    beauty = beauty.reshape(H, W, 3)
    ses.Stop()

    chans = []
    for i, (e, n) in enumerate(LPES):
        buf = np.empty(W * H * 3, dtype=np.float32)
        film.GetOutputFloat(pysuperluxcore.FilmOutputType.LPE, buf, i, True)
        chans.append(buf.reshape(H, W, 3))
        print(f"  {n:9s} {e:14s} mean-lum={buf.mean():.4f}")

    # Shared-exposure preview strip: each channel / beauty-p95 -> sRGB
    ref = np.percentile(np.clip(np.stack(chans[-1:]), 0, None), 95)
    strip = []
    for a in chans:
        g = np.clip(a / max(ref, 1e-6), 0, 1) ** (1 / 2.2)
        strip.append((g * 255).astype(np.uint8))
    Image.fromarray(np.concatenate(strip, axis=1)).save(OUTDIR / "lpe_strip.png")

    b = np.clip(beauty, 0, 1)
    Image.fromarray((b * 255).astype(np.uint8)).save(OUTDIR / "beauty.png")
    print(f"saved {OUTDIR}/beauty.png, lpe_strip.png, lpe_*.exr, beauty_agx.png")


if __name__ == "__main__":
    main()
