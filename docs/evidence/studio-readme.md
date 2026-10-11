# Comparison render evidence

The previous thirteen-scene photographic showcase was retired on 2026-10-12. Its
JPEG payload is deleted from the public site. The current publication set is the
four newly authored, procedural studies in [`new-delta-manifest.json`](new-delta-manifest.json):
the manifest records `currentSceneCount: 4` and `historicalSceneCount: 13`; the
latter is the retired archive count, not a current homepage scene count.

- **AURORA / serum packaging** — glass transmission, a metal cap and a
  SuperLuxCore diffraction foil against the upstream `metal2` approximation.
- **ION / candy clearcoat EV** — automotive paint, coat response and conductor
  trim against the upstream `glossy2` approximation.
- **KINETIC / knit sneaker** — knit upper, rough rubber and a SuperLuxCore sheen
  response against the upstream `glossy2` upper approximation.
- **LUMEN / smart-glass atrium** — layered glazing and architectural metal with
  SuperLuxCore Sellmeier/thin-film controls against upstream Cauchy glass.

Each study is rendered twice from the same source geometry, camera, lights, seed,
resolution and target samples. The upstream side is the closest supported
LuxCoreRender 2.11.2 approximation when a fork-only material has no equivalent.
The fork side uses SuperLuxCore 2.11.27. The manifest records source-scene hashes,
render hashes, material overrides, observed samples and exact output paths.

## Current publication gate

These are **technical comparison previews**, not final ACES 2.0 beauty plates. Both
sides use `PATHCPU`, seed `42`, target `64 spp`, OIDN and the recorded
`TONEMAP_LINEAR + GAMMA_CORRECTION` display proof. Full frames are 960×630; the
identical comparison crops are 630×630. `qualityGate.marketingReady` is therefore
`false` until a later review produces the final ACES 2.0 beauty pass. The homepage
labels this scope directly.

Run the current validator from the repository root:

```sh
python3 docs/evidence/studio-package.py
```

If Pillow is available, the validator also checks full-frame and crop dimensions.
It always checks the eight published files and their recorded SHA-256 hashes.

## Archived material

`studio-scenes.py`, `studio-render.py`, `studio-assets.py`, the old research brief
and the former review manifest remain only as historical reproduction notes for the
retired showcase. They are not a command for rebuilding the current website and
expect files that are intentionally no longer published. The authoritative current
records are [`new-delta-manifest.json`](new-delta-manifest.json) and
[`provenance.json`](provenance.json).
