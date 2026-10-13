# SuperLuxCore project website

GitHub Pages serves this directory from `main` / `/docs` with no build step,
trackers, external fonts or third-party runtime. Public URL:
<https://claudianus.github.io/SuperLuxCore/>.

| File | Contents |
|---|---|
| `index.html` | Page shell, metadata, no-JS fallback |
| `content.js` | Hand-written English/Korean copy, fork statistics, furnace measurements |
| `data.js` | 48-entry feature catalogue, FAQ, coverage notes, commit areas |
| `showcase.js` | **Generated** render receipts the page reads (do not edit) |
| `app.js` | Rendering, language selection, comparison slider, pass viewer, catalogue search |
| `assets/showcase/` | **Generated** WebP images |
| `evidence/showcase-2026-10-12.json` | **Generated** full per-image receipts and published-file hashes |
| `evidence/commit-audit.json`, `fork-*` | Commit inventory and fork analysis |

Language: the browser's primary language picks Korean or English. `?lang=ko` /
`?lang=en` override it, and an explicit choice persists when storage works.

## Showcase images

Scenes, renders and publishing live in
`test-scenes/showcase-2026-10-12/` of the development workspace:

```sh
PY=/Applications/Blender.app/Contents/Resources/5.2/python/bin/python3.13
python3 -I assets.py                                   # CC0 Poly Haven assets, md5-verified, lock.json
Blender -b --factory-startup --python build.py         # scenes/<name>/scene.scn + variants.json + PLY meshes
./finals.sh                                            # every render in out/ with a JSON receipt
$PY publish.py                                         # -> docs/assets/showcase, showcase.js, evidence JSON
```

Each comparison pair shares one `scene.scn` byte for byte. The only
differences are the `variants.json` lines (upstream gets its closest supported
model when it has no equivalent feature) and the render flags in the receipt.
Both sides render through the published Python modules (`pyluxcore` 2.11.2
wheel and `pysuperluxcore` 2.11.27) and display through OpenColorIO ACES 2.0.

Rules for claims on the page:

- State the sample count, seconds, backend and denoiser for every image. The
  receipt card shows them, plus the exact lines that differ.
- Speed is scene- and machine-dependent; the Metal comparison is one scene on
  one Apple M5 Pro.
- The furnace numbers in `content.js` come from a white-metal sphere in a
  constant white environment (256 spp, depth 64). Rerun them before changing them.
- Features that have open correctness issues stay out of the hero comparisons
  until fixed (as of 2026-10-12: GPU spectral variance/bias, OpenPBR SSS
  energy, a CPU/GPU many-lights mismatch).

Renderer source is Apache-2.0; the Blender extension is GPL-3.0. Built on
LuxCoreRender; assets by Poly Haven (CC0).
