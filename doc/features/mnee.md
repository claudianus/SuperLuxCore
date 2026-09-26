# MNEE — Manifold Next Event Estimation (specular-chain solver)

Status: implemented. Newton-iteration specular-chain solver for hard caustics,
single + multi-specular chains, on CPU and GPU (OpenCL/Metal) kernels.

## What and why

Specular-only light paths (caustics through glass, mirror chains) are nearly
impossible to sample by ordinary path tracing because the connection direction
must satisfy refraction/reflection constraints at every vertex. MNEE treats
finding a valid specular chain as a **root-finding problem on a manifold**: it
places a specular chain between a shading point and a light, then iterates a
half-vector / Newton step until the half-vector constraint holds at every
vertex. This converts "invisible" caustics into an estimable connection.

## References

- Hanika, Droske, Fascione. **Manifold Next-Event Estimation.** Computer
  Graphics Forum (EGSR) 34(4), 2015. (The half-vector / Newton manifold walk.)
- Zeltner, Georgiev, Jakob. **Specular Manifold Sampling for Rendering
  High-Frequency Caustics and Glints.** SIGGRAPH 2020. (Production-grade
  variant; our chain solver is a MNEE-family implementation.)
- Jakob, Marschner. **Manifold Exploration: A Markov Chain Monte Carlo
  Technique for Rendering Scenes with Difficult Specular Transport.** SIGGRAPH
  2012. (Underlying manifold framework.)

## Implementation

- `slg/bsdf/manifold*` — Newton solver: builds a specular chain between the
  shading point and a sampled light point, iterates until half-vector
  constraints hold (within a tolerance) or rejects.
- Single-specular (`direct light through delta specular`) and multi-specular
  chains (`multi-specular chain solver`) supported; closed glass-slab and
  glass-ball caustics verified at CPU parity.
- GPU kernels mirror the CPU solver (Metal + OpenCL); `P1-2: MNEE specular
  chain solver in the GPU kernels (Metal)`.
- Correctness guards: reject the unphysical opposite-side mirror case, apply
  the light-weight `r12^2` measure factor only to the plain half-vector term,
  guard the optional Jacobian output.
- Instrumentation (default-off): `LUX_MNEE_ITER`, `LUX_MNEE_REJX` dump chain
  iterations / rejection reasons for debugging.

### Manifold seed cache (GPU, E4)

Converged single-vertex solutions are stored in a fixed-size hashed grid
(`mneeSeeds`, 16384 x 32B) keyed by (light, occluder mesh, quantized
shadow-ray occluder hit position). A later attempt blocked by the same
occluder region warm-starts Newton from the cached vertex: for mirrors this
skips the mirrored-light seed trace entirely; for glass it replaces the
line seed. The seed only selects the Newton basin - the solver still
verifies the half-vector constraint on re-projected surface vertices, so a
stale or colliding entry costs iterations but never biases the result.
Opt-out with `path.mnee.seedcache = 0` (default on; GPU only, CPU path
unchanged).

Validated (`dev-tools/e17_mnee_seedcache_gpu_test.py`, glass + mirror
occluders, 8/8): unbiased vs uncached, caustic-region mean preserved
(ratio 0.999-1.001), on/off pixel differences confined to caustic pixels
(legitimate solution-basin changes, not noise).

### Properties

- `path.mnee.enable` (default off) — specular-chain direct light sampling.
- `path.mnee.maxiterations` (default 12) — Newton/line-search bound.
- `path.mnee.maxspecular` (default 1) — 2..4 enable multi-specular chains.
- `path.mnee.seedcache` (default on, GPU only) — manifold seed cache.
- Exposed to Blender via `MNEE specular caustics` option (`P1-2: expose MNEE
  specular caustics option`).

## Test scenes / validation

- `scenes/causticcube/`, glass slab / glass ball caustics — rendered at CPU
  parity on Metal GPU.
- `scenes/juice/test.scn` + `test-mirror.scn` — point-light + glass/mirror
  occluder seed-cache coverage (`e17_mnee_seedcache_gpu_test.py`).
- `dev-tools/mnee_design.md` — internal design notes.

## Platforms

CPU, OpenCL GPU, Metal GPU.
