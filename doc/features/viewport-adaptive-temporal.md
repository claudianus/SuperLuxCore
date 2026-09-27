# Viewport adaptive sampling, temporal reuse & interactive smoothing

## What

Four viewport-only systems layered on top of the instant-coverage
pipeline ([viewport-instant-coverage.md](viewport-instant-coverage.md)).
All of them operate on the imagepipeline display buffer or on *which*
pixels get sampled next — never on the accumulated film data, so the
final render stays unbiased.

1. **Adaptive sampling** (`sampler.*.adaptive.strength`): once the film's
   host-side `NOISE` estimate is warm, each sampler probabilistically
   skips pixels already below the noise threshold and keeps a uniform
   floor so every pixel still converges. Compute migrates to noisy
   regions (glass, caustics, glossy, light leaks).

2. **`VIEWPORT_TEMPORAL` imagepipeline plugin**: keeps a display-side
   snapshot of the previous frame plus the `POSITION` (world position)
   channel. After a camera-only edit, while the film is reset and
   repaints sparsely, each history pixel's world position is reprojected
   into the new camera (`world -> camera space -> raster`, same math as
   `Camera::ProjectPointToFilm`), z-splat by camera depth, and used to
   fill pixels that have no real samples yet.

3. **`VIEWPORT_SMOOTH` imagepipeline plugin**: à-trous edge-aware
   wavelet filter (Dammertz 2010 / SVGF '17 lineage) applied only to
   pixels below `minsamps` accumulated weight — converged pixels are
   passed through untouched.

4. **LT speckle softening** (`VIEWPORT_INFILL` `ltblend`): light-tracing
   splats (screen-normalized radiance) count as coverage but also blend
   toward their covered neighbours, turning the LT "static" pattern into
   a smooth light-fog that converges.

## Why

Artists judge a viewport in the first ~200ms after an edit. The goal is
not faster convergence of the film (that stays the honest, unbiased
accumulator) but a *displayed frame* that is coherent, spatially
complete, and spends compute where it is actually missing. Camera
orbits in particular used to drop the display to a sparse repaint for
several hundred milliseconds; with `VIEWPORT_TEMPORAL` the reprojected
previous frame holds 60–85% coverage through the reset window.

## How it works

### Adaptive sampling

- `film.noiseestimation.*` produces a host-side `NOISE` channel
  (variance estimate per pixel); for OpenCL engines the host uploads it
  to the device — the kernel never writes it.
- Samplers read `noise[pixel]` and reject a sample when
  `noise < 1 - strength` with probability ramping to `strength`; a hard
  floor (`max(noise, 1 - strength)` semantics + periodic full-coverage
  passes) keeps the estimate unbiased.
- Supported: `TILEPATHSAMPLER`, `RTPATHCPUSAMPLER`, `SOBOL`, `RANDOM`,
  `PMJ02` (shared stratified path). `USER_IMPORTANCE` channel weight is
  an optional extra term (`adaptive.userimportanceweight`).
- Blender: *Viewport > Adaptive Sampling* writes
  `sampler.<type>.adaptive.strength = 0.8` for the active sampler.

### Temporal reprojection

- `RenderSession::PublishViewportCamera()` runs at `Start()` and on
  every `EndSceneEdit()`: serializes `rasterToCamera`, `cameraToWorld`,
  `worldToRaster`, `worldToCamera`, `clipHither` and a
  `cameraonly` flag (`editActions.HasOnly(CAMERA_EDIT)`) into film
  metadata, which survives `Film::Reset()`.
- The plugin snapshots display pixels + `POSITION` (host channel,
  device-uploaded like every film channel). History lives in the plugin
  instance, so it survives film resets.
- On every `Apply` with `cameraonly=1`: each history pixel with finite
  POSITION reprojects into the current camera and z-splats by
  camera-space `z` (nearest wins, `atomic<float>` CAS min); uncovered
  display pixels take the winning history color. History pixels update
  only where the current frame has real coverage AND a finite position
  (LT splats are coverage but carry no POSITION).
- Non-camera edits leave history unwarped (stale material colors would
  be wrong); the plugin still keeps updating from fresh samples.

### Edge-aware smoothing

- `minsamps` (default 8) weights gate the filter: only low-sample
  pixels are filtered and only low-sample neighbours contribute, so
  converged regions are bit-exact passthrough.
- 3 à-trous iterations at dilated steps 1/2/4, binomial 3x3 kernel,
  edge stops: relative depth discontinuity, shading-normal deviation
  (`AVG_SHADING_NORMAL`), luminance distance.
- Needs `DEPTH` + `AVG_SHADING_NORMAL` channels; the adapter requests
  them automatically when `use_smooth` is on (viewport only).

## Channels used (all optional; missing channel = feature silently off)

| Feature | Channels |
|---|---|
| Adaptive | `NOISE` (+ optional `USER_IMPORTANCE`) |
| Temporal | `POSITION`, `RADIANCE_*` coverage |
| Smooth | `DEPTH`, `AVG_SHADING_NORMAL`, `RADIANCE_*` weight |
| LT soften | `RADIANCE_PER_SCREEN_NORMALIZED` |

Note: PATHOCL kernels currently do not write `sampleResult->depth`/
`position`, so DEPTH/POSITION are empty on that engine and temporal/
smooth degrade to no-ops there. RTPATHOCL, RTPATHCPU, PATHCPU,
BIDIRCPU write them.

## Blender controls (Viewport panel)

| Control | Property |
|---|---|
| Instant Coverage | `use_infill` (default on) |
| LT Speckle Softening | `lt_blend` (0–1) |
| Adaptive Sampling | `use_adaptive` (default on) |
| Temporal Reuse | `use_temporal` (default on) |
| Interactive Smoothing | `use_smooth` (default on) |

## Tests

`dev-tools/e52_viewport_adaptive_temporal.py`:

| Leg | Assertion | Result |
|---|---|---|
| adaptive RTPATHOCL | coverage > 0.6, 12/16 cells, finite | PASS (0.75–0.81) |
| LT speckle | softened <= raw speckle count, finite | PASS |
| temporal | warp advantage > 0.02 in reset window, warp correlates with pre-edit frame | PASS (adv ~0.03, corr ~0.97) |
| smooth | speckle non-increasing, finite | PASS (2729 -> 1486 px) |
| adaptive RTPATHCPU | coverage > 0.6, 12/16 cells, finite | PASS (0.96) |

`dev-tools/e51_viewport_infill.py` still passes on all four legs
(RTPATHOCL, PATHOCL, RTPATHCPU, BIDIRCPU).

## Caveats

- `scene.Parse` of `scene.camera.*` properties does **not** register
  `CAMERA_EDIT` — camera edits must go through `Scene::GetCamera()`
  (`Translate`, `Rotate`, …), which is also what Blender uses. Temporal
  reuse therefore does not engage for property-level camera edits.
- The temporal warp fills *displayed holes only* — it never biases the
  film. Disoccluded regions (no reprojected history) are left for the
  infill pass.
- `Film::Reset(true)` on the fast camera-edit path clears the
  accumulation next pass boundary; the temporal plugin bridges exactly
  that window.
