# Performance ledger — ranked bottleneck book

Living document for the measurement-gated optimization cycle defined in
`dev-tools/megaplan-production-sota.md`. Each entry records: hotspot,
call path, measurement source + share, hypothesis, correctness
constraints, A/B result, decision, follow-up.

Rules:
- Measure on an idle host (gauntlet prints load warnings; `uptime`
  loadavg should be roughly < core-count before trusting timings).
- Interleaved A/B, min-of-3; a <2% delta is noise unless confirmed on
  repeated runs.
- Anything touching sampling weights / pdfs needs e26-style
  unbiasedness + cpu/gpu parity gates, not just timing.
- Bit-identical transforms are preferred; statistically-identical
  transforms need a stated justification.

## CPU render-thread profile (2026-09, PATHCPU, Apple Silicon)

Sorted by profiled share of render-thread time.

| # | Hotspot | Share | Status | Commit |
|---|---------|-------|--------|--------|
| 1 | Sampler hashing (Sobol/Metropolis) | ~6% | landed | `77368bfc6` |
| 2 | PhotonGI retrace worker contention | ~5.5% | landed (threads knob + saturation backoff) | `023952789`, `8f52f6ffa` |
| 3 | libm transcendentals on hot callers | ~2.5% | landed | `cd568c328` |
| 4 | Spectral `ProjectToRGB` per-field CIE/whitepoint | ~1.5% | landed | `e25c8ffc6` |
| 5 | `DataSet::GetAccelerator` per ray segment | ~0.3% | landed | `e25c8ffc6` |
| 6 | LightBVH `NodeImportance` trig chain | ~0.1% | landed | `e25c8ffc6` |

## CPU render-thread profile (2026-09-30 r2, PATHCPU, portal-interior 640x360)

Portal-interior (sealed-indirect worst case) re-profile after the first
backlog drained. Shares are top-of-stack on render threads.

| # | Hotspot | Share | Status | Commit |
|---|---------|-------|--------|--------|
| 1 | Embree tri+instance traversal | ~20% | natural cost | — |
| 2 | `SobolSampler::InitNewSample` geometry+adaptive | ~11% | **landed** (film cache + magic udivs, -43% leaf) | `537a48c6d` |
| 3 | `SobolSequence::GetSample` ctz-walk | ~8% | residual | — |
| 4 | `MetropolisSampler::GetSample/NextSample` | ~8% | residual (MLT mutation loop amortized) | — |
| 5 | `sincosf` latlong/disk/cone sampling | ~6% | no redundant calls identified; approx breaks parity | — |
| 6 | HitPoint attr chain (`Init`+`GetDifferentials`+interpolate+Buffer[] stubs) | ~9% | open | — |
| 7 | PathVolumeInfo bookkeeping | ~3% | open (has-volumes fast gate) | — |

### r2 #2 Sobol InitNewSample film cache (`537a48c6d`)

- Path: `SobolSampler::InitNewSample` per eye sample — adaptive
  convergence test (NOISE channel + second moments) + bucket/pixel
  arithmetic.
- Observation: per sample paid 2 `std::set::count` channel lookups,
  ~8 runtime `udiv`s (tileSize² mod/div, tiletWidthCount mod/div,
  overlapping div), dead `GetEngineFilm()` ref.
- Change: `UpdateFilmCache()` snapshots subregion-derived geometry,
  magic divisors and channel flags once per subregion change (channels
  frozen post-`Film::Init`; only subregion can still move via dyn-res).
  `floor(2^32/d)+1` mulhi division is exact for every u32 dividend —
  pixel visit order and RNG draws bit-identical.
- Validation: Release build; `parity-regression` 4/4 on final binary;
  portal-interior smoke render clean.
- Result: leaf 42.7k → 24.5k (-43%), sampler group ~104k → ~73.7k
  (-29%) at identical 25s `sample` window.


- GPU crawl-bail for the wavefront MNEE state machine — corrupts state
  when bailing mid-phase; rule: early exits only at valid phase
  boundaries (`ab7e944e6`, see `megaplan` + session log).
- Candidate-prefilter round — measured no-win (`24599e018`).

## Entry details

### #4 Spectral RGB projection (`e25c8ffc6`)

- Path: `PathTracer::ProjectSampleResultToRGB` → `Spectral::ProjectToRGB`
  × ~15 `SampleResult` fields.
- Observation: per field the old code re-sampled 9 CIE SPD lookups and
  re-normalized the sampled white point — all functions of the drawn
  wavelengths only.
- Change: `Spectral::RGBProjector` prepared once per sample
  (`PrepareRGBProjection`), per-field projection reduced to dot +
  `ToRGB`; black-spectrum early-out is exact (`ToRGB(0)=0`).
- Constraint: `nY<=0` invalid path preserved (zero spectrum, no early
  return skipping field init); `aliveMask` respected inside the
  projector.
- Validation: Release build, parity-regression 4/4, spectral render
  mean/luminance unchanged.

### #5 `Scene::Intersect` accelerator lookup (`e25c8ffc6`)

- Path: CPU `Scene::Intersect` `for(;;)` shadow-transparency loop.
- Observation: `dataSet->GetAccelerator(ACCEL_EMBREE)` is a
  `std::map::find` executed per ray *segment* — multi-segment hits
  (glossy transmittance, volumes) multiplied the lookup.
- Change: resolve once per call before the loop. No semantic change.

### #6 LightBVH `NodeImportance` dot-space (`e25c8ffc6`)

- Path: light sampling `SampleLights`/`SampleLightPdf` descent,
  CPU `NodeImportance` + GPU `LightBVH_NodeImportance`
  (`lightbvh_funcs.cl`) — kept identical by contract.
- Observation: per evaluation `asin`+2×`acos`+2×`cos` (~5 libm calls).
- Change: `thetaO` cos/sin baked into `LightBVHNode` at build (+8B/node).
  `cos(max(0,acos(d)-t)) == 1` when `d >= cos(t)`, else
  `d·cos(t) + sqrt(1−d²)·sin(t)`; `thetaO+thetaB >= PI` cover-case
  detected as `cO <= 0 && sB >= sO` (sin monotonic on [π/2,π] domain,
  handles omni `thetaO=PI`). Zero transcendentals per call now.
- Constraint: E&K'18 bound must stay conservative — the identity is
  exact, the `cosO >= cBO` gate preserves the inside-cone → 1 case, and
  the `Max(0,·)` wrap preserves the >π/2 → 0 clamp.
- Validation: `e26_lightbvh_test.py` 10/10 — unbiasedness vs LOG_POWER,
  bounded RMSE (0.0017 < flat 0.0027), cpu-gpu parity, finite outputs.
- Timing: ~0.1% profile share, below run-to-run noise without idle
  host; landed on correctness-neutral cost-reduction grounds.

## Backlog (ranked candidates for next rounds)

From gauntlet v2 + audit (`dev-tools/sota-acceleration-audit.md`):

- Sampler hashing residual (~6% → post-`77368bfc6` residual share
  needs re-measure on idle host).
- PhotonGI retrace CPU worker — consider SIMD / batch bvh queries;
  verify `LUX_PGIC_UPDATE_THREADS` tuning default.
- Wavefront queues: still opt-in/auto-off; re-evaluate per-workload
  after queue-fill improvements (see `wavefront-queues.md`).
- GPU PhotonGI deposits → full GPU cache update path (B-series).
- Spectral: 4-bin → wider coverage / n,k table-driven IOR; M6 item.
- `maxDepth`/device-scheduling heuristics for auto mode (M4 gate).
