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

## Photographic art and reproducibility

All thirteen showcase scenes were rebuilt on October 11 with 32 credited CC0
Poly Haven model, PBR texture and HDRI families. Earlier render assets and
recipes were discarded. The portfolio covers food, wine, electronics, personal
objects, ceramics, textiles, furnished interiors, architecture and optics.

The native renderers produce every pixel. CPU material comparisons use the
same camera, geometry, lighting, sampler, seed, target SPP, exposure and ACES 2.0
view. The official LuxCoreRender 2.11.2 fork baseline is labelled explicitly.
Some independent artwork uses Apple Metal; the per-image manifest identifies
its actual backend. These examples do not imply blanket CPU/GPU parity.

See [`evidence/photoreal-research.md`](evidence/photoreal-research.md) for primary
research sources, [`evidence/studio-readme.md`](evidence/studio-readme.md) for
reproduction, [`evidence/studio-props.json`](evidence/studio-props.json) for CC0
source creators and immutable file receipts, and
[`evidence/provenance.json`](evidence/provenance.json) for completed render
records, image hashes and visual review notes. All thirteen projects expose
actual albedo, normal and undenoised beauty views. The interior also exposes
actual object and material Cryptomatte coverage.

No generated illustration, stock beauty render, painted caustic, variance clamp
or per-image brightness normalisation is used. Native OIDN receives actual
albedo and average shading normal auxiliaries. Technical passes are scene-data
views; beauty and undenoised beauty use OCIO ACES 2.0.

The homepage is an artist-facing portfolio, not a commit inventory. Feature
cards distinguish visual context from actual technical passes. They link to
source documentation for controls and validation limits. Render times in the
manifest are provenance, not uncontended performance measurements.

Remove owned temporary geometry, wheel extractions, caches and rejected previews
once published assets and reproduction records are preserved. Do not remove
unrelated validation evidence or user processes. Renderer source is Apache-2.0;
Blender extension source is GPL-3.0. The inherited LuxCoreRender foundation and
the original CC0 asset authors are credited.
