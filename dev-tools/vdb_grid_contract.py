"""Probe authored VDB voxel centers, reconstruction edges and numeric types.

Uses a temporary C++ bridge calling the loaded native module's scene parser and
texture evaluator. Fixture voxels and the independent linear oracle live in
vdb_grid_contract.cpp. No Blender process or render is started. Compilation and
the evaluation child are bounded and reaped; temporary bridge and fixture data
are removed on success and failure. Run with the module's Python ABI.
"""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import os
import shlex
import signal
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]


def cases(domain_min, domain_max, extra_properties, index_mode=False, padding=2):
    dimensions = [int(b - a) for a, b in zip(domain_min, domain_max)]
    assert all(n > 0 for n in dimensions)
    base = '\n'.join([
        'scene.textures.test.type = densitygrid',
        'scene.textures.test.storage = float',
        'scene.textures.test.wrap = black',
        'scene.textures.test.mapping.type = globalmapping3d',
        *(f'scene.textures.test.n{axis} = {size}'
          for axis, size in zip('xyz', dimensions)),
        extra_properties,
    ])
    probes = [
        ('inclusive-last-voxel', 'float-last-plane', [[7, 3, 3], [6.75, 3, 3]]),
        ('linear-sparse-edge', 'float-isolated',
         [[0, 0, 0], [.25, 0, 0], [-.25, 0, 0], [.25, .25, .25],
          [1.25, 0, 0], [-1.25, 0, 0], [1.75, 0, 0], [-1.75, 0, 0]]),
        ('nonzero-background', 'float-background', [[-.25, 0, 0], [1, 0, 0]]),
        ('negative-inclusive-last-voxel', 'float-negative', [[-1, -4, -4], [-1.25, -4, -4]]),
        ('active-tile-without-leaves', 'float-tile-only', [[3, 3, 3], [7, 3, 3], [7.25, 3, 3]]),
    ]
    probes += [(f'numeric-{name}', name, [[3, 3, 3], [6.75, 3, 3], [7.25, 3, 3]])
               for name in ['float', 'double', 'int32', 'int64', 'bool', 'vec3f', 'vec3d', 'vec3i', 'vec4f']]
    probes += [(f'alpha-{name}', name, [[3, 3, 3], [7.25, 3, 3], [-1.75, 0, 0]])
               for name in ['float', 'int64', 'bool', 'vec3i', 'vec3d', 'vec4f']]
    result = []
    for interpolation in (['linear', 'cubic'] if index_mode else ['linear']):
        for name, grid, points in probes:
            lo = list(domain_min)
            hi = list(domain_max)
            properties = base
            read_padding = max(padding, 3) if interpolation == 'cubic' else padding
            if index_mode:
                extent = 1 if grid in ['float-isolated', 'float-background'] else 8
                lo = [-read_padding] * 3
                hi = [extent + read_padding] * 3
                dimensions = [b - a for a, b in zip(lo, hi)]
                properties = '\n'.join([
                    'scene.textures.test.type = densitygrid',
                    'scene.textures.test.storage = float',
                    'scene.textures.test.wrap = black',
                    'scene.textures.test.mapping.type = globalmapping3d',
                    'scene.textures.test.openvdb.mode = index',
                    f'scene.textures.test.openvdb.padding = {read_padding}',
                    f'scene.textures.test.interpolation = {interpolation}',
                    *(f'scene.textures.test.n{axis} = {size}'
                      for axis, size in zip('xyz', dimensions)), extra_properties])
            if grid == 'float-negative':
                lo = [n - 8 for n in lo]
                hi = [n - 8 for n in hi]
            component = 'alpha' if name.startswith('alpha-') else 'rgb'
            properties += f'\nscene.textures.test.openvdb.component = {component}'
            extent = 1 if grid in ['float-isolated', 'float-background'] else 8
            active_lo = [-8] * 3 if grid == 'float-negative' else [0] * 3
            carrier_padding = 2 if interpolation == 'cubic' else 1
            carrier_lo = (active_lo[0] - carrier_padding - lo[0]) / (hi[0] - lo[0])
            carrier_hi = (active_lo[0] + extent - 1 + carrier_padding - lo[0]) / (hi[0] - lo[0])
            result.append({'name': name + '-' + interpolation, 'grid': grid,
                           'properties': properties, 'interpolation': interpolation, 'component': component,
                           'expected_active_bounds': active_lo + [n + extent - 1 for n in active_lo],
                           'domain_min': lo, 'domain_extent': [b - a for a, b in zip(lo, hi)],
                           'carrier_normalized': [carrier_lo, carrier_hi],
                           'samples': [{'index': p, 'normalized': [(p[a] - lo[a]) / (hi[a] - lo[a])
                                        for a in range(3)]} for p in points]})
    if index_mode:
        for interpolation in ['linear', 'cubic']:
            properties = '\n'.join([
                'scene.textures.test.type = densitygrid', 'scene.textures.test.storage = float',
                'scene.textures.test.wrap = white', 'scene.textures.test.mapping.type = globalmapping3d',
                f'scene.textures.test.interpolation = {interpolation}',
                *(f'scene.textures.test.n{a} = 2' for a in 'xyz'),
                'scene.textures.test.data = ' + ' '.join(['3'] * 8)])
            points = [[.99, .5, .5], [1.01, .5, .5], [-.01, .5, .5]]
            result.append({'name': 'white-wrap-high-minorant-' + interpolation,
                           'grid': 'float', 'properties': properties, 'interpolation': interpolation,
                           'expected_active_bounds': [0, 0, 0, 7, 7, 7], 'wrap': 'white',
                           'domain_min': [0] * 3, 'domain_extent': [2] * 3,
                           'carrier_normalized': [0, 1], 'override_constant': 3,
                           'samples': [{'normalized': p, 'index': [2 * x for x in p]} for p in points]})
        for wrap, points in [
            ('clamp', [[-5.25, -8.625, -3.75], [8.5, 11.25, 7.5], [-.0625, .03125, .0625]]),
            ('repeat', [[-.125, .5, .5], [.875, .5, .5], [1.875, .5, .5]]),
        ]:
            properties = '\n'.join([
                'scene.textures.test.type = densitygrid', 'scene.textures.test.storage = float',
                f'scene.textures.test.wrap = {wrap}', 'scene.textures.test.mapping.type = globalmapping3d',
                'scene.textures.test.interpolation = cubic', *(f'scene.textures.test.n{a} = 2' for a in 'xyz'),
                'scene.textures.test.data = 1 2 3 4 5 6 7 8'])
            result.append({'name': 'cubic-' + wrap + '-finite-data', 'grid': 'float',
                           'properties': properties, 'interpolation': 'cubic', 'wrap': wrap,
                           'expected_active_bounds': [0, 0, 0, 7, 7, 7],
                           'domain_min': [0] * 3, 'domain_extent': [2] * 3,
                           'carrier_normalized': [0, 1], 'override_data': list(range(1, 9)),
                           'samples': [{'normalized': p, 'index': [2 * x for x in p]} for p in points]})
    return result


def bounded(argv, *, timeout, cwd=None, log=None, env=None):
    child = subprocess.Popen(argv, cwd=cwd, env=env, stdout=log,
                             stderr=subprocess.STDOUT, start_new_session=True)
    try:
        return child.wait(timeout=timeout)
    except BaseException:
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait(timeout=5)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, default=REPO / 'out/build')
    parser.add_argument('--request-json', type=Path,
                        help='Optional explicit case properties/normalized sample coordinates.')
    parser.add_argument('--extra-properties', type=Path)
    parser.add_argument('--domain-min', type=float, nargs=3, default=[0, 0, 0])
    parser.add_argument('--domain-max-exclusive', type=float, nargs=3, default=[8, 8, 8])
    parser.add_argument('--index-mode', action='store_true',
                        help='Probe active-inclusive extraction with explicit index API.')
    parser.add_argument('--padding', type=int, default=2)
    parser.add_argument('--fixture-output', type=Path,
                        help='Retain one small authored fixture for later Blender validation.')
    parser.add_argument('--rgba-postfilter-output', type=Path,
                        help='Also author the separate piecewise RGBA Blender fixture.')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--fixture-worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--bridge', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    assert sys.platform == 'darwin', 'This temporary bridge uses the macOS dynamic linker.'
    if args.worker or args.fixture_worker:
        sys.path.insert(0, str(args.module_dir))
        import pysuperluxcore as lux
        native_path = Path(getattr(lux, 'pysuperluxcore', lux).__file__)
        native = ctypes.CDLL(str(native_path), mode=ctypes.RTLD_GLOBAL)
        lux.Init()
        bridge = ctypes.CDLL(str(args.bridge))
        if args.fixture_worker:
            function = (bridge.slc_vdb_make_rgba_postfilter_fixture if args.rgba_postfilter_output
                        else bridge.slc_vdb_make_grid_fixture)
            path = args.rgba_postfilter_output or args.fixture_output
            function.argtypes = [ctypes.c_char_p]
            function.restype = ctypes.c_int
            return 0 if function(os.fsencode(path)) == 1 else 1
        bridge.slc_vdb_grid_contract.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        bridge.slc_vdb_grid_contract.restype = ctypes.c_int
        passed = bridge.slc_vdb_grid_contract(os.fsencode(args.request_json),
                                              os.fsencode(args.output)) == 1
        proof = json.loads(args.output.read_text())
        request = json.loads(args.request_json.read_text())
        if request.get('metadata_active_bounds'):
            metadata = []
            for record in proof['records']:
                if any(x['grid'] == record['grid'] for x in metadata):
                    continue
                expected = next(x['expected_active_bounds'] for x in request['cases']
                                if x['grid'] == record['grid'])
                item = {'grid': record['grid'], 'expected_active_inclusive_bounds': expected}
                try:
                    info = lux.GetOpenVDBGridInfo(request['fixture'], record['grid'], True)
                    item['actual_active_inclusive_bounds'] = list(info[1])
                    item['passed'] = item['actual_active_inclusive_bounds'] == expected
                except Exception as error:
                    item['passed'] = False
                    item['error'] = str(error)
                metadata.append(item)
            proof['active_bounds_metadata'] = metadata
            passed = passed and all(x['passed'] for x in metadata)
            proof['passed'] = passed
        proof['native_sha256'] = hashlib.sha256(native_path.read_bytes()).hexdigest()
        proof['native_path'] = str(native_path)
        args.output.write_text(json.dumps(proof, indent=2) + '\n')
        return 0 if passed else 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    source = REPO / 'dev-tools/vdb_grid_contract.cpp'
    commands = json.loads((args.build_dir / 'compile_commands.json').read_text())
    row = next(x for x in commands if x['file'].endswith('/textures/blackbody.cpp')
               and '-DCMAKE_INTDIR="Release"' in shlex.split(x['command']))
    original = shlex.split(row['command'])
    flags, i = [], 1
    while i < len(original):
        if original[i] in ['-o', '-c', '-MT', '-MF']:
            i += 2
        elif original[i] in ['-MD', '-MMD', '-O3']:
            i += 1
        else:
            flags.append(original[i]); i += 1
    # The renderer under test remains its optimized build. Only this independent
    # oracle/fixture bridge uses O1 to bound compiler time and memory, retaining
    # the __OPTIMIZE__ macro required by the existing Release precompiled header.
    flags.append('-O1')
    proof = {'passed': False, 'complete': False}
    exit_code = 1
    with tempfile.TemporaryDirectory(prefix='slc-vdb-grid-contract-') as temporary:
        tmp = Path(temporary)
        bridge = tmp / 'contract.dylib'
        fixture = args.fixture_output.resolve() if args.fixture_output else tmp / 'numeric-edges.vdb'
        fixture.parent.mkdir(parents=True, exist_ok=True)
        fixture_preexisted = fixture.exists()
        fixture_authored = fixture_preexisted
        rgba_preexisted = not args.rgba_postfilter_output or args.rgba_postfilter_output.exists()
        rgba_authored = rgba_preexisted
        if args.request_json:
            request = json.loads(args.request_json.read_text())
        else:
            extra = args.extra_properties.read_text() if args.extra_properties else ''
            request = {'cases': cases(args.domain_min, args.domain_max_exclusive, extra,
                                      args.index_mode, args.padding),
                       'metadata_active_bounds': args.index_mode}
        request['fixture'] = str(fixture)
        request_path = tmp / 'request.json'
        request_path.write_text(json.dumps(request, indent=2) + '\n')
        # A crashed child must never inherit a successful previous JSON proof.
        args.output.unlink(missing_ok=True)
        try:
            with args.output.with_suffix('.build.log').open('w') as log:
                code = bounded([original[0], *flags, '-dynamiclib', '-undefined', 'dynamic_lookup',
                                str(source), '-o', str(bridge)], timeout=900,
                               cwd=row['directory'], log=log)
                if code:
                    raise RuntimeError(f'bridge compilation exited {code}')
            with args.output.with_suffix('.runtime.log').open('w') as log:
                # Native evaluation runs in a fresh process. The custom Vec4f
                # registration used for authoring must not leak into it.
                if not fixture.exists():
                    generator_exit = bounded([sys.executable, str(Path(__file__).resolve()),
                                              '--fixture-worker', '--bridge', str(bridge),
                                              '--module-dir', str(args.module_dir),
                                              '--fixture-output', str(fixture), '--output', str(args.output)],
                                             timeout=120, log=log)
                    if generator_exit:
                        raise RuntimeError(f'fixture authoring exited {generator_exit}')
                    fixture_authored = True
                if args.rgba_postfilter_output and not rgba_preexisted:
                    args.rgba_postfilter_output.parent.mkdir(parents=True, exist_ok=True)
                    generator_exit = bounded([sys.executable, str(Path(__file__).resolve()),
                                              '--fixture-worker', '--bridge', str(bridge),
                                              '--module-dir', str(args.module_dir),
                                              '--rgba-postfilter-output', str(args.rgba_postfilter_output),
                                              '--output', str(args.output)], timeout=120, log=log)
                    if generator_exit:
                        raise RuntimeError(f'RGBA fixture authoring exited {generator_exit}')
                    rgba_authored = True
                exit_code = bounded([sys.executable, str(Path(__file__).resolve()), '--worker',
                                     '--bridge', str(bridge), '--module-dir', str(args.module_dir),
                                     '--request-json', str(request_path), '--output', str(args.output)],
                                    timeout=240, log=log)
            proof = json.loads(args.output.read_text())
            proof['worker_exit_code'] = exit_code
            proof['worker_reaped'] = True
        except BaseException as error:
            if not fixture_preexisted and not fixture_authored:
                fixture.unlink(missing_ok=True)
            if not rgba_preexisted and not rgba_authored:
                args.rgba_postfilter_output.unlink(missing_ok=True)
            proof = {'passed': False, 'complete': False, 'error': str(error), 'worker_reaped': True}
        proof['request'] = request
        proof['source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
        proof['fixture_retained'] = bool(args.fixture_output)
        proof['fixture_generation_separate_process'] = True
        proof['vec4_registration_not_supplied_by_oracle'] = True
        if args.rgba_postfilter_output and rgba_authored:
            proof['rgba_postfilter_fixture'] = {
                'path': str(args.rgba_postfilter_output.resolve()),
                'sha256': hashlib.sha256(args.rgba_postfilter_output.read_bytes()).hexdigest(),
                'bytes': args.rgba_postfilter_output.stat().st_size,
                'separately_authored_this_run': not rgba_preexisted}
    proof['temporary_bridge_and_fixture_removed'] = not Path(temporary).exists()
    args.output.write_text(json.dumps(proof, indent=2) + '\n')
    print('VDB_GRID_CONTRACT', json.dumps({k: proof.get(k) for k in
          ['complete', 'passed', 'native_sha256', 'worker_exit_code',
           'temporary_bridge_and_fixture_removed']}), flush=True)
    return exit_code if proof.get('complete') else 2


if __name__ == '__main__':
    sys.exit(main())
