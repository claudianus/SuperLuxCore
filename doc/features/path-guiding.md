# Path Guiding — online path-space importance sampling

Status: implemented. The current field is a lock-free adaptive SD-tree
with per-leaf vMF mixtures (M4e), hierarchical cold-leaf fallback and
BIC-selected component counts (P5), driven by the `path.guiding.*`
properties on both CPU (PATHCPU) and GPU (PATHOCL/Metal). Learns a
per-region incident radiance field during the render and samples it to
reduce variance. The sections below keep the M1–M3 history; the
production feature set is described in "P5: production hardening".

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
- **Fixed:** the BSDF-side one-sample-MIS denominator used a hardcoded
  50/50 mixture while the selector ran the adaptive `MixWeight(total)` —
  biased whenever `wGuide != 0.5`. It now uses the real selection
  probabilities `(1-wGuide)*bsdf + wGuide*guide` on both sides.
- **Fixed:** the training-round swap cadence counted only *kept*
  records. In dim scenes most arrivals carry ~0 local value and are
  dropped before storage, so the counter never reached `SWAP_RECORDS`
  and the read side stayed permanently empty — guiding was silently
  dead exactly in the scenes that need it. `Record()` now counts every
  record attempt toward the round cadence.
- **Fixed:** the record direction convention was inverted. The cell
  field must approximate `L_i(x, w)` — "from x, direction w found
  radiance" — so the record belongs at the *previous* vertex
  (`eyeRay.o`) under the sampled outgoing direction (`eyeRay.d`), valued
  by this vertex's local value. Storing it at the current vertex under
  `-eyeRay.d` learned the reversed transport direction (paths guided
  back along the camera chain instead of toward the radiance).
- **Fixed (E26):** the guide-side `Evaluate` correction divided the
  result by the local cosine for EVERY non-volume material. The
  division exists only to undo Disney's double-cosine quirk (Disney's
  `Evaluate` returns `f * cos^2` — see disney.md); every other material
  already returns the single-cos `f * |cos|` the mixture weight expects.
  Applying it unconditionally stripped the cosine from matte, glossy,
  cloth, hair, ... — guided bounces were over-weighted by ~1/cos (a
  biased brightening, strongest at grazing angles). The division is now
  gated on `MATERIAL_TYPE == DISNEY` on both CPU
  (`pathtracer.cpp`) and the OpenCL kernel
  (`pathoclbase_kernels_micro.cl`); volumes keep their pass-through
  (phase * albedo, no cosine). Regression:
  `dev-tools/e26_pathguiding_cosine_test.py` (guided/unguided mean on
  pg-indirect: 1.001 CPU, 0.96 GPU — the buggy variant drifts high).

## Volume scattering vertices (M3)

`GuidableBsdf` now admits `BSDF::IsVolume()` and `Sample()`/`Pdf()` take
an `isotropic` flag: at volume vertices the cosine-hemisphere weighting
is dropped (media scatter into the full sphere — shadeN = -rayDir has no
shading meaning), and the degenerate-pick fallback is uniform-sphere
instead of cosine. The DL-side mixture pdf uses the same isotropic
`Pdf()`, and the cosine division in the guide-side `Evaluate` is skipped
for volumes (their BSDF carries no cosine factor — `Evaluate` already
returns `phase * albedo`).

### vMF directional fit (M3b, CPU)

Each cell additionally accumulates the directional first moment
`S1 = sum(w_i * flux)` alongside the flat bins. When the cell is warm
and the resultant length `r = |S1|/S0 > 0.05` the incident field is
approximated by a von Mises-Fisher lobe (`mu = S1/|S1|`,
`kappa = r(3-r^2)/(1-r^2)` clamped to 64, Banerjee approximation) — far
better shaped than 128 flat bins for a sharp directional field
(god-ray beam, single dominant source). `Sample()` then draws from
`0.85 * vMF + 0.15 * uniform-sphere` (uBin picks the strategy, exact
closed-form inversion for cos(theta) — no rejection loop); `Pdf()`
evaluates the same mixture `k e^{k(c-1)} / (2 pi (1-e^{-2k}))`. Cells
that fail the fit keep the existing bin path. `LUX_PG_NOVMF=1` forces
the bin path for A/B tests. The fit is CPU-only: the GPU coarse-table
serialization is unchanged (bins only). Measured fit rate in the
god-ray room was ~100% of warm volume cells, i.e. the incident field
there is consistently directional.

Measured on fog scenes (dense homogeneous fog + embedded light, thick
multi-scatter fog, god-ray room, 4 seeds each vs converged refs): the
volume guide is **unbiased** (guided/unguided means agree within ~0.5%
at 256 spp) but **variance-neutral to ~11% worse** — identical result
with vMF and with bins, so the limiter is not the directional
representation: in these scenes the residual noise is dominated by
distance/transmittance sampling and direct-light visibility, which
direction guiding cannot address, while the mixture spends up to 75%
of continuation draws on a noisy field proposal. Kept as opt-in
(`path.guiding.enable`) groundwork.

To make guiding *deliver* a visible gain rather than merely run, the
next steps are product-with-phase guiding (`f * L_i`, not `L_i` alone)
and a validation scene whose variance is dominated by deep (depth≥2)
indirect transport with a peaked field.

## P5: production hardening (hierarchical fallback, adaptive K, properties)

### Hierarchical fallback

A leaf whose own statistics are still cold no longer stays unguided while
its ancestors are already warm. `BuildReadTree` aggregates complete
subtree statistics (`AggStats`), walks cold leaves toward the root, and
fits the nearest ancestor whose aggregate record count satisfies warmup.
The borrowed model is installed on the cold leaf with a damped synthetic
visitation count, so all consumer gates (warmup, mixture weight, GPU leaf
layout) accept it unchanged — the GPU contract is identical, only the
leaf's provenance differs. Debug logs report the borrowed-leaf count per
swap.

### Adaptive component count

Fixed `K=4` EM fits were replaced by per-leaf model selection: candidate
lobe counts 0..cap are seeded greedily, each is EM-fitted, and the fit
with the best BIC (`2*logLik - p*log(N)`, lobe params = weight + 2×mean +
concentration, uniform adds one) wins. Broad/unimodal fields collapse to
K=1 instead of overfitting; genuinely multimodal leaves keep their lobes.
`path.guiding.components` caps the search (default 4). The uniform
component stays as a coverage floor so the pdf keeps positive support.

### Formal properties

Artist-facing configuration is now `path.guiding.*` (parsed by
`PathTracer::ParseProperties`, serialized by `ToProperties`, defaults in
`GetDefaultProps`); `LUX_PG_*` env vars remain as debug fallbacks only.

| Property | Default | Meaning |
|---|---|---|
| `path.guiding.enable` | 0 | master switch |
| `path.guiding.tablefile` | "" | warm-start: load a previously saved tree |
| `path.guiding.savetable` | "" | dump the trained read tree on Stop (CPU **and** OCL) |
| `path.guiding.risk` | 0 | RIS product-guiding candidates (0=off) |
| `path.guiding.mindepth` | 2 | eye-path depth gate |
| `path.guiding.glossythreshold` | 0.3 | min BSDF glossiness |
| `path.guiding.diffuse` | 0 | also guide diffuse-only BSDFs |
| `path.guiding.strength` | 1 | guide-side MIS scale |
| `path.guiding.warmup` | env/1024 | leaf visitation gate (floored at the GPU constant) |
| `path.guiding.freeze` | 1 | freeze structure once converged |
| `path.guiding.debug` | 0 | debug counters/visualization |
| `path.guiding.swaprecords` | 65536 | training round cadence |
| `path.guiding.split` | 0.01 | leaf split threshold |
| `path.guiding.maxdepth` | 10 | SD-tree max depth |
| `path.guiding.maxleaves` | 4096 | leaf budget |
| `path.guiding.components` | 4 | max vMF lobes per leaf |

Portal fields: `path.portal.<i>` (12 floats = 4 CCW corners),
`path.portal.count`, `path.portal.weight`, `path.portal.sidegate`,
`path.portal.adapt`.

Cache settings resolve once per engine through
`PathGuidingCache::SettingsFromProperties` — PATHCPU and PATHOCL share the
identical resolution path, and the OCL task config carries the bounce
gates (`guidingRisK`, `guidingMinDepth`, `guidingGlossiness`,
`guidingDiffuse`, `guidingStrength`) so the kernels apply the same policy
with no host-side constants.

### Validation

- `dev-tools/e43_pathguiding_test.py` — table save/load roundtrip,
  unbiased mean (~1.000), guided-variance sanity band, env fallback +
  property precedence, `strength=0`, `components=1`, PATHOCL parity,
  hierarchical fallback (borrowed-leaf count > 0), cold-start stability.
- `dev-tools/e43_pathguiding_visual.py` — 1280×720 AgX-Punchy PATHOCL
  renders of `scenes/cornell/pg-gallery.scn` (a hall lit only through a
  window from a hidden rear room): guided warm-start measured ~1.1×
  indirect-RMSE better than unguided at 48 spp and shows the warm-start
  production flow (`savetable` on the long render → `tablefile` on the
  shot).

**Measurement caveat (verified):** PATHCPU renders are nondeterministic
across identical calls — each render thread walks its own Sobol stream
over normalized pixel space, so pixel/sample assignment follows thread
scheduling (and external CPU load). Same-seed renders differ by up to
~4 luminance and 5-run ensemble variance ratios swing 0.5×–1.8× on
unchanged code (pre-P5 and P5 alike). All quantitative checks therefore
run on PATHOCL (deterministic task/seed batching). On deterministic GPU
at test scales (320×180–720p, ≤64 spp) the trained field measures
**variance-neutral to ~10–20% worse** on the pg-* scenes — guiding's
benefit needs deeper-indirect scenes and longer budgets; the sanity
bound is what the regression enforces.

## Platforms

CPU (M1+) and GPU (PATHOCL: OpenCL + Metal) share the same field, gates
and leaf format; the diffuse opt-in is plumbed on both backends.
