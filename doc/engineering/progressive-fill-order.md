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
