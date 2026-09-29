# Path-space regularization design (D1 잔여)

References:

- Kaplanyan & Dachsbacher, *Path Space Regularization for Holistic and
  Robust Light Transport*, CGF 32(2), 2013.
- Weier, Hanika, Dachsbacher, Vorba, *Optimised Path Space
  Regularisation*, CGF 40(4), 2021 — learns per-path-type blur widths
  via differentiable rendering; key practical finding: regularization
  **accelerates guiding learning** (hot paths get found before the
  learned field has to encode them).
- Production equivalent: Blender/Cycles "Filter Glossy", Corona's
  indirect-caustic smoothing.

## Problem

SD(S)…D caustic chains have near-zero probability under BSDF
sampling: a glossy vertex must land exactly inside a narrow solid
angle of the next specular stage. MNEE/LMNEE cover the light-connect
side for (S|DS)+D chains through manifolds, but camera-side and
mixed chains remain. PhotonGI covers classic SSS/SDS caustics only
through the photon path - no glossy-intermediate chains.

## PSR in one line

Replace each BSDF f_r by f_r ⊗ K_σ along the path (K a kernel of
width σ in half-vector/direction space). The integrand gets blurred:
SDS chains acquire finite-measure support, so plain path tracing
resolves them. The estimator is biased (integrates a blurred model)
but self-consistent — the standard production trade-off. OPSR shows
the blur width per vertex type (D/G/S position) is what matters:
blur only non-first, non-delta vertices.

## Engine constraints

- ~20 materials each compute alpha/roughness locally at Sample and
  Evaluate; kernel-side equivalents live in materialdefs_funcs_*.cl.
  A global "just widen alpha" is invasive **unless** funneled through
  the shared microfacet library (`microfacet.h` /
  `materialdefs_funcs_microfacet.cl`) — most glossy/glass materials
  already route alpha through it.
- Delta (perfect mirror/flat glass) lobes: v1 keeps them exact.
  Note the correction vs an earlier draft: PSR does blur δ lobes in
  Kaplanyan — convolving a δ BSDF into a narrow finite kernel is
  precisely how SDS paths acquire finite measure. v1 does not perform
  the delta→glossy substitution (it would change event types and
  MNEE endpoint semantics); that is the follow-up that unlocks
  pure-delta caustics under the eye path.
- Sample/Evaluate must use the SAME inflated alpha or the estimator
  gets double-counted bias — this is the main correctness constraint.
- MIS with NEE: light connections evaluate f_r with the blurred alpha
  too — consistent, still biased w.r.t. the true model (intended).

## Implementation status (v1 landed, opt-in)

- `path.regularization.sigma` (default 0 = off),
  `path.regularization.mindepth` (default 1: the camera-visible
  bounce stays exact).
- Plumbing: `PathDepthInfo.regularization{,MinDepth}` is seeded at
  path init (CPU `PathTracer`, GPU `GenerateEyePath`/light-task init
  from `taskConfig->pathTracer`), `HitPoint_SetRayContext` gates it
  into `HitPoint.regularization` per vertex — zero signature changes,
  covers eye and light paths on PATHCPU/PATHOCL (wavefront + dense).
- Materials route alpha through `RegularizeAlpha()` (microfacet.h) /
  `Microfacet_RegularizeAlpha` (materialdefs_funcs_microfacet.cl):
  `alpha' = sqrt(alpha² + σ²)` — quadratic accumulation in slope
  space matches NDF convolution better than a linear add. Covered:
  glossy2, roughglass, metal2 (both GGX and Schlick paths; CPU + CL).
- Sample/Evaluate/Pdf share the inflated alpha per BSDF instance →
  consistent biased estimator.
- Regression: `dev-tools/e99_psr_regularization.py` (roughglass plate
  scene, σ=0 vs σ=0.06, PATHCPU + PATHOCL; asserts finite output, a
  measurable sigma effect, and a σ=0 anchor band).

## Measured (caustic-roughglass.scn, 384²)

- σ=0.15: visible caustic/shadow region energy recovery on both
  engines (floor mean +8% CPU, brightened S-D-L paths); σ=0 output
  bit-comparable to pre-change.
- RMSE vs unbiased 2048spp reference is NOT the acceptance gate: the
  blur is biased by design and rebalances error (more coverage, less
  peak noise). At moderate σ (0.02-0.06, alpha units) the effect is
  subtle; at 0.15 the material visibly softens.
- Follow-up worth measuring: Kaplanyan's halflife decay
  (`path.regularization.halflife`, σ→0 over samples → consistent in
  the limit) and delta→glossy substitution for pure-SDS chains.

## Design v1 (biased-but-consistent, Corona-style)

1. `path.regularization.sigma` (default 0, i.e. off — opt-in until
   measured), `path.regularization.mindepth` (default 2: camera and
   direct-lighting paths stay exact), `path.regularization.halflife`
   in samples (0 = static blur; >0 decays σ → unbiased in the limit,
   Kaplanyan's consistent scheme).
2. BSDF gains a `float regularization` field set at construction from
   the path vertex count (CPU `PathTracer` knows the depth; GPU
   GPUTask carries the depth state - set in the bounce kernels when
   the BSDF is initialized).
3. Shared helpers `RegularizeAlpha(alpha, reg)` in microfacet.h +
   materialdefs_funcs_microfacet.cl: `sqrt(alpha*alpha + reg*reg)`
   (energy-ish accumulation in alpha² matches NDF convolution better
   than linear add — GGX α behaves like a std-dev of slope space).
4. Materials opt in mechanically: wherever alphaT/alphaB is computed
   for a glossy/specular-rough event, apply the helper. Delta
   branches untouched. ~12 call sites CPU + .cl mirrors.
5. Effect: glossy→specular and specular→glossy chains widen their
   effective lobes → caustic firefly rate drops at the cost of a
   slightly softened specular response (same trade Cycles documents).

## Validation plan

- `e9x`: cornell + focused-caustic-ring at equal time with σ=0 vs
  σ>0 — expect large RMSE drop on caustics, ~0 on diffuse walls.
- Furnace/consistency: blurred dielectric still passes the
  white-furnace gate (conservation holds — the kernel widens the
  lobe, D and visibility normalization already renormalize).
- CPU/GPU parity via `cpu-gpu-parity.sh` with regularization enabled.
- Regression gate: existing e33/furnace tests must pass at σ=0
  (default off) and at σ>0 for blurred materials only.

## Non-goals / follow-ups

- OPSR-style learned per-path-type σ: the fixed-σ v1 measures the
  payoff first; a learned table is a second step (needs the
  differentiable infrastructure we do not have — use OPSR's published
  per-type ratios as a static table instead if warranted).
- Combining with MNEE: regularized chains become cheaper manifold
  seeds — seed-cache hit rate should improve measurably; worth a
  follow-up measurement.
- SDS+S chains remain the domain of MNEE/PGIS - PSR cannot create
  measure on a pure-delta subpath.
