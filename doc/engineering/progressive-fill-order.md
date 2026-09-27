# Progressive fill order (viewport)

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Progressive fill order (viewport-visible) — verified 2026-09

Bucket samplers (SOBOL / RANDOM / PMJ02-via-SobolSharedData) used to
serve pixel buckets in Morton-tile row-major order, so CPU engines
(PATHCPU, BIDIRCPU/BIDIRVMCPU eye pass, hybrid back-forward) visibly
filled the film bottom-to-top on the first pass. `Sampler::
ScatterBucketIndex` (sampler.h) now permutes the sequential bucket
index via a golden-ratio stride bijection (i*k mod n, k coprime to n):
one-to-one, so coverage/pass accounting is untouched — just scattered.
Regression: `pyunittests/.../testbucketscatter.py`.

Measured fill order (720p, heavy scene, band coverage over time):
- RTPATHCPU / RTPATHOCL: scattered coarse first pass — uniform ✓
- PATHCPU/BIDIRCPU + SOBOL/RANDOM/PMJ02: scattered after fix ✓
- PATHOCL (SobolOCL): taskCount ≈ pixelCount, all buckets per launch —
  full-frame every iteration ✓ (Metal & Vulkan same code path)
- TILEPATHCPU/OCL: Hilbert tile order, completes tile-by-tile from the
  lower-left — intentional (cache locality); final-render only, never
  used for viewport display.
- LIGHTCPU / Metropolis: random splats / random walk — no pixel order.

## Update — lattice RT coverage + VIEWPORT_INFILL (2026-09)

- `sampler_tilepath_funcs.cl` RTPATHOCL branch: stride-R grid + Morton
  order replaced by a rank-1 lattice `pix = (i*A + epoch) % pixelCount`
  walked continuously across preview and steady phases. Preview-phase
  R×R block splat (weight 0.001) removed — the display-side infill
  reconstructs gaps instead, which is smoother and uses real samples.
- `VIEWPORT_INFILL` imagepipeline plugin (`plugins/viewportinfill.*`)
  fill-pulls holes (zero radiance weight, incl. LT-splat coverage) from
  a pull-push pyramid on the linear IMAGEPIPELINE buffer, pre-tonemap.
  Engine-agnostic (reads film channels) → covers RTPATHOCL, PATHOCL+LT,
  RTPATHCPU, PATHCPU, BIDIRCPU viewports when the adapter injects it.
- RTPATHCPU first frame: single-pixel writes (fake weight kept), no more
  zoomFactor×zoomFactor block splat.
- Kernel gotcha: `(ulong)i * A` needs 64-bit math for exactness (uint
  wraps at i≥2); `ulong` is fine through cl2msl (precedent: cloth TEA).
  Scalar `min`/`max` on uints are #defined to metal::min/max by the
  preamble — `Max/Min` C++ helpers do NOT exist in kernel code.
- Validation: dev-tools/e51_viewport_infill.py — RTPATHOCL 720p first
  pass raw ≈11–16% → infill ≈88%, 16/16 cells; PATHOCL+LT/RTPATHCPU/
  BIDIRCPU invariants (infill ≥ raw, finite, sane).

## Update — adaptive, temporal, smoothing (2026-09)

- `TILEPATHSAMPLER` GPU kernel: `filmNoise`-driven probabilistic skip
  inside `TilePathSampler_Init` rank-1 lattice walk (rejects jittered
  retry until a qualifying pixel or attempt cap; lattice index still
  advances so coverage ordering is preserved). Fields added to
  `slg::ocl::Sampler::tilepath` (adaptiveStrength,
  adaptiveUserImportanceWeight). `sampler.tilepath.adaptive.*` props.
- `RTPATHCPUSAMPLER`: same skip rule against `channel_NOISE` on the
  per-sample walk (floor kept: `max(noise, 1-strength)`).
- `VIEWPORT_TEMPORAL` plugin: snapshots display RGB + `POSITION`
  channel + reprojects each history pixel's world position into the
  current camera whenever `viewport.edit.cameraonly` metadata is 1.
  Camera transforms are serialized into film metadata by
  `RenderSession::PublishViewportCamera()` at Start/EndSceneEdit —
  metadata survives `Film::Reset()`.
- `VIEWPORT_SMOOTH` plugin: à-trous (steps 1/2/4) edge-aware filter on
  pixels with radiance weight < minsamps; converged pixels passthrough.
- Gotchas discovered:
  - `scene.Parse("scene.camera.*")` does NOT register CAMERA_EDIT —
    only `Camera::Translate/Rotate*` via `scene.GetCamera()` (the path
    Blender uses). Property-level camera edits therefore skip the fast
    path AND temporal reuse.
  - `sampleResult->depth` is a ray parameter, not camera-space
    distance: don't unproject with it, use the `POSITION` channel +
    `Camera::ProjectPointToFilm` math instead.
  - PATHOCL kernels never write `sampleResult->depth/position` →
    DEPTH/POSITION are all-inf on that engine; temporal/smooth
    silently no-op there. RTPATHOCL/RTPATHCPU/PATHCPU/BIDIRCPU fill
    them (98%+ finite in cornell).
  - Updating the history camera tag unconditionally on every Apply is a
    bug: the first post-edit Apply rewrites it before the film reset
    lands, disabling the warp exactly when needed. World positions are
    camera-independent, so no history camera needs storing at all.
- `stats.renderengine.pass.eye` / `.light` separate eye and light
  passes; light samples go to the screen-normalized channel.
