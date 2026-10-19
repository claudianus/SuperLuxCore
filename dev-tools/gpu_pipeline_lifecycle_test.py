"""Real native GPU image pipeline lifecycle stress, including CPU controls.

Run with the candidate pysuperluxcore installed and NumPy. A GPU is mandatory
unless --cpu-only is specified; absence never becomes a passing GPU result.
Each completed iteration is journaled before the next potentially fatal call.
"""
import argparse
import gc
import hashlib
import json
import os
from pathlib import Path
import time

import numpy as np
import pysuperluxcore as slc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cpu-only', action='store_true')
    parser.add_argument('--iterations', type=int, default=12)
    parser.add_argument('--require-backend', choices=('CUDA_GPU','METAL_GPU','OPENCL_GPU'))
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    native_log = args.out.with_suffix('.native.log')
    try:
        saved_stderr = os.dup(2)
    except OSError:
        saved_stderr = None  # GUI-only Windows launch may have no stderr fd.
    with native_log.open('w') as stream:
        os.dup2(stream.fileno(), 2)
        try:
            run(args, native_log)
        finally:
            if saved_stderr is not None:
                os.dup2(saved_stderr, 2)
                os.close(saved_stderr)
            elif stream.fileno() != 2:
                os.close(2)


def run(args, native_log):
    args.out.parent.mkdir(parents=True, exist_ok=True)
    messages = []

    def logger(message):
        messages.append(str(message))

    slc.Init(logger)
    descriptions = slc.GetOpenCLDeviceDescs()
    devices = {name: descriptions.Get(name+'.type').GetString()
               for name in descriptions.GetAllUniqueSubNames('opencl.device')}
    if not args.cpu_only:
        assert any(value.endswith('_GPU') for value in devices.values()), devices
        if args.require_backend:
            assert args.require_backend in devices.values(), devices
    native = Path(slc.pysuperluxcore.__file__)
    report = dict(passed=False, GPU_required=not args.cpu_only, devices=devices,
                  native_sha256=hashlib.file_digest(native.open('rb'), 'sha256').hexdigest(),
                  records=[], current_operation='initialization', NGX_evaluations=0,
                  Blender_context=globals().get('PROBE_CONTEXT'))

    def journal(operation):
        report['current_operation'] = operation
        native_messages = native_log.read_text(errors='replace').splitlines()
        report['hardware_pipeline_log'] = [m for m in [*messages, *native_messages] if 'Film hardware device used:' in m]
        args.out.write_text(json.dumps(report, indent=2)+'\n')

    def props(text):
        result = slc.Properties()
        result.SetFromString(text)
        return result

    def pipeline(gamma):
        return props('film.imagepipelines.0.0.type = TONEMAP_AUTOLINEAR\n'
                     'film.imagepipelines.0.1.type = GAMMA_CORRECTION\n'
                     'film.imagepipelines.0.1.value = '+str(gamma))

    modes = ('cpu',) if args.cpu_only else ('cpu', 'gpu')
    for mode in modes:
        for iteration in range(args.iterations):
            journal(f'{mode}/{iteration}: create and trace')
            scene = slc.Scene()
            scene.Parse(props('''
scene.camera.lookat.orig = 0 -4 2
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 0 1
scene.lights.environment.type = constantinfinite
scene.lights.environment.color = 0.2 0.4 0.8
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.5 0.5 0.5
scene.objects.floor.material = floor
scene.objects.floor.vertices = -10 -10 0 10 -10 0 10 10 0 -10 10 0
scene.objects.floor.faces = 0 1 2 0 2 3
'''))
            settings = props(f'''
renderengine.type = PATHCPU
native.threads.count = 2
sampler.type = SOBOL
film.width = 64
film.height = 40
film.hw.enable = {int(mode == 'gpu')}
batch.haltspp = 2
path.lighttracing.auto = 0
path.hybridbackforward.enable = 0
path.mnee.enable = 0
path.guiding.enable = 0
''')
            if mode == 'gpu' and args.require_backend:
                selected = next(name for name, kind in devices.items() if kind == args.require_backend)
                settings.Set(slc.Property('film.hw.device', int(selected.rsplit('.', 1)[1])))
            settings.Set(pipeline(2.2))
            config = slc.RenderConfig(settings, scene)
            session = slc.RenderSession(config)
            session.Start()
            try:
                deadline = time.monotonic()+30
                while not session.HasDone():
                    assert time.monotonic() < deadline, 'Tracing deadline exceeded'
                    session.UpdateStats()
                    time.sleep(.01)
                session.Stop()
                film = session.GetFilm()
                raw = np.zeros(64*40*3, np.float32)
                film.GetOutputFloat(slc.FilmOutputType.RGB, raw, 0, False)
                assert np.isfinite(raw).all() and np.max(raw) > 0
                journal(f'{mode}/{iteration}: first GPU context on native async worker')
                film.AsyncExecuteImagePipeline(0)
                if iteration % 3 == 0:
                    # Parse must join active work before deleting its plugins.
                    session.Parse(pipeline(1.8))
                else:
                    deadline = time.monotonic()+120
                    while not film.HasDoneAsyncExecuteImagePipeline():
                        assert time.monotonic() < deadline, 'Image pipeline deadline exceeded'
                        time.sleep(.01)
                    film.WaitAsyncExecuteImagePipeline()
                    if iteration % 3 == 1:
                        session.Parse(pipeline(1.8))
                image = np.zeros_like(raw)
                film.GetOutputFloat(slc.FilmOutputType.RGB_IMAGEPIPELINE, image, 0, True)
                assert np.isfinite(image).all() and np.max(image) > 0
                journal(f'{mode}/{iteration}: replace and explicitly delete GPU plugins')
                film.DeleteAllImagePipelines()
                session.Parse(pipeline(2.2))
                film.AsyncExecuteImagePipeline(0)
                film.WaitAsyncExecuteImagePipeline()
                after = np.zeros_like(raw)
                film.GetOutputFloat(slc.FilmOutputType.RGB, after, 0, False)
                np.testing.assert_array_equal(raw, after)
                report['records'].append(dict(device=mode, iteration=iteration, finite=True,
                    raw_bit_exact_after_pipeline_replacement=True,
                    async_context_created_on_worker=True, passed=True))
            finally:
                if session.IsStarted():
                    session.Stop()
            # Release FilmImpl's keep_alive owner before releasing the session.
            journal(f'{mode}/{iteration}: destroy session on caller thread')
            del film, session, config, scene
            gc.collect()
            journal(f'{mode}/{iteration}: destroyed')
    if not args.cpu_only:
        assert report['hardware_pipeline_log'], 'GPU pipeline never actually initialized'
        assert any('CUDA' in m or 'METAL' in m or 'OPENCL' in m
                   for m in report['hardware_pipeline_log'])
    if args.require_backend:
        assert all(args.require_backend in m for m in report['hardware_pipeline_log'])
    report['hardware_plugin_fallbacks'] = [m for m in [*messages, *native_log.read_text(errors='replace').splitlines()] if 'hardware plugin failed' in m]
    assert not report['hardware_plugin_fallbacks'], report['hardware_plugin_fallbacks']
    report['passed'] = True
    journal('complete')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
