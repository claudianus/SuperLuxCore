# ReSTIR DI — Resampled Importance Sampling for Direct Illumination

Status: implemented (opt-in). Light strategy `RESTIR_DI`. RIS reservoir over a
target-proportional proposal, with candidate-generation, temporal and spatial
reuse stages. CPU + GPU (OpenCL/Metal) kernels.

## What and why

Direct lighting with many lights is normally handled by importance sampling a
subset per bounce. ReSTIR instead keeps a small **reservoir** per pixel that
holds a weighted sample of "good" lights and **reuses** reservoirs across time
(temporal) and space (spatial neighbours), so each pixel effectively sees far
more candidate lights than it samples. This is the technique behind
sample-efficient direct lighting in recent real-time and offline renderers.

## References

- Bitterli, Wyman, Pharr, Shirley, McGuire, Baxter. **Spatiotemporal Reservoir
  Resampling for Real-Time Ray Tracing with Dynamic Direct Lighting.** SIGGRAPH
  2020. (ReSTIR DI itself.)
- Lin, Wyman, Yakowitz, Bitterli, Ensor. **Generalized Resampled Importance
  Sampling: Foundations of ReSTIR.** SIGGRAPH 2022. (GRIS — the unbiasedness
  framework the spatial merge is based on.)
- Talbot, Cline, Egbert. **Importance Resampling for Global Illumination.** EGSR
  2005. (RIS — the reservoir scheme ReSTIR builds on.)

## Implementation

Light strategy `restir_di` plugs into the existing light-selection path:

| Stage | Commit(s) | What |
|---|---|---|
| 1 | `LightStrategy: add RESTIR_DI` | RIS reservoir over a proposal distribution; weighted reservoir sampling per bounce. |
| 2 | `stage 2: zero-weight occluded` | Occluded candidates get zero weight (cheap pruning). |
| 3 | `stage 3: contribution-aware` | `SampleLightsBSDF` target weights candidates by actual contribution. |
| 4 | `stage 4: spatial reuse (hashed grid)` + `adaptive candidate count` | Neighbouring reservoirs merged through a spatial hash grid. |
| GPU | `GPU kernel port`, `per-pixel temporal reuse (opt-in)`, `spatial reuse` | Reservoir stages ported to OpenCL/Metal kernels. Spatial reuse (E2b) is a **screen-space neighbour-pixel merge**: up to 2 pseudo-random pixels in a 5x5 window, gated by a same-surface check (hit point within 2% of the world radius, landing normal within ~25 deg). It replaced an earlier world-space hash-grid port that merged unrelated surfaces and measured ~1.1-1.3x *worse* RMSE on manylights (e14); the pixel merge is neutral on spots and ~1.1x on mesh, and stays opt-in. |
| Visibility | `restir-di: visibility-weighted RIS target on GPU` + CPU parity | GPU: candidate shadow rays ride the tail of `rays[]/rayHits[]` (slots `[taskCount, taskCount*(1+K+2))`), are resolved by the new `MK_RT_RESTIR` state. CPU: candidates are traced inline on the scene accelerator inside `SampleLightsBSDF()`. Both sides give each candidate its own light-surface sample which doubles as the contribution sample - re-sampling the winner elsewhere than the tested point would bias the binary V term. On CPU the winning candidate's sample is handed back through `SampleLightsBSDF`'s `lightSurfaceUs` out-parameter; the winner's real shadow ray still goes through the normal transparent-shadow/shadow-catcher path. Under visibility, CPU cross-cell spatial merges are skipped (a merged winner's V-free re-evaluation would mix target measures); on GPU the gated pixel-space merge runs with REAL visibility (see E2d). Opt-in (`lightstrategy.restir.visibility.enable`). |
| Shift | `reconnection shift on spatial merge` (E2c) | The reservoir stores the winning sample's light-surface draws (`lsU/lsV/lsP`). A neighbour merge replays the SAME emitter point at the current shade point - GRIS's reconnection shift - so pi_new/pi_old reflects only the shading change, not a different point on the light; on win the sample propagates. |
| Vis-shift | `visibility-aware spatial merge` (E2d) | Under the visibility target, the 2 merge-candidate shadow rays ride the same tail trace as the K fresh candidates (slots `[K, K+2)` per task): the enqueue replays each gated neighbour's stored light-surface sample, and MK_RT_RESTIR folds the traced hit into pi_new of the GRIS weight - occluded transfers contribute zero instead of the V-free approximation. |
| GRIS | `GRIS-correct spatial reservoir merge` | Generalized RIS weighting keeps the spatial merge unbiased. |
| Fixes | `exact direct-hit MIS under culling`, `bounded negatives` | Dark-bias / energy corrections. |

### Properties

- `light.strategy.type = RESTIR_DI` (opt-in; default stays the classic
  importance-sampled strategy).
- `lightstrategy.restir.visibility.enable` (default off) folds the binary
  visibility term into the RIS target: each candidate's shadow ray is traced
  before reservoir resolution (GPU: batched through `rays[]`; CPU: inline on
  the scene accelerator) and V multiplies its target weight.
- Adaptive candidate count and temporal/spatial reuse are internal tunables;
  experimental bounds are **default-off**.

### Known limitations (honest status)

- Reuse is currently **DI only** — no ReSTIR PT / GI / PG stages.
- Under visibility weighting, CPU spatial merges are restricted to the
  own-cell/temporal reservoir (a merged winner's V-free target
  re-evaluation would mix two target measures); on GPU the pixel-space
  merge queues per-neighbour shadow rays with the candidate batch, so
  pi_new carries the traced V - not an approximation (E2d).
- The per-pixel reservoir slot stores the **pre-spatial-merge** state.
  Storing post-spatial totals feeds an inflated wSum back into the
  neighbours' merges on the next pass and compounds (measured ~2x mean
  bias on the spots scene). A representative-winner gate
  (`target >= 5% * wSum/M`) is applied on merge reads: a fluke
  tiny-target winner would otherwise explode the pi_new/pi_old ratio
  (measured ~20x RMSE hot pixels). The GPU merge additionally clamps
  the GRIS ratio at 64 (`RESTIR_MERGE_MAX_TARGET_RATIO`): even gated,
  a steep emission gradient (cone edge) can produce an outlier ratio
  and the paid contribution scales with wSum - observed as
  intermittent ~9x RMSE tail events before the clamp. The CPU merge
  deliberately stays unclamped (its store-side filtering keeps
  unrepresentative winners out of the grid); the bounded divergence
  is confined to the outlier tail.
- The binary V term reallocates error into the penumbra tail: on
  mostly-visible scenes the visibility-weighted target can measure
  ~1.0-1.8x worse RMSE than the unweighted baseline at low SPP on both
  backends (e16/e18 T2 asserts the spread stays bounded, not that V is
  always a win).
- Reuse is **primary-hit (depth-0) only** on GPU. The per-pixel
  reservoir's stored target is measured at that pixel's depth-0 point;
  merging it at a deeper vertex without re-evaluating the stored
  winner's target is not a valid GRIS merge (the pi_new/pi_old ratio
  degenerates to 1 and scales the output weight by an arbitrary
  factor). The temporal merge, spatial merge and reservoir store are
  all gated on `depth == 0` (fixed in the audit; deeper vertices
  previously merged the primary reservoir and biased the estimate).
- On CPU, `lightstrategy.restir.temporal.enable` runs the own-cell
  merge alone when spatial reuse is off (previously the flag was
  parsed but never consumed - a silent no-op while the GPU honoured
  it). The own-cell bucket is the CPU counterpart of the GPU
  per-pixel temporal reservoir.
- Experimental stages are gated default-off pending regression coverage.

## Test scenes / validation

- `scenes/manylights/` — many-light direct illumination exercises the reservoir.
- `dev-tools/e16_restir_visibility_gpu_test.py` — visibility-weighted target
  vs a LOG_POWER reference: unbiased within 3%, bounded variance, finite
  output, temporal+spatial combination (8/8 PASS on Metal).
- `dev-tools/e18_restir_visibility_cpu_test.py` — same gates on PATHCPU
  (8/8 PASS).
- `dev-tools/e14_restir_spatial_gpu_test.py` — pixel-space spatial reuse:
  unbiased within 3%, bounded variance vs no-reuse baseline (6/6 PASS).
- Validated on CPU and Metal GPU against the classic strategy for unbiasedness
  (no dark bias after the MIS / GRIS corrections).

## Platforms

CPU, OpenCL GPU, Metal GPU. The reservoir state is device-resident on GPU.
