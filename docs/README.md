# SuperLuxCore project website

GitHub Pages serves this directory from `main` / `/docs`, without a build step,
trackers, external fonts or a third-party runtime. Public URL:
<https://claudianus.github.io/SuperLuxCore/>.

`content.js` contains paired English/Korean content. `app.js` selects the
browser's primary language and persists an explicit choice when storage is
available. `?lang=ko` / `?lang=en` override that choice. Auto removes the override;
storage failures do not prevent rendering. Comparisons support pointer/touch
and keyboard input; the feature catalog supports bilingual search and filters.

The snapshot is **2026-10-11 / native 2.11.27**. Update version, release URLs,
comparison scope and both translations together. The 48-card catalog links to
source documentation. CPU/GPU coverage, opt-in modes, automatic behavior and
experimental work must be described separately.

## Change history and benefit claims

`evidence/commit-audit.json` inventories **all 509 engine commits** after the
2.11.2 fork baseline (`e9ced7e7a`) through `adba86f43`, by subject and changed
paths. Areas overlap; counts include fixes, reversions and tools. The paired
`fork-analysis.md` / `fork-analysis.ko.md` explain the current marketed benefits
and their source references. This is not a line-by-line audit of all historic
diffs. Upstream remains active; the matrix compares the fork point rather than
claiming coverage of every later upstream change.

## Fresh art and reproducibility

Every render asset was newly authored and rendered on October 11. Old showcase
and diagnostic images were discarded. Eleven unique 1600 × 900 images supply
seven gallery works and three aligned comparisons (metal and diffraction studies are shared
with the comparison section). `evidence/provenance.json` contains hashes and
per-image records. Pair JSON files disclose exact settings and sample counts.

`evidence/artist-scenes.py` generates original PLY geometry and tapered cyHair
strands and renders each official wheel in a separate process. It requires
CPython 3.13, NumPy, Pillow and OpenColorIO 2.6. Use Blender 5.2's OCIO config,
or an equivalent config that supplies `Linear Rec.709` → `sRGB` / `ACES 2.0`.
The config hash is recorded. ACES is applied to linear renderer output after
native OIDN; there is no per-image normalization or generative image.

```sh
python3.13 docs/evidence/artist-scenes.py metals --engine upstream --lib /path/to/upstream-wheel --work /tmp/slc-art --out /tmp/upstream-metal.jpg --spp 768
python3.13 docs/evidence/artist-scenes.py metals --lib /path/to/fork-wheel --work /tmp/slc-art --out /tmp/fork-metal.jpg --spp 768
python3.13 docs/evidence/artist-scenes.py transmission --engine upstream --lib /path/to/upstream-wheel --work /tmp/slc-art --out /tmp/upstream-transmission.jpg --spp 512 --exposure 1
python3.13 docs/evidence/artist-scenes.py transmission --lib /path/to/fork-wheel --work /tmp/slc-art --out /tmp/fork-transmission.jpg --spp 512 --exposure 1
python3.13 docs/evidence/artist-scenes.py spectral --lib /path/to/fork-wheel --work /tmp/slc-art --out /tmp/rgb.jpg --spp 512 --exposure 1
python3.13 docs/evidence/artist-scenes.py spectral --lib /path/to/fork-wheel --work /tmp/slc-art --out /tmp/spectral.jpg --spp 512 --exposure 1 --spectral 1
```

Gallery recipes: `orbs` at 768 SPP, `fur` at 384 SPP, `caustics` at 768 SPP,
`godrays` and `spinner` at 512 SPP; the spectral-disc gallery image is shared
with the spectral comparison; all use `--exposure 1`. `metals` uses exposure 0 on both engines.
Caustics use BIDIRCPU/METROPOLIS; others PATHCPU/SOBOL. Native
OIDN has sharpness 0; auxiliary prefiltering is requested but this recipe does
not supply an ALBEDO AOV. Two bounded workers were used; times are not uncontended
benchmarks. Do not publish speed ratios or universal convergence claims.

The upstream recipe disables optional OpenCL discovery in memory because the
original official 2.11.2 wheel rejects a zero-platform OpenCL configuration on
the tested M5 Pro. Renderer bytes and CPU shading remain unchanged. The
workaround, wheel and native-library fingerprints are disclosed in provenance.

Remove owned render geometry, wheel extractions, cache and preview files once
assets and reproducibility records are preserved. Do not remove unrelated
validation evidence or user processes. Renderer source is Apache-2.0; Blender
extension source is GPL-3.0. The inherited LuxCoreRender foundation is credited.
