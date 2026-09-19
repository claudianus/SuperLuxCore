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
| GPU | `GPU kernel port`, `per-pixel temporal reuse (opt-in)`, `spatial reuse` | Reservoir stages ported to OpenCL/Metal kernels; the spatial hash grid is appended to the reservoir buffer and merged with the same GRIS weights as CPU. |
| GRIS | `GRIS-correct spatial reservoir merge` | Generalized RIS weighting keeps the spatial merge unbiased. |
| Fixes | `exact direct-hit MIS under culling`, `bounded negatives` | Dark-bias / energy corrections. |

### Properties

- `light.strategy.type = RESTIR_DI` (opt-in; default stays the classic
  importance-sampled strategy).
- Adaptive candidate count and temporal/spatial reuse are internal tunables;
  experimental bounds are **default-off**.

### Known limitations (honest status)

- Reuse is currently **DI only** — no ReSTIR PT / GI / PG stages.
- The RIS target does **not** yet include the visibility term; spatial reuse
  therefore has a measured ~2x inefficiency vs a visibility-weighted target
  (roadmap E2). Without shift mappings, spatial reuse does not reduce RMSE on
  the manylights validation scenes - measured ~1.1-1.3x worse on CPU and GPU
  alike (`dev-tools/e14_restir_spatial_gpu_test.py`); it stays opt-in.
- Experimental stages are gated default-off pending regression coverage.

## Test scenes / validation

- `scenes/manylights/` — many-light direct illumination exercises the reservoir.
- Validated on CPU and Metal GPU against the classic strategy for unbiasedness
  (no dark bias after the MIS / GRIS corrections).

## Platforms

CPU, OpenCL GPU, Metal GPU. The reservoir state is device-resident on GPU.
