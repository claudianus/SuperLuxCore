"""Bounded OpenPBR SSS energy, exact-output and CPU cost regression.

The furnace isolates transport from exposure and denoising. The legacy case
models the Blender export before the default-volume fix; auto uses the same
surface limits with no explicit volume override. No transport defaults change.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import time

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--module', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--spp', type=int, default=256)
    ap.add_argument('--size', type=int, default=16)
    ap.add_argument('--threads', type=int, default=1)
    ap.add_argument('--seed', type=int, default=17)
    ap.add_argument('--rrdepth', type=int)
    ap.add_argument('--sss-fastloop',choices=['auto','on','off'],default='auto')
    ap.add_argument('--aovs',action='store_true')
    ap.add_argument('--hybrid',action='store_true')
    ap.add_argument('--mnee',action='store_true')
    ap.add_argument('--sss-guiding',choices=['auto','on','off'],default='auto')
    ap.add_argument('--cases', default='legacy,auto,chromatic,spectral,textured,anisotropic,vdh,point')
    ap.add_argument('--tiled', action='store_true', help='Exact finite per-pixel sample budget')
    ap.add_argument('--engine', choices=['PATHCPU','PATHOCL','TILEPATHOCL'], default='PATHCPU')
    args = ap.parse_args()
    allowed_cases = {'legacy','auto','chromatic','spectral','textured','anisotropic',
                     'vdh','point','litpoint','interior','blacktextured','shadowtransparent','nested'}
    unknown_cases = set(args.cases.split(',')) - allowed_cases
    if unknown_cases:
        ap.error('Unknown SSS cases: ' + ', '.join(sorted(unknown_cases)))
    args.out = args.out.resolve()
    if args.tiled and math.isqrt(args.spp)**2 != args.spp:
        ap.error('--tiled requires a square --spp')
    if args.engine == 'TILEPATHOCL' and not args.tiled:
        ap.error('TILEPATHOCL requires --tiled')
    args.out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(Path(args.module).resolve()))
    import pysuperluxcore as s
    s.Init()
    mask = None
    if args.engine != 'PATHCPU':
        desc = s.GetOpenCLDeviceDescs()
        mask = ''.join('1' if desc.Get(n+'.type').GetString() == 'METAL_GPU' else '0'
                       for n in desc.GetAllUniqueSubNames('opencl.device'))
        if '1' not in mask:
            raise RuntimeError('No Metal GPU available')
    repo = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location('sss_furnace', repo/'dev-tools/e53_sss_energy.py')
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    os.chdir(repo)

    def props(text):
        p = s.Properties()
        p.SetFromString(text)
        return p

    rows = []
    for case in args.cases.split(','):
        chromatic = case in ('chromatic', 'spectral', 'point', 'litpoint')
        scene_text = fixture.SCENE
        if chromatic:
            scene_text = scene_text.replace('0.0085 0.0085 0.0085', '0.0085 0.00425 0.002125')
        if case == 'point':
            # A zero-power point light must not degrade a chromatic walk.
            # It used to activate CPU equiangular distance sampling ahead
            # of SSS, bypassing the throughput-weighted channel mixture.
            scene_text += '''
scene.lights.zero.type = point
scene.lights.zero.position = 0 0 8
scene.lights.zero.gain = 0 0 0
'''
        if case == 'litpoint':
            scene_text += '''
scene.lights.lit.type = point
scene.lights.lit.position = -2.776 1.0 3.279
scene.lights.lit.gain = 100 100 100
'''
        if case == 'textured':
            scene_text = scene_text.replace('subsurfacecolor = 0.8 0.8 0.8', 'subsurfacecolor = varying')
            scene_text += '''
scene.textures.varying.type = checkerboard3d
scene.textures.varying.texture1 = .2 .5 .7
scene.textures.varying.texture2 = .7 .3 .1
'''
        if case == 'anisotropic':
            scene_text += '\nscene.materials.ball.subsurfaceanisotropy = .6\n'
        if case == 'vdh':
            scene_text += '''
scene.volumes.test.type = homogeneous
scene.volumes.test.ior = 1.5
scene.volumes.test.multiscattering = 1
scene.volumes.test.phase = hg
scene.volumes.test.sssalbedo = .8 .8 .8
scene.volumes.test.sssmfp = .0085 .0085 .0085
scene.volumes.test.sssprofile = vandehulst
scene.materials.ball.volume.interior = test
'''
        if case == 'interior':
            # A finite emitter inside the body exercises volume NEE and
            # emitter-hit MIS, beyond an opaque exterior furnace.
            scene_text=scene_text.replace('0.0085 0.0085 0.0085','.2 .2 .2')
            scene_text += '''
scene.lights.env.gain = 0 0 0
scene.materials.inner.type = matte
scene.materials.inner.kd = 0 0 0
scene.materials.inner.emission = 20 20 20
scene.objects.inner.material = inner
scene.objects.inner.vertices = -2.976 2.15 3.079  -2.576 2.15 3.079  -2.576 2.15 3.479  -2.976 2.15 3.479
scene.objects.inner.faces = 0 1 2 0 2 3
'''
        if case == 'blacktextured':
            scene_text += '''
scene.textures.blackcells.type = checkerboard3d
scene.textures.blackcells.texture1 = 0 0 0
scene.textures.blackcells.texture2 = .8 .8 .8
scene.volumes.blackcells.type = homogeneous
scene.volumes.blackcells.ior = 1.5
scene.volumes.blackcells.multiscattering = 1
scene.volumes.blackcells.phase = hg
scene.volumes.blackcells.sssalbedo = blackcells
scene.volumes.blackcells.sssmfp = .0085 .0085 .0085
scene.volumes.blackcells.sssprofile = vandehulst
scene.materials.ball.volume.interior = blackcells
'''
        if case == 'shadowtransparent':
            scene_text += '\nscene.materials.ball.transparency.shadow = .6 .6 .6\n'
        if case == 'nested':
            scene_text=scene_text.replace('0.0085 0.0085 0.0085','.2 .2 .2')
            scene_text += '''
scene.volumes.void.type = clear
scene.volumes.void.ior = 1
scene.volumes.void.absorption = .2 .4 .8
scene.volumes.void.priority = 1
scene.materials.nested.type = null
scene.materials.nested.volume.interior = void
scene.objects.nested.material = nested
scene.objects.nested.ply = scenes/cornell/sphere-mid.ply
scene.objects.nested.transformation = .5 0 0 0  0 .5 0 0  0 0 .5 0  -1.388 1.38 1.6395 1
'''
        scene = s.Scene()
        scene.Parse(props(scene_text))
        cfg = props(f'''
renderengine.type = {args.engine}
renderengine.seed = {args.seed}
sampler.type = SOBOL
native.threads.count = {args.threads}
film.width = {args.size}
film.height = {args.size}
film.hw.enable = 0
film.filter.type = NONE
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = RGB
film.outputs.0.filename = unused.exr
film.outputs.1.type = SAMPLECOUNT
film.outputs.1.filename = unused.exr
batch.haltspp = {args.spp}
path.pathdepth.total = 13
path.pathdepth.diffuse = 5
path.pathdepth.glossy = 5
path.pathdepth.specular = 12
path.spectral.enable = {int(case == 'spectral')}
path.hybridbackforward.enable = 0
path.lighttracing.auto = 0
path.lighttracing.enable = 0
path.mnee.auto = 0
path.photongi.indirect.enabled = 0
path.photongi.caustic.enabled = 0
path.clamping.variance.maxvalue = 0
''')
        if case == 'legacy':
            cfg.Set(s.Property('path.pathdepth.volume', 0))
        if args.rrdepth is not None:
            cfg.Set(s.Property('path.russianroulette.depth', args.rrdepth))
        if args.hybrid:
            cfg.Set(s.Property('path.hybridbackforward.enable',True))
        if args.mnee:
            cfg.Set(s.Property('path.mnee.enable',True))
        if args.sss_fastloop != 'auto':
            cfg.Set(s.Property('path.sss.fastloop.enable',args.sss_fastloop=='on'))
        if args.aovs:
            for name in ['ALBEDO','SHADING_NORMAL','IRRADIANCE','DIRECT_DIFFUSE','INDIRECT_DIFFUSE','EMISSION']:
                cfg.Set(s.Property('film.outputs.test'+name+'.type',name))
                cfg.Set(s.Property('film.outputs.test'+name+'.filename','unused.exr'))
        if args.sss_guiding != 'auto':
            cfg.Set(s.Property('path.sss.guiding.enable',args.sss_guiding=='on'))
        if mask:
            cfg.SetFromString(f'''opencl.devices.select = {mask}
opencl.native.threads.count = 0
opencl.task.count = {max(8192,64*args.spp) if args.tiled else 8192}
''')
        if args.tiled:
            cfg.SetFromString(f'''renderengine.type = {'TILEPATHCPU' if args.engine == 'PATHCPU' else 'TILEPATHOCL'}
sampler.type = TILEPATHSAMPLER
tile.size = {16 if args.engine == 'PATHCPU' else 8}
tile.multipass.enable = 0
tilepath.sampling.aa.size = {math.isqrt(args.spp)}
''')
        config = s.RenderConfig(cfg, scene)
        effective = str(config.GetProperties())
        (args.out/(case+'.cfg')).write_text(effective)
        ses = s.RenderSession(config)
        start = time.monotonic()
        try:
            ses.Start()
            preparation = time.monotonic()-start
            deadline = time.monotonic()+180
            while not ses.HasDone():
                if time.monotonic() > deadline:
                    raise TimeoutError(case)
                time.sleep(.01)
                ses.UpdateStats()
            ses.Stop()
            elapsed = time.monotonic()-start
            rgb = np.empty((args.size,args.size,3), np.float32)
            counts = np.empty((args.size,args.size), np.uint32)
            ses.GetFilm().GetOutputFloat(s.FilmOutputType.RGB, rgb, 0, False)
            ses.GetFilm().GetOutputUInt(s.FilmOutputType.SAMPLECOUNT, counts, 0, False)
            assert np.isfinite(rgb).all(), case
            if args.tiled:
                assert np.all(counts == args.spp), (case,counts.min(),counts.max())
            if case == 'legacy':
                assert float(rgb.mean()) < .3, 'fixture no longer detects truncated SSS'
            elif case == 'auto':
                assert .75 < float(rgb.mean()) < .83, 'SSS furnace energy regression'
            elif case == 'point':
                assert .7 < float(rgb.mean()) < .9, 'point light bypassed chromatic SSS'
            np.save(args.out/(case+'.npy'), rgb)
            if args.aovs:
                for name in ['ALBEDO','SHADING_NORMAL','IRRADIANCE','DIRECT_DIFFUSE','INDIRECT_DIFFUSE','EMISSION']:
                    buffer=np.empty_like(rgb)
                    ses.GetFilm().GetOutputFloat(getattr(s.FilmOutputType,name),buffer,0,False)
                    np.save(args.out/(case+'-'+name+'.npy'),buffer)
            np.save(args.out/(case+'-counts.npy'), counts)
            rows.append(dict(case=case,mean=rgb.mean(axis=(0,1),dtype=np.float64).tolist(),
                             finite=bool(np.isfinite(rgb).all()),seconds=elapsed,
                             preparation_seconds=preparation,spp=args.spp,
                             size=args.size,threads=args.threads,
                             seed=args.seed,rrdepth=args.rrdepth,
                             sss_guiding=args.sss_guiding,sss_fastloop=args.sss_fastloop,
                             min_samples=int(counts.min()),max_samples=int(counts.max())))
            print(json.dumps(rows[-1]), flush=True)
        finally:
            if ses.IsStarted():
                ses.Stop()
    native = Path(getattr(s,'pysuperluxcore',s).__file__)
    zero_light = None
    if args.tiled and {'point','chromatic'}.issubset(args.cases.split(',')):
        point,chromatic=np.load(args.out/'point.npy'),np.load(args.out/'chromatic.npy')
        zero_light=dict(bit_exact=bool(np.array_equal(point,chromatic)),
                        max_absolute_error=float(np.max(np.abs(point-chromatic))))
        # A different GPU light list compiles a different kernel specialization;
        # allow one small floating-point rounding difference, not a transport shift.
        assert zero_light['bit_exact'] if args.engine=='PATHCPU' else np.allclose(point,chromatic,rtol=1e-6,atol=1e-7), \
            ('A zero-power point light changed SSS transport',zero_light)
    (args.out/'results.json').write_text(json.dumps(dict(
        native=str(native),sha256=hashlib.sha256(native.read_bytes()).hexdigest(),
        zero_light=zero_light,cases=rows),indent=2))


if __name__ == '__main__':
    main()
