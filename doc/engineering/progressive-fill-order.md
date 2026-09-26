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

