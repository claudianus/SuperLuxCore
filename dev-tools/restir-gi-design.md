# ReSTIR GI / PT — E2 continuation design

Status: **G1 + G1-b CPU + G2 GPU implemented** —
`src/slg/engines/restirgi.cpp` (PATHCPU) and the `pathoclbase` kernel
tail (PATHOCL/TILEPATHOCL, OpenCL + Metal) share the same estimator and
`path.restir.gi.*` properties. e19 covers both backends (10/10).
Roadmap item E2 (the "ReSTIR PT/GI/PG" remainder after E2a–E2d
landed). This document fixes the scope, estimator math, integration
points and validation plan so the implementation lands in reviewable
chunks like the DI stages did.

## What / why

ReSTIR DI (shipped) resamples *which light* a direct-lighting estimate
uses. ReSTIR GI resamples *which second-bounce vertex* an indirect
estimate continues through: at a primary vertex x1 the indirect
contribution is `∫ f_r(x1,ω)·G(x1,x2)·L_o(x2→x1) dx2`, and correlated
reuse of good x2 proposals across pixels collapses variance in
indirect-heavy scenes exactly like DI collapsed it in many-light
scenes. This is the single largest remaining quality/perf lever in the
roadmap; the DI infrastructure (per-pixel reservoirs, same-surface
gates, reconnection shifts, tail-queue shadow tracing, ratio clamp)
carries over almost verbatim.

## References

- Ouyang et al. 2021, *ReSTIR GI: Path Resampling in Real-Time* —
  secondary-vertex reservoirs + Jacobian-corrected reconnection shift.
- Lin et al. 2022, *Generalized Resampled Importance Sampling:
  Foundations of ReSTIR* — GRIS framework, `b = wSum·(π̂_new/π̂_old)`
  merge weight, proxy-target correctness (target need not equal the
  integrand; W corrects the residual).
- Bitterli et al. 2020 (ReSTIR DI) — reservoir semantics the DI stage
  already implements (`doc/features/restir-di.md`).

## Scope decision

**G1 (this design): first-bounce resampling at depth-0 vertices.**

The path's continuation vertex x2 becomes an RIS-selected sample:
K candidate x2's are proposed (the path's own BSDF continuation is
candidate 0 — its ray is already traced), reservoirs merge temporal +
gated screen-space spatial reuse, and the winning x2's continuation
is scaled by the RIS weight. Properties:

- The target function π̂(x2) is a *proxy*: `f_r(x1)·G(x1,x2)·L̂(x2)`
  where L̂(x2) is x2's direct-lighting estimate (one light sample +
  one shadow ray at x2). The estimator stays unbiased because the
  payoff uses the real continuation `L_real(x2)`: estimate =
  `W·f_r·G·L_real(x2)`, `W = wSum/(M·π̂)`. Since `f_r·G` appears in
  both π̂ and the payoff, the per-path correction reduces to
  `wSum·L_real/(M·L̂)` — a throughput multiplier on the normal path
  continuation. Cheap-target / exact-payoff asymmetry is the GRIS
  property that makes this viable: proxy quality trades variance, not
  bias.

- **Support floor (found during validation, e19)**: a proxy π̂ must be
  nonzero wherever the integrand `f_r·G·L_real` is nonzero. L̂ is a
  one-sample estimate whose binary-V shadow ray reports 0 for
  occluded-but-lit x2's — without a floor those directions could never
  win and their true indirect contribution is dropped (measured −4.5%
  darkening at K=8; at K=1 the all-zero-target redraw instead
  conditioned on "proxy dark" and measured +6%). Implementation adds
  `eps = 5% · max_i L̂_i` to every candidate's proxy, and on the
  degenerate all-zero-target edge returns the fresh winner with the
  plain BSDF payoff (`W = 1/pdfW`) rather than redrawing — both keep
  supp(π̂) ⊇ supp(f·cos) and restored e19's unbiasedness gate
  (K=1 now matches the BSDF baseline to 4 decimal places).

**Deferred (documented, not implemented in G1):**

- Recursive GI (L̂ including x2's own indirect reservoir) — feedback
  risk needs the pre-spatial-store + clamp machinery extended; do it
  after G1 is measured.
- ReSTIR PT (whole-path-suffix reservoirs, multi-vertex reconnection
  shifts) — path-payload storage and multi-vertex Jacobians are an
  order larger than G1.
- ReSTIR PG (volumes) — needs medium reservoirs and a different shift;
  separate design.
- Deeper-vertex GI (depth ≥ 1 x1) — same machinery, but per-pixel
  reservoir semantics assume primary hits for the pixel gate.

## Reservoir layout (as implemented, `RestirGI::Reservoir`, ~64B)

- `x1` hit point + geometric normal (shift source)
- `x2` hit point + geometric normal (Jacobian cosine on shifts)
- `dir` = x1→x2 (or the env-miss direction)
- `L̂(x2)` proxy radiance (3 floats)
- `wSum`, `M`, `target` (=π̂ at store time), `isMiss`

One entry per film pixel — screen-space by construction, so no
same-surface gate is needed for the temporal merge (the pixel's own
x1 is the shift source; J = 1 on static geometry).

## Estimator (G1, per depth-0 vertex)

1. Candidates 0..K-1: BSDF samples → bounce rays → x2 hits (CPU
   traces them inline via the Embree accelerator; the GPU port will
   ride the E2a-style tail queue). Each candidate's π̂ needs x2's
   direct light: one light pick + one shadow ray at x2.
2. `wSum += π̂_i/q_i` per candidate, `M += 1`; merge the pixel's
   temporal reservoir with the **Jacobian-corrected reconnection
   shift**: `b_nbr = wSum_nbr·(π̂_new/π̂_old)·J` where
   `J = (|cos_x2(x2→x1_cur)|/d_cur²)/(|cos_x2(x2→x1_src)|/d_src²)`
   and the reconnected segment x1_cur→x2 is visibility-tested (the
   E2d rule — cheap on CPU). `RESTIR_GI_MAX_TARGET_RATIO = 64` clamps
   the composite ratio, mirroring `RESTIR_MERGE_MAX_TARGET_RATIO`.
3. Winner x2: the path physically continues through it (existing
   ray/next-vertex machinery) with `pathThroughput *= W`
   (`W = wSum/(M·π̂)`, `M` capped at 2K). Everything past x2 (RR,
   depth, MIS on later bounces) is untouched — the reservoir only
   decides *which* x2. `outPdfW = 1/W` is the RIS marginal density
   used for MIS bookkeeping.

Delta BSDF at x1 skips GI (same rule as DI). The estimator never
creates NaN/Inf: empty reservoirs contribute nothing, culled winners
are excluded by `π̂ ≤ 0`, and the all-zero-target edge returns the
fresh winner with its BSDF payoff instead of redrawing (see the
support-floor note above).

## Integration points

GPU (`pathoclbase`):
- New `restirGIReservoirs` buffer, `filmWidth·filmHeight` entries —
  same allocation site as `restirReservoirsBuff`.
- Candidate bounce rays + x2 shadow rays extend the `rays[]`/`rayHits[]`
  tail (E2a pattern); a resolve state analogous to `MK_RT_RESTIR`
  evaluates π̂ (material at x2 + one NEE shadow ray) after the trace.
- Hook: `MK_HIT_OBJECT` at `depth==0` enqueues GI candidates instead
  of blindly continuing; the resolved winner sets the next-vertex ray
  and the `W` throughput multiplier.
- cl2msl: no new machinery — same tail-queue and resolve-state pattern.

CPU (`pathtracer.cpp` / `LightStrategyRestirDI`-adjacent):
- `RenderEyeSample` knows `sampleResult.pixelX/Y` → per-pixel CPU
  reservoirs are addressable without pixel-context plumbing (the DI
  world-grid restriction does not apply here — GI reservoirs are
  inherently screen-space).
- Synchronous candidate shading: extra BSDF samples + `scene.Intersect`
  calls inline (the CPU visibility path already traces inline).

BlendLuxCore: `restir_gi_enable` checkbox beside the existing ReSTIR
visibility toggle once validated.

## Properties (as implemented, PATHCPU)

- `path.restir.gi.enable` (default 0)
- `path.restir.gi.candidates` (K fresh candidates, default 4)
- `path.restir.gi.temporal.enable` (default 1) — temporal merge with
  the reconnection shift + binary-V test.
- `path.restir.gi.spatial.enable` (default 1) — G1-b: up to 2
  neighbour pixels in a 5x5 window, same-surface gated on x1 (E2b
  constants), Jacobian-corrected shift + binary-V test; the stored
  reservoir keeps its pre-spatial state so merge weights cannot feed
  back.

## Validation status

- `dev-tools/e19_restir_gi_test.py` (cornell, 160×120):
  unbiased mean within 3% vs a 512-spp reference, RMSE bounded at 3×
  the plain baseline, finite output, temporal combo — **CPU 5/5 +
  GPU 5/5 PASS**. The K=1 edge case was used to isolate the
  support-floor bias (above). The GPU section additionally asserts a
  merge-explosion tripwire (bulk pixel-ratio p99 < 3 and the >8x
  hot-pixel fraction < 0.5%) — see the GPU concurrency notes below.
- Regression: e14/e16/e17/e18 stay green.

## GPU port (G2, as implemented)

Three new pipeline stages on `pathoclbase` (dense + wavefront
dispatch, OpenCL and Metal):

1. `MK_GENERATE_NEXT_VERTEX_RAY` (depth-0, non-delta x1) draws K BSDF
   proposals and queues their bounce rays into the GI tail —
   `candRays[0..K-1]`.
2. `MK_RT_GI_BOUNCE` consumes the hits, builds each hit's x2 BSDF in
   the task's `tmpBsdf` scratch (`BSDF_Init` re-derives the material
   from the `RayHit`), records emission/env radiance, and queues one
   NEE shadow ray per hit candidate — `candRays[K..2K-1]`. It also
   queues the temporal-visibility ray into `candRays[2K]`
   (x1 → stored x2 reconnected segment; masked when no usable entry).
3. `MK_RT_GI_RESOLVE` folds the NEE visibility into the proxies, runs
   RIS + temporal + spatial merges, stores the pre-spatial reservoir
   and hands the winner to `MK_GENERATE_NEXT_VERTEX_RAY` through the
   task's `RestirGIResult` record (`pending`/`dir`/`bsdf·W`/`pdfW`).

The tail stride is `1 + DI slots + 2K + 1` (the last slot is the
temporal V-ray); GI candidate/result records live in
`restirReservoirs[]` after the per-pixel `RestirGIReservoir` region
(`giReservoirOffset`, `giCandDataOffset`, `giCandStride`,
`giCandRayBase` in `taskConfig.pathTracer.restirGI`). K is host-clamped
to ≤ 4 and further reduced by a 256 MB tail budget.

### GPU concurrency findings (why Metal exploded and OpenCL did not)

- **Pass stamp** (`RestirGIReservoir.pass`): thousands of tasks touch
  the same pixel's reservoir inside one pass; without a stamp the
  temporal merge folds a just-written wSum back into itself and the
  weight compounds geometrically. Temporal merges accept only entries
  stamped `pass < own pass`; spatial merges intentionally keep
  same-pass neighbours.
- **Seqlock publish**: the store writes `pass = 0xFFFFFFFF` first and
  the real stamp *last*; merges snapshot the entry and re-read the
  stamp afterwards, rejecting torn or superseded reads. On Metal
  (weaker store ordering + translated atomics) unsynchronized
  read/write produced measurable corruption; OpenCL's device path
  happened to hide the same window.
- **vSeq pairing** (`RestirGIResult.vSeq`): the bounce tags the
  visibility ray with the entry's stamp; the resolve accepts the
  merge only if the entry still carries that stamp — a reservoir
  rewritten between bounce and resolve can never pair a stale
  occlusion test with a new sample.
- **Representative-winner gate on the temporal merge** (same 5%
  filter as spatial): rejects stored entries whose winner target is
  disproportionately small vs `wSum/m` — a corruption signature that
  otherwise injects a large stale block.
- **GuidingPass sampler-type dispatch** (pre-existing latent bug the
  GI work exposed): `GuidingPass` cast `samplesBuff` to
  `RandomSample` unconditionally, so on Sobol it read RNG state as the
  pass — garbage stamps made the temporal gate effectively random and
  also polluted guiding seeds. Now dispatches on
  `taskConfig->sampler.type` (TilePath/Sobol/Random layouts).

Residual known-good tail: merges still produce a handful of
~10–300× pixels at low spp (e.g. a stale-bright proxy whose reconnected
segment keeps a saturated ratio) — the CPU path shows the same tail
(max ≈ 70x on cornell at 32 spp, both merge paths), it shrinks with
spp (64×48 test: max ratio ≈ 1.2 at 32 spp), and the e19 tripwire
gates on bulk statistics rather than the max.

### Audit follow-ups (CPU parity + failed-shift mass)

- **CPU seqlock + pass stamp**: the CPU `Reservoir` grew the same
  `pass` stamp the GPU entry carries, read/written through
  `std::atomic_ref` so the struct stays a POD. Merges snapshot the
  entry under `acquire` and re-check the stamp afterwards; temporal
  merges require a strictly older pass, spatial merges accept any
  consistent (non-mid-write) entry. Without it, torn reads and
  same/future-pass feedback injected arbitrary wSum - measured as the
  e19 CPU T3 regression (RMSE 0.0315 vs 0.0092 baseline, 3.4x gate).
- **Failed shifts do not count toward M** (both CPU and GPU, temporal
  and spatial): a reconnected segment that is degenerate, occluded, or
  (GPU) unvalidated by `vSeq` produces a sample outside the current
  target domain; crediting its `m` anyway inflated `mTotal` with draws
  that could never win and systematically darkened the output (cornell
  temporal-only mean −3.5% → −1.9% vs reference; GPU gi+merges mean
  −1.2% → −0.3%, RMSE 0.0112 → 0.0095). In-domain draws with zero
  target (black BSDF eval) still count, matching the fresh-candidate
  convention.
- **Cost report (measured)**: on cornell at K=4 the resample costs
  ~2.2× wall time per 64 spp (K bounce rays + K NEE probes per depth-0
  vertex) and does *not* reduce RMSE there — cornell's indirect is
  diffuse and smooth, so BSDF sampling is already near-optimal. The
  honest G1 status is "estimator correct, reuse machinery in place";
  the variance win requires scenes with concentrated indirect
  (interior-through-window) and is expected to come from the GPU port
  where the candidate rays ride the tail queue.
- **classroom-hdr (measured, Metal, 160×120)**: the "concentrated
  indirect" follow-up scene (sky.exr gain 10 through window portals).
  At 32 spp vs a 512-spp reference: GI mean matches the off baseline
  exactly (gi/off = 1.000, both ≈ 0.152) and p9999 tails are identical
  (~3.7 both) — the estimator is unbiased and stable on a textured
  real scene — but RMSE is still a wash (gi/off ≈ 1.03). The dominant
  error at these settings is primary-light + first-bounce noise, not
  the indirect tail ReSTIR GI reuses; a win case needs a scene where
  indirect dominates image error (e.g. glossy-indirect or multi-bounce
  interiors) or deeper-vertex GI (deferred).

## Risks

- **Correlation artifacts**: spatial reuse shares paths between
  pixels — splotchy low-frequency noise under too-aggressive reuse.
  Mitigations: same-surface gate, merge cap, ratio clamp, K floor.
- **Proxy quality**: `L̂` = direct-only under-proposes x2's that are
  strong *indirect* reflectors (e.g. bright walls). W corrects bias;
  variance residual is the measured item for the recursive-GI follow-up.
- **Temporal lag** on dynamic scenes: same bounded-bias story as DI.
- **Tail-buffer arithmetic**: GI adds slots to the candidate tail —
  the E2a OOB wedge came from exactly this class of bug; the stride
  change needs the same allocation-site audit plus a 512K-task smoke.
