# Light-strategy audit (E37)

> 갱신(2026-09-25): 이 감사 이후 **`LIGHT_BVH`**가 6번째 전략으로 추가됨
> (`6c0006bc4ab8fc8a6139873ba6160812290b448d` — E&K'18 binned-SAH light tree; AGENTS.md §Light BVH 참조).
> 아래 "정확히 5개" 서술은 감사 시점 기록.

Full audit of every registered light strategy — `UNIFORM`, `POWER`,
`LOG_POWER`, `DLS_CACHE`, `RESTIR_DI` — across the CPU path tracers and
the GPU (OpenCL/Metal) kernels. Focus: sampling/PDF consistency, MIS
pairing, CPU/GPU semantic parity, numerical robustness, and bias.

## Key architecture

- `LightStrategyRegistry` registers exactly the five strategies above.
- `LightSources` keeps three strategy instances: emit, illuminate,
  infinite-only. `TASK_ILLUMINATE` excludes lights with
  `IsDirectLightSamplingEnabled() == false`; `TASK_INFINITE_ONLY`
  includes only infinite lights.
- `DLS_CACHE` derives directly from `LightStrategy` (NOT from
  `DistributionLightStrategy`) — it does not share the distribution
  base-class cast path.
- ReSTIR keeps its proposal PDF as the light-selection `q`; the RIS
  factor travels separately via `risScale`. Direct-hit MIS must use the
  same strategy/distribution that the NEE draw used at the previous
  vertex.
- DLSC cache entries are keyed on the landing SHADE normal
  (`HitPoint::GetLandingShadeN`), not the geometric normal.

## Fixes (B1-B10)

| ID | File | Fix |
|---|---|---|
| B1 | `compilelights.cpp` | Infinite-strategy DLSC cast used `illuminateLightStrategy` — now casts `infiniteLightStrategy`, so `DLS_CACHE` compiles the correct infinite distribution on GPU |
| B2 | `pathoclbase_funcs.cl` | Shadow-catcher `onlyinfinitelights` vertices now bypass the DLSC lookup (the cache never covers the infinite distribution) |
| B3 | `pathoclbase_funcs.cl` | GPU DLSC sampling/PDF queries use `BSDF_GetLandingShadeN` (was geometric normal — could sample and MIS-evaluate different cache entries) |
| B4 | `pathinfo.*`, `pathtracer.cpp`, `pathoclbase_funcs.cl` | New `lastOnlyInfiniteLights` flag on `EyePathInfo`; `DirectHit*` MIS selects the same strategy the previous vertex's NEE used, and the DLSC zero-PDF firefly guard no longer drops hits that lack infinite-only coverage |
| B5 | `lightstrategy_funcs.cl` | Null distribution returns `0.f` instead of `NULL_INDEX` (a huge unsigned sentinel corrupting MIS weights) |
| B6 | `restirdi.cpp` | Non-BSDF `SampleLights` delegates to log-power — the flat-target reservoir (w = 1/q) converged to a uniform pick while paying candidateCount evaluations |
| B7 | `pathoclbase_funcs.cl` | GPU GRIS spatial-merge weight unclamped (CPU parity): capping `pi_new/pi_old` is a data-dependent weight distortion that biases dark |
| B8 | `dlscacheimpl.cpp` | `pass + 1` into `RadicalInverse` (pass 0 is the (0,0,0) corner) and `receivedLuminance / samplesTaken` (was `/ pass`: divide-by-zero on pass 0, mean overestimate on early break) |
| B9 | `dlscacheimpl.cpp` | Persistent cache load validates each entry's distribution count against the scene light count — a mismatched file indexed out of range |
| B10 | `restirgi.cpp`, `bidircputhread.cpp` | Light sampling uses the landing shade normal (was geometric) — matches the DLSC key convention |

## Numerical guards

`UNIFORM`, `POWER`, `LOG_POWER` now reject NaN / negative / +Inf
distribution weights: NaN poisons the whole CDF, and +Inf makes
`funcInt` infinite so `func *= 1/funcInt` zeroes every light.

## Unbiasedness notes

- A data-dependent merge gate (clamp OR skip on the GRIS ratio) is
  biased: inclusion correlates with the candidate's weight. The
  principled outlier defence is the representative-winner gate
  (winner target >= 5% of wSum/M), which both sides already apply.
- `DirectHit` firefly guard (`pdf == 0` drop) is only valid when the
  NEE distribution could have picked the light — under the
  infinite-only restriction a finite light's pdf is structurally 0 but
  the BSDF hit is the sole covering technique, so it must keep
  weight 1.

## Regression

`dev-tools/e37_lightstrategy_audit.py` renders a
shadowcatcher.onlyinfinitelights + infinite + finite light scene on
PATHCPU and PATHOCL for all five strategies and checks finite output,
dead-pixel share, GPU/CPU luminance ratio and RMSE.

    python3.13 dev-tools/e37_lightstrategy_audit.py
