"""Attribute raw image variance by first-camera-lobe AOV, including covariance.

These AOVs cannot distinguish terminal NEE from a BSDF-sampled light hit, nor
count individual internal scattering events. Do not interpret them as such.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np


def covariance(component, total, mask):
    x = component - component.mean(axis=0)
    y = total - total.mean(axis=0)
    return float((x[:, mask] * y[:, mask]).sum(axis=0).mean() / (len(total)-1))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--regions', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    inputs = []
    for path in sorted(args.evidence.glob('*/result.json')):
        record = json.loads(path.read_text())
        if record.get('aov_names'):
            inputs.append((path.parent, record))
    assert len(inputs) >= 3, 'At least three independent seeds are required'
    first = inputs[0][1]
    keys = ['native_sha256', 'authored_scene_sha256', 'benchmark_source_sha256',
            'spp', 'threads', 'size', 'aov_names']
    fidelity_keys = {'path.regularization.sigma', 'path.regularization.halflife', 'path.filterglossy',
                     'path.clamping.cycles.direct', 'path.clamping.cycles.indirect',
                     'path.clamping.variance.maxvalue'}
    seeds = set()
    for path, record in inputs:
        assert record['comparison_contract'] == 'sss-unregularized-v2' and not record['spectral']
        assert record['engine'] == 'SUPERLUXCORE'
        assert record['raw_controls'] == dict(denoising=False, adaptive_sampling=False)
        assert set(record['native_fidelity']) == fidelity_keys
        assert all(value == 0 for value in record['native_fidelity'].values())
        assert all(record[key] == first[key] for key in keys), path
        assert record['seed'] not in seeds, 'Duplicate seed'
        seeds.add(record['seed'])
        counts = np.load(path/'samples.npy')
        assert np.all(counts == record['spp']), path
    images = np.stack([np.load(path/'linear.npy').astype(np.float64) for path, _ in inputs])
    assert np.isfinite(images).all()
    labels = np.load(args.regions)
    masks = dict(sss=labels > 0, bulk=(labels > 0) & (labels <= 3), thin=labels > 3)
    total_variance = {name: covariance(images, images, mask) for name, mask in masks.items()}
    output = dict(seeds=sorted(seeds), spp=first['spp'], threads=first['threads'],
                  native_sha256=first['native_sha256'], authored_scene_sha256=first['authored_scene_sha256'],
                  regions_sha256=hashlib.sha256(args.regions.read_bytes()).hexdigest(),
                  scope='First camera lobe only; not terminal exit or per-collision SSS events',
                  uncertainty='Three-seed or larger finite-sample diagnostic; not a variance-reduction claim',
                  regions={name: dict(total_interseed_variance=value, components=[])
                           for name, value in total_variance.items()})
    reconstructed = np.zeros_like(images)
    for component in first['aov_names']:
        values = np.stack([np.load(path/('aov-'+component+'.npy')).astype(np.float64)
                           for path, _ in inputs])
        assert values.shape == images.shape and np.isfinite(values).all(), component
        reconstructed += values
        for name, mask in masks.items():
            cov = covariance(values, images, mask)
            total_mean = float(images[:, mask].mean())
            output['regions'][name]['components'].append(dict(
                name=component, mean_rgb=values[:, mask].mean(axis=(0, 1)).tolist(),
                mean_share=float(values[:, mask].mean()/total_mean) if total_mean else None,
                variance=covariance(values, values, mask), covariance_with_total=cov,
                variance_share=cov/total_variance[name] if total_variance[name] else None))
    assert np.allclose(reconstructed, images, rtol=5e-5, atol=1e-5), 'AOV sum does not reconstruct RGB'
    output['reconstruction_max_abs'] = float(np.max(abs(reconstructed-images)))
    for name, row in output['regions'].items():
        row['components'].sort(key=lambda item: item['covariance_with_total'], reverse=True)
        covariance_sum = sum(item['covariance_with_total'] for item in row['components'])
        assert np.isclose(covariance_sum, row['total_interseed_variance'], rtol=1e-4, atol=1e-6)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2))
    for name, row in output['regions'].items():
        print(name, [(item['name'], item['variance_share']) for item in row['components'][:3]])


if __name__ == '__main__':
    main()
