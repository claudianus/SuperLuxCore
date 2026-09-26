# Path Guiding — online path-space importance sampling

Status: implemented (M1 CPU + M2/M2b/M2c GPU). Learns a per-region incident
radiance field during the render and samples it to reduce variance.

## What and why

In indirectly-lit scenes the useful light arrives from a few directions, but
uniform BSDF sampling wastes most rays. Path guiding builds an approximate
incident-radiance field (a spatial-directional mixture / table) **during**
rendering and samples directions proportional to it — the renderer "learns
where the light is". This is a leading practical variance-reduction method.

## References

- Vorba, Herholz, Křivánek, et al. **On-line Learning of Parametric Mixture
  Models for Light-Transport Simulation.** SIGGRAPH 2014. (Online learning.)
- Müller, Gross, Novák. **Practical Path Guiding for Efficient Light-Transport
  Simulation.** EGSR 2017. (The guiding method our implementation follows.)
- Jensen. **Importance Driven Path Tracing Using the Photon Map.** EGWR 1995.
  (Early guiding idea.)

## Implementation

| Stage | Commit | What |
|---|---|---|
| M1 | `CPU path guiding M1` | Uniform spatial grid, one-sample MIS (CPU). |
| M2b | `GPU path guiding M2b` | Frozen coarse-table sampling on GPU. |
| M2b-2 | `GPU online training` | Record buffers + drain + swap: GPU trains its table online. |
| M2c | `indirect training + adaptive mixture` | Indirect-radiance training data + depth/count adaptive mixture (CPU+GPU). |
| Rounds | `frozen training rounds` | Cache finalized after N training rounds. |

- Stability fixes: `Fix GPU+guiding exit-GC crash` (drain-path file dumps +
  redundant zeroing removed).
- Experiments (absolute smoothing, stratified ubin) are **default-off**.

## Test scenes / validation

- `scenes/cornell/pg-indirect.scn` — a divider-with-window room whose
  camera-facing compartment is lit only by bounced light, built to exercise
  the deep-indirect case guiding targets.
- Unbiasedness confirmed (guided/unguided mean ≈ 1.0 on all scenes).

## Honest status / measured findings

Guiding is **functional and unbiased but currently shows no measurable
variance reduction** on the tested scenes (Cornell, the pg-indirect room,
a narrow-window variant). Three compounding reasons, verified by
inspection and A/B renders:

- **Conservative gating.** The bounce-time guide fires only at eye-path
  `depth >= 2` (M2c choice — earlier bounces are served well enough by
  direct-light sampling that guiding them only dilutes), and only on
  glossy BSDFs by default (`BSDF_GetGlossiness() >= 0.3`). A purely
  diffuse bounce is opt-in via `LUX_PG_DIFFUSE=1`.
- **Coarse field.** The directional histogram is a flat per-cell bin grid:
  16³ spatial × (16φ×8θ)=128 directional bins on CPU, and a coarser frozen
  table on GPU (8³ × (8φ×4θ)=32 bins, chunked for memory). A broad or
  slowly-varying indirect field converges to ~uniform inside each bin, so
  the guided proposal collapses toward cosine/BSDF sampling and buys
  nothing — measurable benefit needs a *peaked* deep-indirect field.
- **Fixed earlier:** the `LUX_PG_DIFFUSE` opt-in was unreachable for pure
  diffuse (matte reports glossiness 0 and failed the glossy-lobe cutoff);
  the gate now routes diffuse-only BSDFs to the flag alone.

To make guiding *deliver* a visible gain rather than merely run, the
natural next step is a higher-resolution / better-shaped field (vMF or
directional-mixture per cell, or product-with-BSDF guiding) plus a
validation scene whose variance is dominated by deep (depth≥2) indirect
light.

## Platforms

CPU (M1) and GPU (M2b/M2c: OpenCL + Metal). The GPU diffuse opt-in is not
yet plumbed (GPU gate is glossy-only); see the roadmap parity note.
