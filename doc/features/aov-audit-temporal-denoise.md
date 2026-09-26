# Render pass (AOV) audit + temporal animation denoising plan

Status: D0 + D1 implemented (commits `74aa37f11`, `99e3e2637`, and the
TEMPORAL_ACCUMULATE plugin commit). Updated 2026-09-24.

## Part 1 — AOV audit vs production baseline

Current `FilmOutputType` set (46 types, `include/slg/film/filmoutputs.h`)
vs. what production pipelines expect (Cycles/Arnold/RenderMan/V-Ray).

### Already at or above production level

- Component decomposition: DIRECT/INDIRECT × DIFFUSE/GLOSSY/SPECULAR ×
  REFLECT/TRANSMIT — richer than Cycles (no reflect/transmit split there).
- `RADIANCE_GROUP` — per-light-group relighting. Production-critical, present.
- `BY_MATERIAL_ID` / `BY_OBJECT_ID` — per-object/material beauty splats.
- `DIRECT_SHADOW_MASK` / `INDIRECT_SHADOW_MASK`, `EMISSION`, `CAUSTIC` (fork).
- Geometry: `DEPTH`, `POSITION`, `UV`, `GEOMETRY_NORMAL`, `SHADING_NORMAL`,
  `AVG_SHADING_NORMAL` (denoise-safe average).
- Denoise guides: `ALBEDO`.
- Diagnostics: `SAMPLECOUNT`, `RAYCOUNT`, `CONVERGENCE`, `NOISE`,
  `USER_IMPORTANCE`, `IRRADIANCE` — better observability than most engines.
- Mist via `MIST` image-pipeline plugin (not an AOV, acceptable).

### Gaps, ordered by production impact

1. **`MOTION_VECTOR` — missing, highest priority.**
   Required by: OIDN 3 temporal mode, any temporal reprojection filter,
   compositing motion blur, TAA. Infrastructure already exists:
   `MotionSystem.Sample(time)` (camera + object transforms), first-hit
   `SampleResult.position`. Design: at the first visible vertex, project the
   hit point through camera+object motion sampled at shutter edges
   (extrapolate to frame −1/+1 when shutter < 1 frame) → screen-space
   backward (cur→prev) and forward (cur→next) flow, 4 floats/px.
   Limitation: LuxCore has transform motion only (no deformation blur yet),
   so MV covers camera+object motion; deformation MV comes with deformation
   blur later. Must land in CPU `pathtracer.cpp` + OpenCL kernels +
   `sampleresult`/`film` channels for parity.
2. **`VARIANCE` — missing.** Per-pixel radiance variance (E[x²]−E[x]²).
   Arnold `noice` requires variance AOVs for temporal denoising; our own
   temporal filter also needs it for per-pixel confidence. Cheap: one extra
   accumulation buffer. The BCD `FilmDenoiser` already accumulates sample
   covariance internally — exposing it as a channel is straightforward.
3. **`VOLUME` split — missing.** Volume scattering folds into the
   diffuse/glossy component channels. `bsdf.IsVolume()` is already tracked
   in the path tracer (`pathtracer.cpp`), so DIRECT_VOLUME/INDIRECT_VOLUME
   channels are a small plumbing change. Needed for volumetric relight/comp.
4. **`CRYPTOMATTE` — missing.** Industry-standard hashed ID mattes
   (object/material/asset). Our per-sample single IDs
   (`materialID`/`objectID`) are compatible with the Cryptomatte
   coverage-ranking model (accumulate top-N IDs across samples). Medium work:
   coverage accumulation + manifest JSON in EXR metadata.
5. **`ENVIRONMENT` — missing.** Environment-light contribution currently
   folds into DIRECT_*. Separate pass wanted for relight workflows; needs an
   env-light flag in `AddDirectLight` (we already distinguish env lights in
   `SampleLightPdf` paths).
6. **Multilayer/multi-part EXR packaging — missing.** Every output is a
   separate file today (`export/aovs.py` emits one filename per output).
   Production comp expects a single multi-part EXR. Moderate work in the
   film save path.
7. **Nice-to-have:** `AO`, `SUBSURFACE` split (SSS currently inside
   diffuse), shadow-catcher AOV (`SampleResult.isHoldout` exists — check
   completeness), LPEs (component splits + light groups cover most cases).

### Denoiser-aux completeness

| Signal        | OIDN 2.x | OIDN 3 temporal | ReLAX-class | Status   |
|---------------|----------|-----------------|-------------|----------|
| albedo        | yes      | yes             | (diffuse demod) | `ALBEDO` ✓ |
| normal        | yes      | yes             | yes         | `AVG_SHADING_NORMAL` ✓ |
| depth/viewZ   | -        | yes             | yes         | `DEPTH` ✓ |
| motion vec    | -        | **required**    | yes         | **missing** |
| prev/future frame | -    | **required**    | history     | plumbing missing |
| variance      | -        | optional        | internal    | **missing** |
| hit distance  | -        | -               | yes         | derivable from `DEPTH` |

## Part 2 — Temporal animation denoising

Current state: OIDN (Metal device, per-frame; components mode with albedo
demodulation + firefly clamp — see `oidn-film.md`), BCD (per-frame, deep
sample statistics), OptiX (per-frame, CUDA only). **Nothing is temporal.**
Per-frame denoising at low spp produces per-frame-incoherent residual
noise → flicker.

### Verified SOTA landscape (2026-09)

- **OIDN 3** (announced HPG 2025, release H2 2026 — imminent): U-Net moved
  to kernel prediction; **temporal mode**. RT variant inputs: previous
  denoised frame + backward motion vectors + depth. Final-quality variant
  also consumes future frames + forward MVs. Integrating it is our
  highest-leverage path and *requires* `MOTION_VECTOR` + sequence plumbing.
  (CG Channel / OIDN releases; latest shipped is 2.5.0.)
- **Arnold `noice`**: production-proven sequence denoiser — temporal
  stability frames (`-i` extra frames) + variance AOVs, patch-search based
  (no MVs needed). Confirms the multi-frame + variance model.
- **Deep Compositional Denoising on Frame Sequences** (Disney/ETH 2023):
  temporal extension of component-decomposed denoising — our `components`
  mode is already the spatial half.
- **Robust Average Networks** (2023): converts spatial denoise CNNs into
  spatio-temporal ones via latent interpolation blocks.
- **NRD ReLAX/ReBLUR** (SVGF family): 1-spp-class spatio-temporal
  denoising; inputs = MV, normal, roughness, viewZ, hit-distance.
  D3D12/Vulkan only, NVIDIA RTX SDK license → reference architecture for
  our own Metal kernels, not a library we can ship.
- **Heitz & Belcour 2019**: permuting pixel seeds between frames spreads
  MC error as temporal blue noise — attacks flicker at the *sampler* level,
  near-zero cost.
- **Adaptive Fusion (IEEE Access 2024)**: fuse raw 1-spp and temporally
  accumulated image — useful pattern when history is unreliable.
- **ReSTIR across frames**: our DI/GI reservoirs could persist across
  animation frames via MV reprojection (GRIS temporal reuse). Off-label
  use — introduces correlation, treat as a quality mode; unique to us since
  the reservoir infra already exists.

### Layered plan

- **D0 — AOV prerequisites:** `MOTION_VECTOR` (fwd+bwd), `VARIANCE`.
  CPU+GPU parity (both `pathtracer.cpp` and the OCL kernels), film channel
  + output plumbing, SuperBlendLuxCore toggle + EXR.
- **D1 — Sequence temporal prefilter ("luxtd")**: post tool over the
  rendered EXR sequence (noice-style, also usable as an in-addon step):
  reproject history via backward MV → validate with depth/normal/ID
  (disocclusion) → variance-guided exponential accumulation of the *noisy
  linear HDR* input → single OIDN components pass on the accumulated
  signal. Accumulating pre-denoise keeps network input statistics
  consistent and lifts effective spp; where reprojection fails, fall back
  to per-frame denoise. No training, biggest quality-per-effort step.
- **D2 — OIDN 3:** bump the dep when it ships; feed prev-denoised frame +
  backward MV + depth (RT mode) or ±frames + both MVs (final mode); keep
  per-component decomposition feeding the temporal filter. Our Metal
  device plumbing is already in place.
- **D3 — Source-level temporal coherence:** frame-correlated sampler seeds
  (same seed schedule every frame = correlated, temporally stable residual)
  and/or Heitz–Belcour seed permutation between frames. Small sampler
  change (`SOBOL`/`PMJ02` already deterministic — make frame correlation a
  flag and measure). Optionally ReSTIR reservoir carry-over via MV.
- **D4 — research tier (differentiator):** inputs nobody else has —
  per-pixel sample histograms/covariance from the BCD `FilmDenoiser`
  accumulators, spectral bins, component decomposition → compact temporal
  kernel-predicting network trained on LuxCore data, or RAN-style latent
  temporalization of an existing net. Also the place for a native Metal
  ReLAX-class filter if 1–4 spp interactive final quality is wanted.

### Expectations

- 16–32 spp + D1 (+D2 when OIDN 3 lands) ≈ flicker-free final quality for
  most content; disocclusions/fast content still need the raw-frame
  fallback path.
- 1–4 spp stable output is ReLAX-class territory — plan for D4 or the
  OIDN 3 RT mode, not for the offline path.

### Touch points

- `include/slg/film/film.h`, `src/slg/film/film.cpp` — channel + buffer.
- `src/slg/film/filmoutputs.cpp` — output type strings.
- `src/slg/engines/pathtracer.cpp` + `include/slg/engines/pathoclbase/
  kernels/*` — first-hit MV/variance writes (CPU/GPU parity rule).
- SuperBlendLuxCore: `properties/aovs.py`, `ui/view_layer_aovs.py`,
  `export/aovs.py`.
- New: `dev-tools/` sequence-denoise tool (or `luxcoreconsole` subcommand)
  + e-test regression (`e3x_temporal_denoise_test.py`): synthetic moving
  scene, flicker metric = per-pixel temporal variance of denoised luma vs
  reference.

## Implementation notes (2026-09-24)

### D0 — done

- `VARIANCE`: `GenericFrameBuffer<4,1,float>` accumulates the weighted
  second moment E[x²] of merged radiance; output derives
  `max(E[x²] - E[x]², 0)`. HDR-only.
- `MOTION_VECTOR`: raw `{vx, vy, valid, objectMotion}` in px/frame,
  forward-in-time. Finite-difference over the shutter interval at the
  first camera-visible hit (CPU `pathtracer.cpp`,
  `Camera::ProjectPointToFilm`; GPU `PathOCL_ComputeFirstHitMotionVector`
  in `camera_funcs.cl`). Environment misses are handled by projecting a
  point at 1e6 along the ray (camera rotation exact, translation error
  ~1e-6 relative). GPU does NOT support per-vertex deformation (vertex
  motion buffers are not passed to path kernels - transform component
  only). CPU/GPU parity validated (max |vx| differs <0.1%).
- Pre-existing bug found and fixed (`99e3e2637`): default object IDs came
  from a process-global counter `defaultObjectIDIndex` → shifted every
  session → per-frame OID instability. Now FNV-1a(name) & 0xffffff.
  Required by both the temporal validator and compositing matte
  consistency across animation frames.

### D1 — `TEMPORAL_ACCUMULATE` image-pipeline plugin

`src/slg/film/imagepipeline/plugins/temporalaccumulate.cpp`. Runs first
in a pipeline. Accumulation targets are the image-pipeline beauty **and
every radiance component channel present in the film** (DIRECT_*,
INDIRECT_*, EMISSION) — this is essential, not optional: OIDN
`components` mode denoises the raw film channels, so accumulating only
the pipeline beauty would leave the denoiser's inputs unfiltered (and
the `(beauty − Σcomponents)` residual would re-inject the unfiltered
difference into the output). Validated: accumulating 7 targets made
TA+OIDN both cleaner and more stable than raw+OIDN.

Per pixel:

1. Reverse reprojection `p - v(p)` with bilinear taps, each tap
   validated (|Δdepth| relative, dot(prevN,curN), object ID equality,
   empty-tap check). Reprojection/validation is shared across all
   targets.
2. Per-target history from the state EXR (`<TAG>R/G/B/W` channel
   groups; beauty has an empty tag so the file stays backward
   compatible). Missing/zero-weight taps fall back to the current
   frame.
3. TAA-style neighbourhood clip: history clamped to the current 5x5
   mean ± `clipsigma` * sqrt(spatialVar + beauty sampleVar) — the
   VARIANCE channel widens the box on noisy frames so stable history is
   not over-clipped.
4. EMA blend `w = min(prevW+1, history)`, written back into the
   pipeline beauty and into each component channel (normalization
   preserved via the raw weight).
5. State persisted per pipeline index as a multi-channel EXR
   (`slg_temporal_state_<index>.exr` in `.statedir`): per-target
   RGBW + shared Z/NX/NY/NZ/OID. Survives across sessions → works for
   per-frame animation renders.

Properties: `film.imagepipelines.P.N.type = TEMPORAL_ACCUMULATE`,
`.frame` (int, 0 resets history), `.statedir`, `.history` (EMA cap,
default 32), `.clipsigma` (default 2.5, 0 disables), `.depththreshold`
(relative, default 0.05), `.normalthreshold` (min dot, default 0.6).
The parser auto-requests MOTION_VECTOR/DEPTH/AVG_SHADING_NORMAL/
OBJECT_ID/VARIANCE channels.

Caveats: the plugin writes accumulated values back into the film's
component channels, so it is designed for final-frame pipelines
(render to halt → pipeline once). Repeated mid-render executions
double-count the same frame in the EMA. On PATHOCL the channel
buffers are transferred asynchronously — the caller must let the film
update land before executing the pipeline (a finished/halting render
session already satisfies this).

Validated (1280x720 PATHOCL, bigmonkey-motion split into real
per-frame scenes, TA → OIDN components → tonemap):
- history acceptance ~80-92% on true animated frames
- temporal luma std 0.025 vs 0.063 raw+OIDN (~60% flicker reduction)
- component accumulation prevents an OIDN blow-up observed on raw
  input (a frame collapsed to ~50% mean luminance with spatial
  artifacts)
- no ghosting; regression: `tests/temporal_accumulate_test.py`.

SuperBlendLuxCore: `denoiser.temporal_*` properties, Temporal Accumulation
sub-panel under Render > Denoiser; plugin is prepended to the denoiser
pipeline (and to pipeline 0 when no denoiser) for final renders.
Animated seed (`use_animated_seed`) already provides per-frame
decorrelated noise — the accumulation needs it to converge.

## References

- OIDN 3 temporal denoising announcement (HPG 2025 keynote; release
  H2 2026) — cgchannel.com/2026/01/open-image-denoise-3-will-support-
  temporal-denoising/ ; github.com/RenderKit/oidn/releases
- Arnold `noice` temporal stability frames + variance AOVs — Autodesk docs.
- Bálint et al., "Deep Compositional Denoising on Frame Sequences" 2023.
- "Robust Average Networks for Monte Carlo Denoising", arXiv:2310.04080.
- Schied et al., "Spatiotemporal Variance-Guided Filtering" (SVGF) 2017;
  NVIDIA NRD (ReLAX/ReBLUR) — reference architecture.
- Heitz & Belcour, "Distributing Monte Carlo Errors as Blue Noise in
  Screen Space by Permuting Pixel Seeds Between Frames", EG 2019.
- Hasselgren et al., "Neural Temporal Adaptive Sampling and Denoising"
  2020; "Adaptive Fusion Network" IEEE Access 2024.
