# Statistical adaptive error / noise-level halt (E5)

**Status:** CPU + GPU | **Test:** `dev-tools/e52_adaptive_noise_test.py` |
**Blender:** Halt Conditions → "Use Noise Level"

A single-knob, Corona-style noise-level halt and adaptive-sampling map.
The artist sets a tolerated relative pixel error (e.g. 3%); the film
computes a statistical per-pixel error estimate from data it already
accumulates, builds a global noise level, feeds a done/importance map
into the NOISE channel for the adaptive samplers, and stops rendering
when the target is reached — on CPU and GPU identically.

## Algorithm

Per pixel `i` with `n` samples (SAMPLECOUNT), mean `mu` from
`RADIANCE_PER_PIXEL_NORMALIZED` and second moment `E[c²]` from the
VARIANCE channel:

```
relErr(i) = sqrt( max(E[c²] - mu², 0) / n ) / (|mu| + eps)
```

— the relative standard error of the pixel mean (Kulla et al. ToG'18,
the Arnold recipe). The map is then:

1. **3×3 max dilation** — a pixel only counts as converged when its
   neighborhood is too; catches sub-pixel detail hidden in smooth
   averages.
2. **Global noise level** — 95th percentile of the dilated map over
   evaluated pixels (pixels under `minsamples` excluded from the
   percentile but counted as not converged).
3. **Halt** — `noiseLevel <= target` sets `statsConvergence = 1`.
   Progress meanwhile is `convergedRatio` = fraction of pixels under
   target, so the progress bar is meaningful.
4. **NOISE channel write** — `clamp(relErr / target, 0, 1)` replaces
   the image-diff heuristic map, so samplers (`sampler.*.adaptive.*`)
   automatically steer samples at the statistically-grounded residual
   error. When enabled, the adaptive error test is the NOISE writer of
   last resort in `RunTests()` (runs after `FilmNoiseEstimation`).

## Why this design

- **No new GPU plumbing.** The estimator reads VARIANCE + SAMPLECOUNT +
  RADIANCE_PER_PIXEL_NORMALIZED, all already accumulated by the GPU
  kernels and merged into the host film. CPU/GPU parity is structural,
  not reimplemented.
- **Statistically grounded.** Unlike the image-difference heuristic
  (`FilmNoiseEstimation`), the variance of the mean is a real error
  bound — it converges monotonically ∝ 1/√n and can't be fooled by a
  noise floor in the tonemapped image.
- Inspired by StatMC (SIGGRAPH Asia'24) / StatER (SIGGRAPH Asia'25)
  "estimate the error, sample where it is" framing, adapted to the
  film channels LuxCore already has.

## Properties

| Property | Default | Meaning |
|---|---|---|
| `film.adaptiveerror.target` | 0 (off) | Relative error target, e.g. `0.03` = 3% |
| `film.adaptiveerror.warmup` | 8 | Min avg spp before first test |
| `film.adaptiveerror.step` | 16 | Min avg spp between tests |
| `film.adaptiveerror.minsamples` | 4 | Per-pixel n floor for a finite estimate |
| `film.adaptiveerror.halt.enable` | true | Halt when noiseLevel ≤ target |

Setting a target > 0 auto-requests `VARIANCE`, `SAMPLECOUNT` and
`NOISE` channels (no manual film config needed).

New public stat: `stats.renderengine.noiselevel` (float, NaN when the
test is disabled).

## Blender

Render Properties → Halt Conditions → **Use Noise Level**: percent
target plus warmup/step. Exported as `film.adaptiveerror.*` above.

## Validation

`dev-tools/e52_adaptive_noise_test.py` (cornell, 128²):

- PATHCPU + PATHOCL, target 60% → halt in ~2 s on both (parity).
- Target 0.2% → no halt inside the time cap; noiseLevel decreases
  monotonically (39.7% → 5.5% over 240 s on cornell).
- NOISE channel finite, spatially varying.

## Limitations

- Relative-error normalization divides by pixel luminance; genuinely
  dark pixels (laser/skybox-less blacks) are treated as converged via
  `mu + eps` — a luminance floor is implicit in `eps`.
- Firefly-clamped or bias-corrected radiance shifts `mu` but variance
  is measured pre-clamp in some accumulation paths — the estimate is
  slightly conservative, which is the safe direction.
- 95th-percentile halt tolerates ~5% pixels above target; scenes with
  unresolved SDS paths will (correctly) refuse to halt at tight
  targets — pair with PhotonGI caustics to make those paths converge.
