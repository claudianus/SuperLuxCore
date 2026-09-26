#!/usr/bin/env python3
"""P5 path-guiding 720p visual check: pg-gallery - a hall lit ONLY by
warm light bouncing through a slit from a hidden rear compartment.
Unguided vs guided vs guided+RIS at equal low spp on PATHOCL/Metal, plus
a converged reference for RMSE. All beauty output goes through the AgX
Punchy OCIO pipeline.
"""
import sys, os, time
from pathlib import Path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "out/build/src/pysuperluxcore/Release"))
import numpy as np
import pysuperluxcore
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
OUTDIR = Path("/tmp/e43_pg_visual"); OUTDIR.mkdir(exist_ok=True)
W, H = 1280, 720
OCIO = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/colormanagement/config.ocio"


def device_mask(want="METAL_GPU"):
    pysuperluxcore.Init()
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


def render(scene, name, spp, guiding, seed=7, extra=""):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = PATHOCL
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
opencl.task.count = 1048576
path.guiding.enable = {1 if guiding else 0}
{extra}
film.imagepipelines.0.0.type = TONEMAP_OPENCOLORIO
film.imagepipelines.0.0.mode = DISPLAY_CONVERSION
film.imagepipelines.0.0.config = {OCIO}
film.imagepipelines.0.0.src = "Linear Rec.709"
film.imagepipelines.0.0.display = sRGB
film.imagepipelines.0.0.view = AgX
film.imagepipelines.0.0.look = "AgX - Punchy"
""")
    sel = device_mask()
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + 1800
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(name)
        time.sleep(1.0)
    film = ses.GetFilm()
    raw = np.empty(W * H * 3, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB, raw, 0, True)
    beauty = np.empty(W * H * 3, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                        beauty, 0, True)
    ses.Stop()
    # Film buffers are bottom-up rows.
    Image.fromarray((np.clip(np.flipud(beauty.reshape(H, W, 3)), 0, 1)
                     * 255).astype(np.uint8)).save(OUTDIR / f"{name}.png")
    raw = np.flipud(raw.reshape(H, W, 3))
    np.save(OUTDIR / f"{name}.npy", raw)
    return raw


def rmse(a, ref, mask=None):
    d = (a - ref) ** 2
    if mask is not None:
        d = d[mask]
    return float(np.sqrt(d.mean()))


def main():
    scene = pysuperluxcore.Scene()
    scene.Parse(pysuperluxcore.Properties(
        str(REPO / "scenes/cornell/pg-gallery.scn")))

    table = OUTDIR / "pg_table.bin"
    # Converged reference; also trains + saves a guide table for
    # warm-starting the production renders (the real-world workflow:
    # reuse a baked guide instead of relearning every frame).
    ref = render(scene, "ref_1024", 1024, guiding=True, seed=3,
                 extra=f"path.guiding.savetable = {table}")
    warm = f"path.guiding.tablefile = {table}"
    u = render(scene, "unguided_48", 48, guiding=False)
    g = render(scene, "guided_48", 48, guiding=True)
    gw = render(scene, "guided_warm_48", 48, guiding=True, extra=warm)
    gr = render(scene, "guided_ris_warm_48", 48, guiding=True,
                extra=warm + "\npath.guiding.risk = 4")

    # RMSE over the whole frame is dominated by the blown window; also
    # report the indirect region (pixels below the ref's 90th percentile
    # luminance) where bounce sampling actually decides the noise.
    refl = ref[..., 0] * .2126 + ref[..., 1] * .7152 + ref[..., 2] * .0722
    ind = refl < np.quantile(refl, .90)
    for tag, a in (("unguided", u), ("guided", g), ("guided warm", gw),
                   ("guided+RIS", gr)):
        r_all = rmse(a, ref)
        r_ind = rmse(a, ref, ind)
        print(f"{tag:11s} RMSE={r_all:.4f} ({rmse(u, ref)/r_all:.2f}x)"
              f"  indirect={r_ind:.4f} ({rmse(u, ref, ind)/r_ind:.2f}x)")
    for a in (u, g, gw, gr, ref):
        assert np.isfinite(a).all()
    assert abs(g.mean() / ref.mean() - 1) < .15

    # Side-by-side strip for quick visual compare.
    imgs = [np.asarray(Image.open(OUTDIR / f"{n}.png")) for n in
            ("unguided_48", "guided_48", "guided_warm_48",
             "guided_ris_warm_48", "ref_1024")]
    Image.fromarray(np.concatenate(imgs, axis=0)).save(
        OUTDIR / "compare.png")
    print(f"images: {OUTDIR}")


main()
