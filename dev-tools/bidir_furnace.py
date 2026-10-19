"""Raw CPU transport oracles; SSS mode is a comparison, not an albedo oracle.

Use separate processes for different native modules. No denoising, adaptive
sampling, regularization, light tracing partition, photon cache or MNEE.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('--module', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--engine', choices=('PATHCPU', 'BIDIRCPU'), default='BIDIRCPU')
parser.add_argument('--mode', choices=('volume', 'matte', 'translucent', 'sss'), required=True)
parser.add_argument('--spp', type=int, default=65536)
parser.add_argument('--seed', type=int, default=17)
parser.add_argument('--rr', type=int, default=3)
parser.add_argument('--sigma', type=float, default=2.)
parser.add_argument('--radius', type=float, default=.085)
parser.add_argument('--tolerance', type=float, default=.002)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.out.resolve()
assert not out.exists(), out
out.mkdir(parents=True)
sys.path.insert(0, str(args.module.resolve()))
import pysuperluxcore as s
s.Init()
os.chdir(root)
controls = {
    'renderengine.type': args.engine, 'renderengine.seed': args.seed,
    'sampler.type': 'SOBOL', 'sampler.sobol.adaptive.strength': 0.,
    'native.threads.count': 1, 'film.width': 4, 'film.height': 4,
    'film.filter.type': 'NONE', 'film.outputs.raw.type': 'RGB',
    'film.outputs.raw.filename': 'unused.exr',
    'film.outputs.count.type': 'SAMPLECOUNT', 'film.outputs.count.filename': 'unused.exr',
    'batch.haltspp': args.spp, 'batch.halttime': 0,
    'path.maxdepth': 1024, 'light.maxdepth': 1024,
    'path.pathdepth.total': 1024, 'path.pathdepth.volume': 1024,
    'path.pathdepth.diffuse': 1024, 'path.pathdepth.glossy': 1024,
    'path.pathdepth.specular': 1024, 'path.russianroulette.depth': args.rr,
    'path.spectral.enable': False, 'path.hybridbackforward.enable': False,
    'path.lighttracing.auto': False, 'path.lighttracing.enable': False,
    'path.mnee.auto': False, 'path.mnee.enable': False,
    'path.photongi.indirect.enabled': False, 'path.photongi.caustic.enabled': False,
    'path.regularization.auto': False, 'path.regularization.sigma': 0.,
    'path.regularization.halflife': 0., 'path.filterglossy': 0.,
    'path.clamping.cycles.direct': 0., 'path.clamping.cycles.indirect': 0.,
    'path.clamping.variance.maxvalue': 0.,
}
scene_source = root/'dev-tools/e53_sss_energy.py'
fixture = next(ast.literal_eval(n.value) for n in ast.parse(scene_source.read_text()).body
               if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'SCENE' for t in n.targets))
with tempfile.TemporaryDirectory(prefix='slc-bidir-furnace-') as scratch:
    if args.mode == 'volume':
        text = f'''
scene.camera.lookat.orig=-2.776 2.76 3.279
scene.camera.lookat.target=-2.776 2.76 4.279
scene.camera.up=0 1 0
scene.camera.fieldofview=2
scene.camera.autovolume.enable=0
scene.camera.volume=fog
scene.lights.env.type=constantinfinite
scene.lights.env.color=1 1 1
scene.volumes.fog.type=homogeneous
scene.volumes.fog.ior=1
scene.volumes.fog.absorption=0 0 0
scene.volumes.fog.scattering={args.sigma} {args.sigma} {args.sigma}
scene.volumes.fog.asymmetry=0
scene.volumes.fog.multiscattering=1
scene.materials.boundary.type=null
scene.materials.boundary.volume.interior=fog
scene.objects.boundary.material=boundary
scene.objects.boundary.ply=scenes/cornell/sphere-mid.ply
'''
        expected = 1.
    else:
        text = fixture.replace('0.0085 0.0085 0.0085', ' '.join([str(args.radius)]*3))
        expected = .8 if args.mode == 'matte' else 1. if args.mode == 'translucent' else None
        if args.mode == 'translucent':
            text = text.replace('scene.materials.ball.type = openpbr', 'scene.materials.ball.type = mattetranslucent')
            text += '\nscene.materials.ball.kr = 0 0 0\nscene.materials.ball.kt = 1 1 1\n'
            text += f'\nscene.materials.ball.volume.interior = fog\nscene.volumes.fog.type = homogeneous\nscene.volumes.fog.ior = 1\nscene.volumes.fog.absorption = 0 0 0\nscene.volumes.fog.scattering = {args.sigma} {args.sigma} {args.sigma}\nscene.volumes.fog.asymmetry = 0\nscene.volumes.fog.multiscattering = 1\n'
        if args.mode in ('matte', 'translucent'):
            if args.mode == 'matte':
                text = text.replace('scene.materials.ball.type = openpbr', 'scene.materials.ball.type = matte')
                text += '\nscene.materials.ball.kd = .8 .8 .8\n'
            lines = (root/'scenes/cornell/sphere-mid.ply').read_text().splitlines()
            end = lines.index('end_header')
            vertices = int(next(x for x in lines[:end] if x.startswith('element vertex')).split()[-1])
            header = [x for x in lines[:end+1] if x not in ('property float nx', 'property float ny', 'property float nz')]
            body = [' '.join(x.split()[:3]) for x in lines[end+1:end+1+vertices]] + lines[end+1+vertices:]
            flat = out/'fixture-flat.ply'
            flat.write_text('\n'.join(header+body)+'\n')
            text = text.replace('scenes/cornell/sphere-mid.ply', str(flat))
    properties = s.Properties()
    properties.SetFromString(text)
    scene = s.Scene()
    scene.Parse(properties)
    cfg = s.Properties()
    for key, value in controls.items():
        cfg.Set(s.Property(key, value))
    config = s.RenderConfig(cfg, scene)
    (out/'scene.scn').write_text(text)
    (out/'render.cfg').write_text(str(config.GetProperties()))
    renderer = s.RenderSession(config)
    started = time.monotonic()
    try:
        renderer.Start()
        deadline = time.monotonic()+90
        while not renderer.HasDone():
            if time.monotonic() > deadline:
                raise TimeoutError('bounded CPU furnace')
            time.sleep(.01)
            renderer.UpdateStats()
        renderer.Stop()
        raw = np.empty((4,4,3), np.float32)
        counts = np.empty((4,4), np.uint32)
        renderer.GetFilm().GetOutputFloat(s.FilmOutputType.RGB, raw, 0, False)
        renderer.GetFilm().GetOutputUInt(s.FilmOutputType.SAMPLECOUNT, counts, 0, False)
    finally:
        if renderer.IsStarted():
            renderer.Stop()
assert np.isfinite(raw).all()
np.save(out/'linear.npy', raw)
np.save(out/'samples.npy', counts)
mean = raw.mean(axis=(0,1), dtype=float)
passed = expected is None or bool(np.max(np.abs(mean-expected)) <= args.tolerance)
record = dict(engine=args.engine,mode=args.mode,seed=args.seed,rr=args.rr,
              sigma=args.sigma,radius=args.radius,requested_spp=args.spp,
              expected=expected,oracle_validated=(expected is not None and passed),tolerance=args.tolerance,mean=mean.tolist(),passed=passed,
              counts_min=int(counts.min()),counts_max=int(counts.max()),
              controls=controls,seconds=time.monotonic()-started,
              native_sha256=hashlib.sha256(Path(s.pysuperluxcore.__file__).read_bytes()).hexdigest(),
              source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              scope='Raw finite CPU furnace; SSS has no closed-form albedo oracle; not a performance gate')
(out/'result.json').write_text(json.dumps(record,indent=2))
print(json.dumps(record),flush=True)
assert passed, record
