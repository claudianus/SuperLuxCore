# VC pool multiplicity bias — thebox4 investigation

Root-cause writeup of the GPU-vs-CPU brightness bias reported on
`b3d/thebox4/Untitled.blend` (artist production scene, ~1.9M tris,
2 lights, OpenPBR/matte materials). Symptom: `PATH(OCL)`/RTPATHOCL
output was ~1.5-1.6× brighter than `BIDIRCPU`/`PATHCPU` (which agreed
within ~5%), uniformly across the image — not fireflies, not spectral,
not tonemapping.

## Isolation path (all on luxcoreconsole, same .scn/.cfg, 1280×720)

| Config | Mean RGB vs `lt_off` baseline |
|---|---|
| `lighttracing.enable=0, vc=0` (GPU) | 1.000 (== PATHCPU lt_on) |
| `lighttracing.only=1` | 1.000 — raw light splats fine |
| `vc=1, lt=0` ("vconly") | +27.7% — excess lives in MK_VC_CONNECT |
| `vc=1, mergeradius=0` | +27.7% — vertex merge contributes ~0 |
| `vc=1, pool=1` | +10.8% |
| `vc=1, pool=4` | +27.7% |
| `vc=1, pool=16` | +74.5% — excess scales ~linearly with pool |

## Root cause

The candidate pool in `AdvancePaths_VCConnect` (`vcPoolTasks` light
tasks' vertex caches) is P independent light subpaths. Each subpath's
connect sum `S_j = Σ_t w_t c_t` is a complete unbiased BDPT-style
estimate of all connect strategies — the estimator is the *average*
over subpaths `(1/P)Σ_j S_j`, not the sum. The kernel summed all
included candidates with the per-pair MIS weight only, so every
connect strategy was inflated by ~P×. `connects>0` Horvitz–Thompson
subsampling only changes which candidates are *evaluated* — its target
is still the full pool sum, so it did not hide the bug.

Residual at pool=1 was the M7d `reuse` reservoir: the replayed vertex
adds one extra sample to its (eye depth, light depth) technique bucket
that nothing divides — and it is argmax-selected (winner's curse), so
its mean is biased upward as well.

## Fix

`pathoclbase_kernels_micro.cl` `AdvancePaths_VCConnect`:

- `vcReplayDepth` = stored replay vertex depth.
- Per-candidate divisor `vcDiv = vcPoolTasks + (vcHasReplay &&
  lv->depth == vcReplayDepth ? 1 : 0)` applied as
  `pending /= vcQ * vcDiv`.

Subpath-average (÷P) is exact even when subpaths are truncated below
`slotsPerTask` — missing slots are draws that contribute 0 but still
count in P. A per-light-depth histogram would be *wrong*: conditioning
on "vertex exists at depth t" changes the draw's conditional mean
(`E[w x | exists] = I_t / P(exists)`), so it over-corrects.

## Measured after fix (same scene, 64spp)

| Config | ratio vs lt_off |
|---|---|
| pool=1, reuse=0 | 1.014 |
| pool=4, reuse=0 | 1.014 |
| pool=4, connects=0, reuse=0 | 1.011 |
| pool=4, reuse=1 (full default) | 1.027 |

The residual +1-3% is within 64spp MC noise plus the documented
winner's-curse term of the adaptive replay reservoir.

## Gotchas worth remembering

- "Each connect is a valid strategy sample" is true per-pair but false
  as a *sum* over independent subpaths — multiplicity must be
  normalized.
- `batch.haltspp` overshoots on GPU VC scenes (the sample counter mixes
  eye+light task iterations while the halt counts film samples) — a
  "256spp" run takes ~9 min wall on this scene at ~0.75M samples/sec.
- The Blender adapter's GPU `PATH` exports `RTPATHOCL`, which shares
  `EnqueueAdvancePathsKernel` — VC/light-task kernels run there too,
  so this fix covers the interactive path.
- Merge (`mergeradius > 0`) is a different estimator family (VCM
  density estimate, normalized by `vmNorm`/`nVM`) and was not part of
  the bug.

## Test

`dev-tools/e100_vc_pool_parity.py [metal|opencl]` — pool=1 vs pool=8
mean-luminance ratio on `cornell-area-caustic.scn`, PASS band
0.97-1.03. Reproduces the bug deterministically if the normalization
regresses.
