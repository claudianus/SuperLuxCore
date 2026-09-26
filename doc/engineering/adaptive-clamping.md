# Adaptive Robust Clamping

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Adaptive Robust Clamping (path.clamping.variance.*)

Firefly suppression in `VarianceClamping` (`src/slg/utils/varianceclamping.cpp`,
GPU twin `include/slg/utils/varianceclamping_funcs.cl` — keep them in
sync). Three orthogonal mechanisms on top of the user `maxvalue`:

- `path.clamping.variance.adaptive` (default 1): the margin is estimated
  from robust statistics of the 3x3 neighborhood of pixel means, read
  straight from the film channel buffer (no extra buffers). Bound per
  pixel: `T = max(ownMean, med) + max(sigma*mad, sqrtMax*(0.1+E))`.
  Spatially coherent bright content (sun glints, caustic patches) has a
  high neighborhood median/MAD so the bound relaxes; isolated fireflies
  sit in dark neighborhoods with tiny MAD and get clamped hard. Median/
  MAD is the online counterpart of DeCoro et al. PG'10 density-outlier
  rejection (breaks only at >50% contamination). <3 valid neighbors →
  legacy `[0, sqrtMax]` virgin bound. `adaptive=0` keeps the legacy
  fixed margin around the own-pixel mean.
- `path.clamping.variance.scope` = `all|indirect|direct` (default
  `indirect`, Cycles-style direct/indirect split). Scope enums:
  `CLAMP_ALL=0, CLAMP_INDIRECT=1, CLAMP_DIRECT=2` — the ints must match
  the CL kernel params. Under `indirect`, emission + first-vertex direct
  components are untouched and the beauty loses exactly the removed
  indirect share (beauty/AOV consistency). `SampleResult::AddEmission`/
  `AddDirectLight` fill the component fields regardless of AOV channel
  declaration, so the decomposition is always valid. Light-traced
  PER_SCREEN splats count as indirect (skipped under `direct` scope).
- `path.clamping.variance.sigma` (default 6): MAD multiplier; 6*MAD ~ 4σ
  for Gaussian neighborhoods.

Multi-group caveat: `directish` lives in group 0; `indirectY` is
computed over all groups, so with >1 radiance groups the threshold is
slightly permissive for group 0 (bookkeeping stays consistent via the
proportional `sB` scale on extra groups).

Regression: `dev-tools/e42_adaptive_clamp_test.py` (firefly suppression,
energy preservation, scope symmetry, legacy mode, CPU/GPU parity).
720p Blender-path visual: `SuperBlendLuxCore/dev-tools/clamp_visual_test.py`.

References: DeCoro et al., "A Memory Efficient Method for Variance
Estimation in Path Tracing" (PG 2010); Cycles direct/indirect clamp
split; Buisine et al. adaptive median-of-means; Zirr & Kaplanyan,
"Re-Weighting Firefly Samples" (CGF 2018).

Gotcha fixed along the way: upstream AOV clamp fallbacks read the
`*_REFLECT` channel as the expected value for `*_TRANSMIT` components
and the `else if` fallback repeated the same (dead) condition — CPU
now reads the proper TRANSMIT channel + aggregate fallback, matching
the GPU twin.

