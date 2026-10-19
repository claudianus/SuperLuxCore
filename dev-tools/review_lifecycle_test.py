"""PhotonGI edit/resume and orthographic footprint regressions (bounded children).

Run with python3.13; --baseline-wheel uses the installed 2.11.34 wheel.
No large output is saved. A killed child is reaped and its scratch is removed.
"""
import ast
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get("SLC_REVIEW_OUT", str(ROOT.parent / "dev-tools/reviews/2026-10-16/fixes")))


def child(mode):
    sys.path.insert(0, os.environ.get("SLC_REVIEW_MODULE_ROOT", str(ROOT / "out/build/src/pysuperluxcore/Release")))
    import numpy as np
    import pysuperluxcore as slc
    slc.Init()
    env = {}
    tree = ast.parse((ROOT / "dev-tools/e117_viewport_caustic_cache_test.py").read_text())
    nodes = [n for n in tree.body if
             isinstance(n, ast.FunctionDef) and n.name in ("box", "camera") or
             isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in ("SCENE", "DEPTH") for t in n.targets)]
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "e117-fixture", "exec"), env)
    def props(text):
        p = slc.Properties(); p.SetFromString(text); return p
    def scene(dark=False):
        s = slc.Scene(); s.Parse(props(env["SCENE"]))
        if dark: s.Parse(props("scene.materials.light.emission = 0 0 0"))
        return s
    def pump(s, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            s.UpdateStats(); time.sleep(.02)
    def mean(s):
        data = np.zeros(32 * 32 * 3, np.float32)
        s.GetFilm().GetOutputFloat(slc.FilmOutputType.RGB_IMAGEPIPELINE, data, 0, True)
        assert np.isfinite(data).all()
        return float(data.reshape(32, 32, 3)[10:22, 10:22].mean())
    settings = env["DEPTH"] + """
film.width = 32
film.height = 32
film.imagepipelines.0.0.type = NOP
renderengine.type = PATHCPU
sampler.type = RANDOM
native.threads.count = 2
path.lighttracing.auto = 0
path.hybridbackforward.enable = 0
path.mnee.enable = 0
path.mnee.auto = 0
path.guiding.enable = 0
path.photongi.indirect.enabled = 0
path.photongi.caustic.enabled = 1
path.photongi.caustic.lookup.radius = 0.1
path.photongi.caustic.updatespp.minradius = 0.001
path.photongi.visibility.lookup.radius = 0.5
path.photongi.caustic.maxsize = 4096
path.photongi.photon.maxcount = 100000
path.photongi.photon.maxdepth = 16
path.photongi.caustic.updatespp = 1
path.photongi.caustic.updatespp.radiusreduction = 0.96
"""
    if mode == "edit-metal":
        desc = slc.GetOpenCLDeviceDescs()
        selection = "".join("1" if desc.Get(n + ".type").GetString() == "METAL_GPU" else "0"
                            for n in desc.GetAllUniqueSubNames("opencl.device"))
        assert "1" in selection
        settings += f"""
renderengine.type = PATHOCL
opencl.task.count = 8192
opencl.native.threads.count = 0
opencl.devices.select = {selection}
"""
    sessions = []
    try:
        if mode.startswith("resume"):
            old, new = map(int, mode.split("-")[1:])
            cfg = slc.RenderConfig(props(settings + f"\nnative.threads.count = {old}"), scene())
            ses = slc.RenderSession(cfg); sessions.append(ses)
            ses.Start(); pump(ses, .7); ses.Pause(); time.sleep(.15)
            resume = Path(os.environ["SLC_REVIEW_SCRATCH"]) / "resume.rsm"
            ses.SaveResumeFile(str(resume)); ses.Resume(); ses.Stop()
            cfg2, state, film = slc.RenderConfig.LoadResumeFile(str(resume))
            cfg2.Parse(props(f"native.threads.count = {new}"))
            ses2 = slc.RenderSession(cfg2, state, film); sessions.append(ses2)
            ses2.Start(); pump(ses2, 1)
            start = ses2.GetStats().Get("stats.renderengine.pass").GetFloat()
            pump(ses2, 1)
            end = ses2.GetStats().Get("stats.renderengine.pass").GetFloat()
            print(json.dumps({"mode": mode, "version": slc.Version(), "pass_before": start, "pass_after": end}), flush=True)
            assert end > start, "resumed render is stuck"
        elif mode in ("edit", "edit-metal"):
            scn = scene(); cfg = slc.RenderConfig(props(settings), scn)
            ses = slc.RenderSession(cfg); sessions.append(ses)
            ses.Start(); pump(ses, 1)
            before = mean(ses)
            ses.BeginSceneEdit(); scn.Parse(props("scene.materials.light.emission = 0 0 0")); ses.EndSceneEdit()
            pump(ses, .6); after = mean(ses)
            ses.BeginSceneEdit(); scn.Parse(props("scene.materials.light.emission = 20 20 20")); ses.EndSceneEdit()
            pump(ses, .6); restored = mean(ses)
            print(json.dumps({"mode": mode, "version": slc.Version(), "before": before, "dark_after_edit": after, "restored": restored}), flush=True)
            assert before > 0 and after == 0 and restored > 0
        elif mode == "orthographic":
            for distance, scale in ((5, 2), (50, 2), (5, 4)):
                scn = slc.Scene()
                scn.Parse(props(f"""
scene.camera.type = orthographic
scene.camera.lookat.orig = 0 0 {distance}
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.screenwindow = {-scale} {scale} {-scale} {scale}
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.7 0.7 0.7
scene.objects.floor.material = floor
scene.objects.floor.vertices = -100 -100 0 100 -100 0 100 100 0 -100 100 0
scene.objects.floor.faces = 0 1 2 0 2 3
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1 1 1
"""))
                cfg = slc.RenderConfig(props(settings + """
path.photongi.caustic.updatespp = 0
path.photongi.caustic.lookup.radius = 0
path.photongi.caustic.maxsize = 1
path.photongi.photon.maxcount = 1
native.threads.count = 1
"""), scn)
                ses = slc.RenderSession(cfg); sessions.append(ses)
                ses.Start(); ses.Stop()
            print(json.dumps({"mode": mode, "version": slc.Version()}), flush=True)
        else:
            raise ValueError(mode)
    finally:
        for s in reversed(sessions):
            if s.IsStarted():
                if s.IsInPause(): s.Resume()
                s.Stop()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    modes = [x for x in sys.argv[1:] if not x.startswith("--")] or ["edit", "resume-2-1", "resume-1-2", "orthographic"]
    results = []
    with tempfile.TemporaryDirectory(prefix="slc-review-lifecycle-") as tmp:
        env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1", "LUX_PGIC_SATBACKOFF": "0", "SLC_REVIEW_SCRATCH": tmp}
        if "--baseline-wheel" in sys.argv:
            wheel = Path.home() / "Library/Application Support/Blender/5.2/extensions/user_default/superluxcore/wheels/pysuperluxcore-2.11.34-cp313-cp313-macosx_14_0_arm64.whl"
            with zipfile.ZipFile(wheel) as z:
                for n in z.namelist():
                    if n.startswith("pysuperluxcore/"): z.extract(n, tmp)
            env["SLC_REVIEW_MODULE_ROOT"] = tmp
        tag = "baseline" if "--baseline-wheel" in sys.argv else "fixed"
        for mode in modes:
            try:
                p = subprocess.run([sys.executable, str(__file__), "--child", mode], env=env, capture_output=True, text=True, timeout=240 if mode == "edit-metal" else 15)
                log, rc = p.stdout + p.stderr, p.returncode
                results.append({"mode": mode, "returncode": rc, "timed_out": False})
                if mode == "orthographic" and rc == 0:
                    radii = list(map(float, re.findall(r"PhotonGI best caustic cache radius: ([0-9.eE+-]+)", log)))
                    correct = len(radii) == 3 and abs(radii[1] / radii[0] - 1) < 1e-4 and abs(radii[2] / radii[0] - 2) < 1e-4
                    results[-1].update({"radii": radii, "distance_invariant_and_scale_linear": correct})
                    if not correct and tag == "fixed": results[-1]["returncode"] = 1
            except subprocess.TimeoutExpired as e:
                log = (e.stdout or b"").decode() + (e.stderr or b"").decode()
                results.append({"mode": mode, "returncode": None, "timed_out": True})
            (OUT / f"lifecycle-{tag}-{mode}.log").write_text(log)
            (OUT / f"lifecycle-{tag}-{mode}.json").write_text(json.dumps(results[-1], indent=2) + "\n")
            print(results[-1], flush=True)
    (OUT / f"lifecycle-{tag}.json").write_text(json.dumps(results, indent=2) + "\n")
    if tag == "fixed": assert all(r["returncode"] == 0 for r in results), results


if __name__ == "__main__":
    if "--child" in sys.argv: child(sys.argv[-1])
    else: main()
