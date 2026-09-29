# ReSTIR PG design — reservoir-fed path guiding

Reference: Zeng, Kettunen, Wyman, Wu, Ramamoorthi, Yan, Lin,
*ReSTIR PG: Path Guiding with Spatiotemporally Resampled Paths*,
SIGGRAPH Asia 2025 (12:1-12:11, doi 10.1145/3757377.3763813).

## What the paper does

After ReSTIR's spatiotemporal reuse, the *accepted* path samples are
collected per pixel (vertex position, normal, BSDF id, incident dir
per vertex). Those directions are splatted into a spatial hash grid;
each cell fits a 4-component vMF mixture with plain EM over the
splat directions, and the next frame's RIS initial candidates are
drawn from the fitted mixtures instead of the raw BSDF/pdf proposal.

The key property that makes this cheap: ReSTIR's accepted samples
already approximate the resampled target distribution, so the EM fit
needs **no radiance weighting** (unlike Vorba 2014-style weighted EM
on raw path vertices). Guiding only shapes proposal q — the estimator
stays exact/bounded through the RIS weights, so a stale or wrong
field costs variance, never correctness.

## Why it matters for us

Measured PT-1/PT-2 weakness on pg scenes: −16..−21% mean parity at
low spp — rare high-radiance suffix directions contribute only after
some path first *probes* them, so consumed picks underfill early. The
K=4 fresh candidates drawn from the BSDF proposal miss narrow
caustic/glossy lobes often enough that reservoirs stay cold. Feeding
the reservoir's own winners back into the proposal closes this loop:
cells that found a hot suffix get guided candidates toward it, and
the reservoir population converges far faster. Same mechanism
stabilizes ReSTIR GI's proxy-guided tournament.

## Adaptation to SuperLuxCore (no new infrastructure)

We already have everything except the record feed:

- **GuideTree** (`pathoclbase_funcs.cl`, `path.guiding.*`): flattened
  SD-tree, 4-component vMF per leaf, `GuideTree_Sample` +
  `Guide_MixWeight` BSDF×vMF one-sample MIS — exactly the fitted
  distribution the paper produces.
- **Training records**: CPU defers `GuidePending` records
  (position + direction + measured continuation credit,
  `pathtracer.cpp` ~1910); GPU writes float4 records into the
  16×256-slot `guideRecN` ring buffers (task t → buffer `t&15`,
  slot `(t>>5)&255`). Host drains them into the EM fit on the
  ForceSwap cadence.
- **Reservoir winners**: `RestirPT::Reservoir`/`Pick` (CPU) and
  `RestirPTResult`/`RestirGICandidate` (GPU) already carry x1 + the
  winning dir + measured/proxy payoff — precisely the paper's
  "selected path sample" tuple.

### Feed design

At each reservoir resolve (CPU `RestirPT::ResampleSuffix`, GPU
`MK_PT_RESOLVE`/`MK_RT_GI_RESOLVE`), emit one guiding record
`(x1, winningDir)` — or reuse the existing per-vertex credit path
with the winner's payoff as the recorded value. The RIS-selected dir
approximates the target, so plain EM is correct by the paper's
argument. Rate: at most one record per resample vertex (K records
would be wrong — losers don't follow the target).

Proposal side is unchanged: `GuideTree_Sample` + `Guide_MixWeight`
already mix the fitted vMF into candidate draws (`guidingEnable`
gated, `guidingMinDepth` applies at the depth-0 vertex where PT/GI
resample). The reservoir candidates then come from
`mixture(BSDF, vMF)` instead of BSDF alone.

### Guards

- Records shape q only — never estimator weights (same rule the
  deferred-record comment already states).
- Suppress the feed while `storeEps`-gated winners dominate (reservoir
  still cold) to avoid locking onto noise: emit only when the winner
  came from a measured suffix or a positive proxy.
- Pass-stamp the record stream with `GuidingPass` so a stale tree
  upload can't double-count.

### Risks / open questions

- Feedback loop: guided candidates → stronger winners → tighter field.
  Bounded by the m-cap and the BSDF mix weight floor; watch for mode
  collapse on multi-modal cells (paper uses 4 vMF components; our
  leaf count is the spatial bound).
- The paper splats *every* vertex of each accepted path; our depth-0
  reservoirs give one dir per pixel. Coverage at depth ≥ 1 needs the
  same records from GI's x2 or a second reservoir level — v1 feeds
  depth-0 only (where reuse already concentrates).
- TILEPATHOCL: record writes live in the same kernels — free.

## Status

Landed (ef17fe5b9, e83863506):

- **Feed**: reservoir winners emit training records — consumed picks
  at `MK_PT_RESOLVE` (GPU) / the consumed `Commit` call site (CPU),
  landed armed picks at `MK_SPLAT_SAMPLE` commit / the deferred
  `Commit` call site. All emit with measured-suffix flux only.
- **Proposal**: `RestirGI_EnqueueBounce` (shared PT/GI path) draws
  each candidate from the `(1-wG)*BSDF + wG*guide` one-sample mixture;
  `rec->pdfW` stores the mixture density so RIS weights stay exact.
  CPU `ResampleSuffix` mirrors via new `guideCache`/`guideStrength`
  args. wG = `guidingStrength * MixWeight(count, peak)`, gated on the
  leaf being warm (nComp>0, count>=256) — cold leaves stay pure BSDF.
- `cl2msl` note: forward declarations need explicit `__private` on
  scalar pointer params — the address-space rewrite only fires on
  definitions.

Measurement (PATHOCL, RMSE ratio pt+pg/pt): pg-indirect 1.030 and
pg-glossy 1.002–1.006 at 64–256spp — neutral, the diffuse-dominated
leaves gate themselves out via PeakGate as designed. On
focused-caustic-ring (concentrated directional transport) the sign
flips: 0.991 at 256spp — the loop engages where the paper predicts.
Verified live via LUX_PG_DEBUG: records drain and the tree swaps
(`leaves=9 kept=20466 warm=1` at ~4s).

GI-side symmetry (landed, same commit):

- `RestirGIResult` carries the winner's proxy radiance `lHatR/G/B`
  (populated in `RestirGI_Resolve` for fresh/temporal/spatial picks);
  `MK_RT_GI_RESOLVE` emits a record at x1/dir with flux `Y(lHat)`
  when `pending == 1` (winner), nothing on the pending==2 fallback.
- CPU `ResampleFirstBounce` gained `outLHat` plus the same
  `guideCache`/`guideStrength` mixture as `ResampleSuffix` — GI
  candidates draw from `(1-wG)*BSDF + wG*guide` and `pdfs[]` holds
  the mixture density; the caller records x1/dir/`lHat.Y()` on
  success. Fresh `GIRandom` streams 0x05/0x06 avoid collisions with
  the existing 0x01–0x04/0x40+ stream map.
- Regressions: e19 GI 10/10 (CPU+GPU Release), e95 6/6, e96 7/7.

Open: depth>=1 reservoir records; longer renders on caustic-heavy
scenes.
