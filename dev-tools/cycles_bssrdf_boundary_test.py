# SPDX-License-Identifier: Apache-2.0
"""Directional boundary measures, independent solid-angle quadrature and Metal.

Usage: python cycles_bssrdf_boundary_test.py PROBE OUTPUT_DIR SHARED_CL
Requires numpy. See cycles_bssrdf_boundary_probe.mm for build instructions.
This is a prerequisite for adjoint transport, not production LT/MIS acceptance.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from functools import lru_cache

import numpy as np

probe, folder, shared = map(Path, sys.argv[1:])
folder.mkdir(parents=True, exist_ok=True)
N = 512
u, v = np.meshgrid((np.arange(N) + .5) / N, (np.arange(N) + .5) / N)
uv = np.column_stack((u.ravel(), v.ravel()))
cases, chunks = [], []
for ior, alpha, mu in ((1.1, .4, .15), (1.1, .8, .6), (1.4, .4, .15),
                       (1.4, .4, .6), (1.4, .4, 1.), (1.4, .8, .6),
                       (3.8, .4, .15), (3.8, .8, .6), (3.8, .8, 1.)):
    for mode in (0, 1):
        rows = np.zeros((N * N, 8), dtype=np.float32)
        rows[:, :3] = (np.sqrt(1 - mu * mu), 0., mu if mode == 0 else -mu)
        rows[:, 3:5] = (ior, alpha)
        rows[:, 5:7] = uv
        rows[:, 7] = mode
        cases.append(dict(ior=ior, alpha=alpha, mu=mu, mode=mode,
                          start=sum(len(c) for c in chunks), count=len(rows)))
        chunks.append(rows)
edge_start = sum(len(c) for c in chunks)
edges = []
for ior in (1.01, 1.4, 3.8):
    for alpha in (0., .0001, .01, 1.):
        for mu in (.0001, .5, 1.):
            for mode in (0, 1):
                edges.append((np.sqrt(1 - mu * mu), 0., mu if not mode else -mu,
                              ior, alpha, .37, .79, mode))
chunks.append(np.asarray(edges, dtype=np.float32))
inputs = np.concatenate(chunks)
inputs.tofile(folder / 'input.f32')
run = subprocess.run([str(probe), str(folder / 'input.f32'), str(folder / 'cpu.f32'),
                      str(folder / 'metal.f32'), str(shared)], capture_output=True, text=True)
(folder / 'probe.stdout').write_text(run.stdout)
(folder / 'probe.stderr').write_text(run.stderr)
assert run.returncode == 0, (run.returncode, run.stderr)
identity = json.loads(run.stdout)
assert identity['metal_completed'] and identity['legacy_entry_max_direction_error'] == 0.
cpu, metal = [np.fromfile(folder / (name + '.f32'), dtype=np.float32).reshape(-1, 12)
              for name in ('cpu', 'metal')]
assert cpu.shape == metal.shape == (len(inputs), 12)
assert np.isfinite(cpu).all() and np.isfinite(metal).all()


def independent_pdfs(o, d, ior, alpha):
    """Float64 Walter refractive Jacobian times Heitz visible-normal density."""
    h = o + ior * d
    h = h / np.linalg.norm(h, axis=-1, keepdims=True)
    h = np.where(h[..., 2:3] < 0., -h, h)
    co, ci = np.sum(o * h, axis=-1), np.sum(d * h, axis=-1)
    # Derive D from tan(theta), rather than the implementation's stable form.
    hz2 = h[..., 2] ** 2
    with np.errstate(divide='ignore', invalid='ignore'):
        D = 1. / (np.pi * alpha ** 2 * hz2 ** 2 *
                  (1. + (1. - hz2) / (alpha ** 2 * hz2)) ** 2)
    def masking(w):
        return 2. / (1. + np.sqrt(1. + alpha ** 2 *
                                  (w[..., 0] ** 2 + w[..., 1] ** 2) / w[..., 2] ** 2))
    denominator2 = (co + ior * ci) ** 2
    support = (co > 0.) & (ci < 0.) & (o[..., 2] > 0.) & (d[..., 2] < 0.)
    pf = D * masking(o) * co / o[..., 2] * ior ** 2 * (-ci) / denominator2
    pr = D * masking(d) * (-ci) / (-d[..., 2]) * co / denominator2
    return np.where(support, pf, 0.), np.where(support, pr, 0.)


@lru_cache(None)
def quadrature_nodes(order):
    x, weights = np.polynomial.legendre.leggauss(order)
    return (x + 1.) / 2., weights / 2.


def quadrature(case, order):
    # Integrate in solid angle dmu dphi on the target macro-hemisphere.
    mu, weights = quadrature_nodes(order)
    phi = (np.arange(2 * order) + .5) * np.pi / order
    result = np.zeros(4)
    fixed = np.array((np.sqrt(1. - case['mu'] ** 2), 0.,
                      case['mu'] if not case['mode'] else -case['mu']))
    for start in range(0, order, 32):
        z = mu[start:start + 32, None]
        r = np.sqrt(1. - z ** 2)
        target = np.stack(np.broadcast_arrays(r * np.cos(phi), r * np.sin(phi),
                                             z if case['mode'] else -z), axis=-1)
        o, d = (target, fixed) if case['mode'] else (fixed, target)
        pf, pr = independent_pdfs(o, d, case['ior'], case['alpha'])
        pdf = pr if case['mode'] else pf
        measure = weights[start:start + 32, None] * np.pi / order
        transpose = pf * o[..., 2] / -d[..., 2]
        result += [np.sum(pdf * measure), np.sum(pdf * target[..., 2] * measure),
                   np.sum(pdf * target[..., 0] * measure), np.sum(transpose * measure)]
    return result


records = []
for case in cases:
    ix = slice(case['start'], case['start'] + case['count'])
    data, gpu, inp = cpu[ix], metal[ix], inputs[ix].astype(np.float64)
    valid = data[:, 10] > .5
    gpu_valid = gpu[:, 10] > .5
    mismatch = float(np.mean(valid != gpu_valid))
    assert mismatch < .0001, (case, mismatch)
    common = valid & gpu_valid
    direction_error = np.max(np.abs(data[common, :3] - gpu[common, :3]))
    # Near the critical angle sqrt(transmission) amplifies float32 math
    # differences by ior/cos(theta_out). Check that conditioned error as
    # well as an absolute bound; do not treat every angle as well conditioned.
    direction_errors = np.max(np.abs(data[common, :3] - gpu[common, :3]), axis=1)
    conditioned_error = 0.
    if case['mode']:
        o, d = data[common, :3], inp[common, :3]
        h = o + case['ior'] * d
        h /= np.linalg.norm(h, axis=1, keepdims=True)
        co = np.abs(np.sum(o * h, axis=1))
        conditioned_error = float(np.max(direction_errors * co / case['ior']))
        assert direction_error < 1e-4 and conditioned_error < 16 * np.finfo(np.float32).eps, (case, direction_error, conditioned_error)
    else:
        assert direction_error < 2e-5, (case, direction_error)
    relative = np.abs(data[common, 3:6] - gpu[common, 3:6]) / np.maximum(1e-8, np.abs(data[common, 3:6]))
    assert np.quantile(relative, .999) < .002, (case, np.quantile(relative, .999))
    target = data[:, :3].astype(np.float64)
    target /= np.maximum(1e-30, np.linalg.norm(target, axis=-1, keepdims=True))
    outside, inside = (inp[:, :3], target) if not case['mode'] else (target, inp[:, :3])
    pf, pr = independent_pdfs(outside[valid], inside[valid], case['ior'], case['alpha'])
    reference = np.column_stack((pf, pr))
    evaluation_error = np.abs(data[valid, 3:5] - reference) / np.maximum(1e-8, reference)
    assert np.quantile(evaluation_error, .999) < .002, (case, np.quantile(evaluation_error, .999))
    # Check adjoint kernel / proposal identity, including the cosine measure.
    weight = pf * outside[valid, 2] / (-inside[valid, 2] * pr)
    assert np.quantile(np.abs(weight - data[valid, 5]) / weight, .999) < .002
    coarse = quadrature(case, 384)
    for order in (768, 1536, 3072, 6144):
        fine = quadrature(case, order)
        quadrature_error = float(np.max(np.abs(coarse - fine)))
        if quadrature_error < .0005 and (case['mode'] or abs(fine[0] - 1.) < .0005):
            break
        coarse = fine
    assert quadrature_error < .0005, (case, coarse, fine)
    sample_moments = np.array((valid.mean(), np.where(valid, target[:, 2], 0.).mean(),
                               np.where(valid, target[:, 0], 0.).mean(),
                               np.where(valid, data[:, 5], 0.).mean()))
    if not case['mode']:
        assert abs(fine[0] - 1.) < .0005, (case, fine)
        errors = np.abs(sample_moments[:3] - fine[:3])
    else:
        errors = np.abs(sample_moments - fine)
    assert np.max(errors / np.maximum(1., np.abs(fine[:len(errors)]))) < .006, (case, sample_moments, fine)
    record = dict(case, quadrature=fine.tolist(), sample_moments=sample_moments.tolist(),
                  quadrature_refinement_error=quadrature_error,
                  quadrature_order=order,
                  metal_direction_max_error=float(direction_error), metal_support_mismatch=mismatch,
                  metal_conditioned_direction_max_error=conditioned_error,
                  metal_direction_p999=float(np.quantile(direction_errors, .999)),
                  metal_pdf_weight_relative_p999=float(np.quantile(relative, .999)), passed=True)
    records.append(record)
    print('BOUNDARY_PASS', case, flush=True)

edge_data, edge_inputs = cpu[edge_start:], inputs[edge_start:]
for p, sample in zip(edge_inputs, edge_data):
    if p[4] == 0.:
        assert sample[3] == sample[4] == 0. and sample[8] == 1.
        if p[7] == 0.:
            assert sample[10] == 1. and sample[6] == sample[7] == 1.
        elif 1. - p[3] ** 2 * (1. - p[2] ** 2) < 0.:
            assert sample[10] == 0. and sample[6] == sample[7] == 0.
        else:
            assert sample[10] == 1. and sample[6] == sample[7] == 1.
report = dict(identity, shared_source_sha256=hashlib.sha256(shared.read_bytes()).hexdigest(),
              production_acceptance=False, spatial_pdf_or_mis_implemented=False,
              records=records, finite_edge_cases=len(edges), passed=True)
(folder / 'boundary-metrics.json').write_text(json.dumps(report, indent=2) + '\n')
print('BOUNDARY_COMPLETE', len(records), len(edges), flush=True)
