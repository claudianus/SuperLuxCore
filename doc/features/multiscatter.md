# Multi-scattering compensation — Turquin / Kulla-Conty for roughglass, carpaint, disney

Status: **shipped on CPU (`PATHCPU`) and GPU (`PATHOCL`, Metal-validated)** —
opt-in per material via `scene.materials.X.multibounce = 1` (default 0,
legacy behaviour preserved). Regression: `dev-tools/e50_multibounce_parity.py`.

## What and why

Single-scatter microfacet BRDFs lose the energy that real microsurfaces
re-emit after inter-facet bounces — rough lobes visibly darken at grazing
angles. `multibounce` on metal2 already recovers this exactly for conductors
via the Heitz'16 random walk (see [ggx-multibounce.md](ggx-multibounce.md)).
This option extends compensation to the remaining glossy lobes with the
closed-form directional-albedo model:

- **GGX lobes** — Turquin 2019 ("Practical multiple scattering compensation
  for microfacet models", EGSR,
  https://blogs.unity3d.com/2019/08/22/...) / Kulla-Conty 2017
  ("Revisiting Physically Based Shading at Imageworks",
  https://blog.yiningkarlli.com/...): the single-scatter BRDF is multiplied
  by `1 + Favg·(1−Ess(μ)) / Ess(μ)`, where `Ess(μ)` is the fitted GGX
  directional albedo and `Favg` the hemispherical Fresnel average.
  Implemented in `GgxDirAlbedo` / `GgxFresnelAverage` / `GgxMSCompensation`
  (`include/slg/materials/microfacet.h`, GPU twins
  `Microfacet_*` in `materialdefs_funcs_microfacet.cl`).
- **Schlick lobes** — the coating-style `(1−G)` first-order term already
  used for glossy2: `f_ms = coso·clamp((1−G)/(4·coso·cosi), 0, 1)`, added to
  the single-scatter lobe. Conservative approximation of the same physics,
  correct sign, energy-bounded by clamping.

The compensation is deterministic (no extra sampling noise), cheap (a few
fitted rational functions), and applied symmetrically in `Evaluate()` and
`Sample()` so PDFs stay the unchanged single-scatter VNDF PDF — the
compensated BRDF integrates to more than the sampled pdf, which is exactly
how glossy2/metal2 handle it (importance sampling stays valid; throughput
gains remain unbiased because `f·cosθ/pdf` is evaluated consistently).

## Materials

| Material | Lobe(s) compensated | Notes |
|---|---|---|
| `roughglass` | reflection lobe only | GGX path → Turquin, Schlick path → (1−G) term. Dielectric transmission stays single-scatter (refracted light continues through the bulk). Film coating evaluated after compensation. |
| `carpaint` | all three glossy layers | Same dual-path treatment per layer; diffuse base and coating absorption untouched. |
| `disney` | metallic/specular GGX lobe | `DisneyEvaluate()` multiplies `metallicEval` by the Turquin factor with `F0/F90` derived from `CSpec0` (metallic specular colour). Other lobes unchanged. |

Already covered previously: `metal2` (Heitz'16 exact), `glossy2`,
`glossytranslucent`, `glossycoating`.

## Properties

- `scene.materials.<name>.multibounce = 0|1` — default `0` for all three
  materials (opt-in; existing scenes render identically).
- Blender: "Multibounce" checkbox on the Disney, Carpaint and Glass
  (rough mode) nodes, matching the existing Metal/Glossy2 UI.

## Validation

`dev-tools/e50_multibounce_parity.py` renders a white-furnace sphere per
material with `multibounce` off/on and checks energy non-decrease plus
CPU/GPU parity (mean-relative). Current results (cornell sphere, 320×240,
96 spp):

| Case | off | on | gain | GPU parity |
|---|---|---|---|---|
| roughglass GGX (α=0.4·aniso, kt=0) | 0.0401 | 0.0402 | 1.003 | 0.1% |
| roughglass Schlick (same) | 0.0306 | 0.0431 | 1.410 | 0.6% |
| carpaint GGX (m=0.5/0.3/0.15) | 0.4305 | 0.4311 | 1.001 | 0.2% |
| disney metallic 0.9 rough 0.55 | 0.5702 | 0.6211 | 1.089 | 0.3% |

Dielectric GGX Fresnel averages are small (~5%), so roughglass gain is
physically modest at α≈0.2; the Schlick `(1−G)` term is the legacy
coating approximation and intentionally overestimates. Disney's metallic
lobe shows the expected ~9% recovery at high roughness.

720p visual check: `dev-tools/e50_visual_showcase.py` (cornell studio,
AgX Punchy) — sellmeier glass / openpbr CB'15 jade SSS / metal2 chromium
+ edge tint / charlie velvet walls / disney multibounce floor.
