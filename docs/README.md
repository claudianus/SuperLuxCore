# SuperLuxCore project website

GitHub Pages serves this directory from `main` / `/docs`, without a build step,
trackers, external fonts or a third-party runtime. Public URL:
<https://claudianus.github.io/SuperLuxCore/>.

`content.js` contains paired English/Korean content. `app.js` selects the
browser's primary language and persists an explicit choice when storage is
available. `?lang=ko` / `?lang=en` override that choice. Auto removes the override;
storage failures do not prevent rendering. Comparisons support pointer/touch
and keyboard input; the feature catalog supports bilingual search and filters.

The snapshot is **2026-10-12 / native 2.11.27**. Update version, release URLs,
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

## Fork comparison art and reproducibility

The retired thirteen-scene showcase is no longer part of the published asset
set. The homepage now exposes four newly authored, procedural product and
architecture studies: optical packaging, an EV clearcoat study, knit footwear
materials and a smart-glass atrium. Each pair uses the same scene source,
camera, light, seed and CPU render settings for LuxCoreRender 2.11.2 and
SuperLuxCore 2.11.27. The upstream side is the closest supported approximation
when a fork material has no upstream equivalent.

These eight images are **technical comparison previews**, not final ACES 2.0
beauty plates: they use OIDN and the recorded Linear+Gamma display proof. The
manifest records output dimensions, observed samples, scene/image hashes,
material overrides and the exact scope in [`evidence/provenance.json`](evidence/provenance.json)
and [`evidence/new-delta-manifest.json`](evidence/new-delta-manifest.json).
Both manifests make the inventory split explicit: `currentSceneCount` is 4 and
`historicalSceneCount` is 13. The latter is the retired archive count, not a
claim that thirteen scenes remain published on the homepage.

The historical scene generator, research brief and package script remain in
`evidence/` as archive material and expect the retired files; they are not the
validation command for the current comparison set. Do not infer blanket CPU/GPU
parity, production beauty quality or a universal speedup from these studies.

The homepage is an artist-facing portfolio, not a commit inventory. Feature
cards distinguish visual context from actual technical passes. They link to
source documentation for controls and validation limits. Render times in the
manifest are provenance, not uncontended performance measurements.

Remove owned temporary geometry, wheel extractions, caches and rejected previews
once published assets and reproduction records are preserved. Do not remove
unrelated validation evidence or user processes. Renderer source is Apache-2.0;
Blender extension source is GPL-3.0. The inherited LuxCoreRender foundation and
the original CC0 asset authors are credited.
