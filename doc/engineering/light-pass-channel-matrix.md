# Light-pass channel & capability matrix

The hybrid/light-tracing stack only works when **three** pieces line up:
eye-side caustic suppression, a light-path sampler, and a film channel
the splats can land in. Auto-routing (`path.lighttracing.auto`,
`path.mnee.auto`) turns the first two on without ever writing
`path.hybridbackforward.enable` into the config — every reader that keys
on the raw flag instead of the resolved state silently breaks the chain.

## Suppression → deposit contract

`PathTracer::RenderEyePath` suppresses caustic-class contributions when
`hybridBackForwardEnable` is set (promoted from `lightTracingEnable` or
`vertexConnectionEnable` in `ParseOptions`). Whatever is suppressed must
be deposited by a light pass into `RADIANCE_PER_SCREEN_NORMALIZED`
(Metropolis splatter). If suppression is on but no splatter channel or
no light sampler exists, the caustic class vanishes → black image.

| Engine | Light tasks | Screen-normalized channel | auto-LT allowed |
|---|---|---|---|
| PATHCPU / RTPATHCPU | `PathCPURenderThread` lt pass | `pathcpu.cpp` InitFilm | yes |
| PATHOCL | GPU task tail / native threads | `pathoclbase.cpp` InitFilm | yes |
| TILEPATHOCL | GPU task tail / native splatter | `pathoclbase.cpp` InitFilm | yes |
| TILEPATHCPU | **none** | **never allocated** | **no** |

## Fixed bugs (this finding)

1. **PATHCPU/RTPATHCPU black image under auto-LT** —
   `PathCPURenderEngine::InitFilm` gated
   `RADIANCE_PER_SCREEN_NORMALIZED` on raw `path.hybridbackforward.enable`
   only. Auto-LT injects `lighttracing.enable` but leaves HBF unset, so
   CPU light splats had no destination channel. Now keys on
   `hbf || lt` (same condition `pathoclbase.cpp` already used).

2. **TILEPATHCPU in the auto whitelist** — `tilepathcputhread.cpp`
   runs eye sampling only; there is no light sampler, no splatter, and
   `InitFilm` only adds `RADIANCE_PER_PIXEL_NORMALIZED`. Auto-LT there
   meant pure suppression with zero deposit. Removed from
   `ApplyAutoLightTracing` (MNEE auto stays — it is an eye-side solver),
   plus a defensive warning+ignore for an *explicit* lt/hbf request in
   `TilePathCPURenderEngine::StartLockLess`.

## Gotchas

- Resolved flags (`pathTracer.lightTracingEnable` etc.) live on the
  `PathTracer` member after `ParseOptions`; raw `cfg` reads see the
  pre-promotion values. `InitFilm` runs **before** `ParseOptions` in
  some engines — prefer the auto-injected cfg keys, not member state.
- `pgic.caustic.enabled` is **not** a light-pass substitute for the
  zero-tail fallback: under HBF the caustic cache is only applied at
  `depth != 0` (`pathtracer.cpp`), so the depth-0 caustic pool still
  belongs to the light pass. Native threads are the only compensation.
- `taskCount <= 8192` ⇒ `lightTaskCount == 0` (8192-chunk rounding).
  PATHOCL demotes to native-thread HBF; TILEPATHOCL native threads run
  the splatter so no demotion is needed.
- GPU-only + zero tail + no PGIC ⇒ LT/HBF/VC all cleared, plain PT.

## Tests

- `dev-tools/e102_caustic_routing_matrix_test.py` — caustic/diffuse ×
  PATHOCL/PATHCPU × auto/explicit/zero-tail/PGIC matrix.
- `dev-tools/e17_mnee_seedcache_gpu_test.py` — T-1 zero-tail demotion.
