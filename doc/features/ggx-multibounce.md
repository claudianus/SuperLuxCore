# metal2 multi-bounce — Heitz'16 height-tracking microsurface walk

Status: **shipped on CPU (`PATHCPU`) and GPU (`PATHOCL`, Metal-validated)** —
opt-in via `scene.materials.X.multibounce = 1` (requires
`distribution = ggx`), exposed in SuperBlendLuxCore as the Metal node's
"Multibounce" checkbox. White-furnace regression in
`dev-tools/e33_glossy2_ggx_parity.py`; comparison scene in
`scenes/metal2mb/metal2-mb.scn` (720p renders `metal2-mb-cpu.png` /
`metal2-mb-gpu.png`).

## What and why

Single-scatter GGX (`D·G2·F / 4|coswo|`) treats the microsurface as if each
photon bounces once and then either escapes or is absorbed — the absorbed
fraction is the energy rough conductors visibly lose (a rough F=1 metal in a
white furnace renders ~0.34 instead of 1.0 at α=1). The `multibounce` option
recovers that energy with the exact Smith microsurface model.

## Algorithm (Heitz et al. 2016, SIGGRAPH)

`GgxMSConductorEval()` (`include/slg/materials/microfacet.h`, GPU twin
`Microfacet_GgxMSConductorEval` in `materialdefs_funcs_microfacet.cl`)
implements the height-tracking random walk from
"Multiple-Scattering Microfacet BSDFs with the Smith Model"
(https://jo.dreggn.org/home/2016_microfacets.pdf), matching NVIDIA
facet-forge semantics (https://github.com/NVLabs/facet-forge):

- Free path in height is exponential: `dh = -log(u)·wr.z/σ(-wr)` with
  microflake cross-section `σ(v) = |v.z|·(1+Λ(v))` ascending, `|v.z|·Λ(v)`
  descending; tracked height `hr` accumulates across bounces.
- Hit microfacet normal drawn from the visible-normal distribution of the
  approach direction (GGX P22 slope-space sampler, valid for both signs of
  `v.z` — ascending legs correctly sample underside-facing normals).
- Each vertex contributes next-event estimation toward the target direction:
  `F·D(wh)·exp(h·Λ(wTarget)) / (4·σ)` — the explicit `exp(h·Λ)` term is the
  transmittance `G1(wo,h)` from depth h, which replaces all
  correlation/segment bookkeeping of position-free estimators.
- Deterministic per-evaluation walks: `Material::Evaluate` carries no RNG, so
  4 walk restarts (`GGX_MS_WALKS`) are driven by a hash of the raw float bits
  of `(wStart, wTarget, hitPoint.p, walk)`. Evaluation stays a fixed function
  of its inputs while different directions/points see independent
  realizations, so error becomes spatial noise rather than frozen bias.
- Returns `f·|cos(wTarget)|` per LuxCore convention; sampling keeps plain
  VNDF because its support covers the full multi-bounce BRDF, so
  `(f_ss+ms)·cos/pdf_ss` stays unbiased.

## Why not Wang et al. 2022 (position-free)

The released `evalPT_new` reference implementation was extracted verbatim and
tested standalone: it overshoots the F=1 white furnace by up to +28% at α=1
(also `evalPT` +9%, `evalBDPT_new` +19%). ORDER analysis showed the bias
comes from bounces ≥4 — the position-free approximation drops the
height correlation between consecutive bounces. The authors themselves
acknowledge this in Wang et al. 2023 (SIGGRAPH Asia, "A Generalized
Microfacet Model..." / invariance-principle paper, arXiv:2302.03408):
"position-free property … much less noise, but also bias" — their Fig. 5
caption states Wang'22 "introduces bias due to the independent-bounce
assumption". Their 2023 correction changes the sampling/segment terms, but
the released code already matches the 2023 Alg.3 form and still biases —
the model, not the port, is the problem. Heitz'16 explicit height tracking
is the provably unbiased alternative (facet-forge reaches exactly ρ=1.000).

## Validation

- Standalone port of the evaluator: white-furnace directional albedo
  `ρ = 1.000 ± 0.002` over α ∈ {0.1..1.5} × view angles, matching
  facet-forge pointwise.
- Engine furnace (`e33` `furnace_metal`, cornell box + sphere, F≈1
  fresnelcolor): `α=0.25` → 0.9217 ss / **0.9989 mb**; `α=1` sphere-only →
  0.34 ss / **0.997 mb**; anisotropic (u=0.15, v=0.8) → 0.858 / **0.999**.
- CPU/GPU parity on the mb furnace: mean 0.9988 vs 0.9968 (rel. err 1.6%).
- `SuperBlendLuxCore/dev-tools/e34_distribution_export_test.py` covers the
  `multibounce` export.

## Pitfalls found (documented for future harnesses)

- `D(wh)` must return 0 for `wh.z <= 0` — without the hemisphere guard the
  ascending legs add spurious energy (overshoot to ρ≈1.3).
- Do NOT seed the deterministic walks from quantized direction components
  (`x*4096+...`): correlated seeds for nearby directions produced systematic
  ±10% integration error. Hash the raw float bits, include `hitPoint.p`, or
  the same (wi,wo) pair reuses the same realization everywhere.
- **Default film pipeline is `AutoLinearToneMap` + gamma 2.2** — it
  normalizes image mean to ~0.5, so a furnace render reads ~0.51 regardless
  of material, light gain, or the feature being tested. Measure reflectance
  through the raw `RGB` output or `film.imagepipelines.0.0.type = NOP`.

## Properties

- `scene.materials.X.multibounce = 0|1` (default 0), metal2 only, only
  consulted when `distribution = ggx`.
- SuperBlendLuxCore: Metal node → "Multibounce" (visible when
  Distribution = GGX).
