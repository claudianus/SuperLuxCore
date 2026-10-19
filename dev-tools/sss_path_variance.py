"""Raw variance attribution by the number of actual volume vertices in LPEs."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from sss_aov_variance import covariance


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--regions', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    inputs = []
    for file in sorted(args.evidence.glob('*/result.json')):
        record = json.loads(file.read_text())
        if record.get('path_audit'):
            inputs.append((file.parent, record))
    assert len(inputs) >= 3, 'At least three independent seeds are required'
    first = inputs[0][1]
    fidelity = {'path.regularization.sigma', 'path.regularization.halflife', 'path.filterglossy',
                'path.clamping.cycles.direct', 'path.clamping.cycles.indirect',
                'path.clamping.variance.maxvalue'}
    same = ['native_sha256', 'authored_scene_sha256', 'benchmark_source_sha256',
            'spp', 'threads', 'size', 'path_audit']
    assert len(set(record['seed'] for _, record in inputs)) == len(inputs)
    for path, record in inputs:
        assert record['comparison_contract'] == 'sss-unregularized-v2' and not record['spectral']
        assert record['raw_controls'] == dict(denoising=False, adaptive_sampling=False)
        assert record['engine'] == 'SUPERLUXCORE'
        assert set(record['native_fidelity']) == fidelity
        assert all(value == 0 for value in record['native_fidelity'].values())
        assert all(record[key] == first[key] for key in same), path
        assert np.all(np.load(path/'samples.npy') == record['spp'])
    images = np.stack([np.load(path/'linear.npy').astype(np.float64) for path, _ in inputs])
    channels = {name: np.stack([np.load(path/('path-'+name+'.npy')).astype(np.float64)
                               for path, _ in inputs]) for name in first['path_audit']}
    assert set(channels) == {'all', 'volume1plus', 'volume2plus', 'volume4plus'}
    assert np.isfinite(images).all() and all(np.isfinite(value).all() for value in channels.values())
    assert np.allclose(channels['all'], images, rtol=5e-5, atol=1e-5)
    ordered = [channels[name] for name in ['all', 'volume1plus', 'volume2plus', 'volume4plus']]
    for larger, smaller in zip(ordered, ordered[1:]):
        assert np.all(smaller-larger <= 5e-5*np.maximum(1., abs(larger))), 'Cumulative LPEs are not nested'
    bins = dict(no_volume=ordered[0]-ordered[1], one_volume=ordered[1]-ordered[2],
                two_to_three_volumes=ordered[2]-ordered[3], four_plus_volumes=ordered[3])
    assert np.allclose(sum(bins.values()), images, rtol=5e-5, atol=1e-5)
    labels = np.load(args.regions)
    masks = dict(sss=labels > 0, bulk=(labels > 0) & (labels <= 3), thin=labels > 3)
    output = dict(seeds=sorted(record['seed'] for _, record in inputs), spp=first['spp'],
                  native_sha256=first['native_sha256'], authored_scene_sha256=first['authored_scene_sha256'],
                  regions_sha256=hashlib.sha256(args.regions.read_bytes()).hexdigest(),
                  path_audit=first['path_audit'], regions={},
                  scope='Actual volume vertex counts anywhere on the complete camera-to-light path; not a terminal-sampler partition',
                  uncertainty='Finite independent-seed image covariance; not a performance or variance-reduction result',
                  reconstruction_max_abs=float(abs(channels['all']-images).max()))
    for name, mask in masks.items():
        variance = covariance(images, images, mask)
        components = []
        for label, value in bins.items():
            cov = covariance(value, images, mask)
            components.append(dict(name=label, mean_rgb=value[:, mask].mean(axis=(0, 1)).tolist(),
                                   mean_share=float(value[:, mask].mean()/images[:, mask].mean()),
                                   variance=covariance(value, value, mask), covariance_with_total=cov,
                                   variance_share=cov/variance if variance else None))
        assert np.isclose(sum(row['covariance_with_total'] for row in components), variance,
                          rtol=1e-4, atol=1e-6)
        output['regions'][name] = dict(total_interseed_variance=variance, components=components)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2))
    for name, row in output['regions'].items():
        print(name, [(item['name'], item['variance_share']) for item in row['components']])


if __name__ == '__main__':
    main()
