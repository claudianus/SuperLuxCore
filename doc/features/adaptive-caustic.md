# Adaptive Caustic Partition — connection-difficulty path classification

Status: **functional on CPU (`PATHCPU` hybrid) and GPU (`PATHOCL` +
`path.lighttracing.enable`, Metal-validated)** — regression coverage in
`dev-tools/e25_adaptive_caustic_test.py`, scene in
`scenes/cornell/caustic-roughglass.scn`.

## What and why

Hybrid back/forward rendering (`path.hybridbackforward.enable`) splits the
path space between the eye pass and the light pass: the light pass owns
"caustic-class" paths and the eye pass must not count them, otherwise the
estimator double-counts. The legacy classifier
(`path.hybridbackforward.glossinessthreshold`, default 0.05) marks a path
caustic-class when every vertex between the receiver and the light is
nearly-specular — a fixed material constant that ignores how hard the
connection actually is.

The failure mode is the threshold boundary: a rough-glass object at
glossiness 0.1 sits above 0.05, so the eye pass owns its caustics. But a
0.1-lobe BSDF sample almost never lands on a small emitter, so the caustic
shows up as rare, huge-contribution fireflies — precisely the paths the
light pass renders cheaply.

The adaptive partition replaces the fixed material test with a
per-connection difficulty estimate:

```
hard for the eye path  <=>  omegaLight < connectProb * omegaLobe
                             omegaLobe = PI * glossiness^2
```

where `omegaLight` is the solid angle the emitter subtends at the
light-adjacent vertex (`LightConnectionSolidAngle()` on CPU,
`Light_ConnectionSolidAngle()` on GPU). Delta terminals are always hard
(eye BSDF sampling can never hit them); glossy terminals are hard when the
light covers a small fraction of the lobe.

## Partition contract

Both sides evaluate the *same* predicate on the *same* path class, which is
what keeps the partition disjoint and the estimator unbiased:

- Eye side (`EyePathInfo::isAdaptiveCaustic`): the camera-adjacent receiver
  must be non-delta; every later vertex must be non-diffuse
  (`SPECULAR | GLOSSY` — a widened chain that also admits boundary-glossy
  interior vertices). At a direct emitter hit or an NEE connection, the
  terminal vertex's delta/glossiness and the emitter's solid angle decide
  suppression (`IsAdaptiveCausticPath` / `IsAdaptiveCausticHitPath`).
- Light side (`LightPathInfo::isAdaptiveS` + `firstVertex*`): the chain
  accumulates identically (all non-diffuse), the connect-to-eye receiver
  must be non-delta, and the same terminal test runs against the *first*
  (light-adjacent) vertex and the emitting light. Connections that pass are
  marked `isCaustic` and splat; on CPU the Metropolis `addonlycaustics`
  contract consumes that flag directly.
- CPU light paths also use `isAdaptiveS` as the path-continuation gate (the
  old `IsSpecularPath()` gate broke rough-glass chains before they could
  reach a receiver).

`omegaLight` is deterministic per (vertex position, light) — triangle
lights use `area * cos(theta) / d^2` from the triangle centroid, sun and
distant lights use their cone solid angle, environment lights are treated
as infinite (always easy for the eye path: a terminal delta still counts
because a delta terminal is hard regardless), and positional emitters
(point/spot/projection/laser) report 0 — they are covered by direct light
sampling and can never be hit by BSDF sampling, so they are *not* treated
as eye-hard.

## Configuration

| Property | Default | Meaning |
|---|---|---|
| `path.hybridbackforward.adaptivecaustic` | true | Use the adaptive classifier instead of the fixed glossiness threshold for the hybrid partition |
| `path.hybridbackforward.terminalglossiness` | 0.3 | Glossiness limit for the light-adjacent vertex; rougher terminals stay eye-owned |
| `path.hybridbackforward.connectprob` | 0.5 | Eye-connection success probability below which the light pass takes over (`omegaLight / omegaLobe`) |

SuperBlendLuxCore exposes all three under *Render Layers > Light Tracing* as
"Adaptive Caustics" (toggle), "Terminal Glossiness" and "Connection
Probability"; disabling the toggle reveals the legacy "Glossiness
Threshold" control.

## Validation

`dev-tools/e25_adaptive_caustic_test.py` renders
`scenes/cornell/caustic-roughglass.scn` (roughglass sphere, uroughness 0.1,
small emissive ceiling panel) at 320x180:

- bias: adaptive OFF vs ON patch means agree within noise (the OFF render
  under-covers the class at finite spp, so ON reads slightly higher — the
  expected convergence advantage, not a leak: the CAUSTIC channel shows the
  class fully moved to the light pass while the eye pass no longer produces
  its firefly tail),
- firefly: patch p99/max do not increase with adaptive ON,
- parity: `PATHOCL` (GPU light tasks) vs `PATHCPU` patch means agree within
  noise (measured 1.05).

`dev-tools/e25_render_compare.py` renders the same scene at 1280x720 with
the classifier OFF/ON and writes tonemapped PNGs plus raw HDR patch
statistics to `out/e25/`. `dev-tools/e25_caustic_channel.py` dumps the
`FilmOutputs::CAUSTIC` view (the `RADIANCE_PER_SCREEN_NORMALIZED` light-pass
splat buffer), which is empty under the fixed threshold and shows the
through-glass transport under the adaptive partition.

### Gotchas discovered during validation

- On PATHOCL the light task population is carved out of
  `opencl.task.count` with an 8192-task eye-pass floor
  (`lightTaskCount = Min(taskCount - 8192, RoundUp(taskCount * f, 8192))`);
  a small `opencl.task.count` silently disables the light pass. The
  regression test uses 65536.
- `GetFilm().GetOutputFloat()` reads whatever the last film merge produced;
  on GPU engines the merge runs inside `UpdateStats()`/`Stop()`. Read film
  outputs after `ses.Stop()` (or after an `UpdateStats()`) or the
  screen-normalized channel lags behind the pass counter.

## References

- Veach & Guibas, "Bidirectional Estimators for Light Transport" (1997) —
  the estimator-partition framing.
- Hachisuka & Jensen, "Stochastic Progressive Photon Mapping" / the hybrid
  back/forward scheme LuxCore implements (`glossinessthreshold` split).
- Hanika, Droske & Fascione, "Manifold Next-Event Estimation" (2015) — the
  complementary solver for the delta-terminal connections the partition
  still cannot connect; MNEE paths are always caustic-class and stay
  light-owned.
