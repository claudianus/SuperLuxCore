#!/usr/bin/env python3
"""Verify completed native renders and package honest image derivatives/credits."""
import hashlib
import json
from pathlib import Path
from PIL import Image

docs = Path(__file__).resolve().parents[1]
assets = docs / 'assets/studio'
review=json.loads((docs/'evidence/studio-review.json').read_text())
assert len(review['scenes'])==13 and all(r['status']=='inspected'for r in review['scenes'].values()), 'All thirteen completed beauty images require visual inspection'
for name,r in review['scenes'].items():
    assert hashlib.sha256((assets/(name+'.jpg')).read_bytes()).hexdigest()==r['image_sha256'], 'Review fingerprint changed: '+name
data_source = (docs / 'content.js').read_text()
data = json.loads(data_source[data_source.index('{'):data_source.rindex('}')+1])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
records = {}
for path in sorted(assets.glob('*.json')):
    record = json.loads(path.read_text())
    image = path.with_suffix('.jpg')
    assert record['resolution'] in [[1920,1200],[1600,1000],[1280,800]], path
    assert record['observed_spp'] >= record['target_spp'], path
    if record['scene'] == 'prism':
        assert record['backend'] == 'PATHOCL' and 'path.spectral.enable = 0' in record['configuration'], path
    assert record['raw_finite'] and record['display']['view'] == 'ACES 2.0', path
    assert record['image_sha256'] == sha(image), path
    assert list(Image.open(image).size) == record['resolution'], path
    records[path.stem] = record
assert len(records) == 18, f'Expected 18 completed renders, got {len(records)}'
derivatives = {}
crops = {'music': (620,755,1380,1200), 'audio': (290,160,1150,730),
         'textile': (460,640,1250,1060)}
for name, crop in crops.items():
    for stem in [name, name+'-baseline']:
        source = assets / (stem+'.jpg')
        target = assets / (stem+'-detail.jpg')
        Image.open(source).crop(crop).save(target, quality=97, subsampling=0)
        derivatives[target.name] = {'source': source.name, 'crop': crop,
                                    'operation': 'Identical crop within each pair; no grading or retouching'}
    a, b = records[name], records[name+'-baseline']
    if 'base_scene_sha256' in a:
        assert a['base_scene_sha256'] == b['base_scene_sha256'], name
    for key in ['resolution','target_spp','seed','sampler','backend','display']:
        assert a[key] == b[key], (name, key)

# Remove task outputs with no place in the page. All displayed passes remain.
for path in assets.glob('*.jpg'):
    if path.stem.endswith(('-objects','-materials')) and '-crypto-' not in path.stem:
        path.unlink()
for record in records.values():
    record['technical_outputs'] = [n for n in record['technical_outputs'] if (assets/n).exists()]

entries = []
for image in sorted(assets.glob('*.jpg')):
    stem = image.stem
    native_record = records.get(stem)
    parent = next((n for n in records if stem.startswith(n+'-') and records[n]['variant']=='beauty'), None)
    entry = {'path': 'assets/studio/'+image.name, 'sha256': sha(image),
             'dimensions': list(Image.open(image).size)}
    if native_record:
        entry['kind'] = 'beauty'; entry['record'] = native_record
    elif image.name in derivatives:
        entry['kind'] = 'comparison_closeup'; entry['derivative'] = derivatives[image.name]
    else:
        entry['kind'] = 'raw_beauty' if stem.endswith('-raw') else 'technical_pass'
        entry['source_render'] = parent
        assert parent is not None, image
    entries.append(entry)
manifest = {
    'editorial_snapshot': '2026-10-11', 'native_version': '2.11.27',
    'upstream_comparison_baseline': 'Official LuxCoreRender 2.11.2 fork baseline',
    'runtime_sources': {
        'superluxcore': {'url': 'https://github.com/claudianus/SuperLuxCore/releases/download/wheels-latest/pysuperluxcore-2.11.27-cp313-cp313-macosx_14_0_arm64.whl', 'wheel_sha256': '66e58276d6637bee0a40d8e0e07e27e448e0db36ce25379307520a1d21045b99'},
        'upstream': {'url': 'https://files.pythonhosted.org/packages/f0/8d/298ea5d041cac6ac143f0c1b8d977319308b5dbaf8ef0852439d4418ecf5/pyluxcore-2.11.2-cp313-cp313-macosx_14_0_arm64.whl', 'wheel_sha256': 'c89c0882bda1b70a91b2f0fae51fbd533153b75482360cbf936530d0d232876e'}
    },
    'old_images_discarded': True, 'artificial_image_generation_used': False,
    'project_count': len(data['projects']), 'native_renders': len(records), 'primary_beauty_renders': 13, 'matched_comparison_pairs': 3,
    'research': 'photoreal-research.md', 'quality_review': json.loads((docs/'evidence/studio-review.json').read_text()),
    'hardware': 'Apple M5 Pro (18 CPU cores, 48 GB RAM), native CPU and Apple Metal renders; per-image configuration recorded',
    'recipes': ['studio-scenes.py','studio-render.py','studio-assets.py','studio-package.py'],
    'reproduction': 'studio-readme.md', 'props': json.loads((docs/'evidence/studio-props.json').read_text()),
    'scope': 'Art-directed CPU/Metal artwork and matched CPU material illustrations, not speed benchmarks or blanket GPU/Cycles validation.',
    'display_note': 'Beauty and raw beauty: OCIO ACES 2.0. Technical normal, albedo and Cryptomatte images: actual scene data views.',
    'assets': entries,
    'recipe_sha256': {name: sha(docs/'evidence'/name) for name in ['studio-scenes.py','studio-render.py','studio-assets.py','studio-package.py']}
}
(docs/'evidence/provenance.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
for path in assets.glob('*.json'):
    path.unlink()  # Consolidated into the public manifest above.
print('Verified', len(records), 'native renders;', len(entries), 'published images')
