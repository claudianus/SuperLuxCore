# Manifold Path Guiding — design notes (E4 last item)

Roadmap item: "매니폴드 패스 가이딩(히스토리→seed 중요도)".

## What the literature says (2023–2026 survey)

- **MPG** (Fan et al., TOG/SIGGRAPH Asia 2023, 42:6/257, arxiv
  2311.12818): learns a discrete seed distribution over (chain length
  n, scattering type, first direction) from historical/coherent
  sub-paths, then manifold-walks seeds into admissible chains. Up to
  40× variance reduction on >5-bounce chains. Supplemental: photons can
  seed the distribution — directly matches our PhotonGI inventory.
- **PMS** (Lee/Jhang/Chang, PACMCGIT 7(3)/HPG 2024): unbiased; reuses
  cached multi-bounce caustic photon *intermediate specular vertices*
  as manifold seeds with a small perturbation cone — "bidirectional
  info for free" given photons/light tracing.
- **Dimension-reduced MNEE** (Granizo-Hidalgo & Holzschuch, PACMCGIT
  7(1) 2024): MNEE roots live on a 1D curve on the specular surface —
  drops derivative computation; cheapest upgrade to our existing
  Newton solver.
- **ReSTIR BDPT** (Hedstrom et al., TOG 2025): caustics reservoirs in
  technique-aware extended path space — amortization layer, high
  implementation cost.
- **MIR** (Fan et al., TOG 44(4)/SIGGRAPH 2025): reweight paths across
  guiding iterations — cheap bolt-on, reduces overfitting.
- Consensus: fitted directional distributions cannot represent delta
  transport; caustics need specialized estimators + seed quality.

## Existing inventory (already paid for)

- `MneeSeedEntry` world-space hash cache — per-(light, occluder,
  blocker-cell) converged-vertex store; seeds select Newton basin only
  (unbiasedness kept by constraint verification).
- PhotonGI caustic **beams** — light-side segments ending in specular
  deposits; geometrically encode candidate specular sub-paths.
- LMNEE light→camera manifold connect + MGE emission focus rings.
- SSP tail — recorded eye-side specular chains (anchor vertices).

## Proposed MPG-lite design

Phase A (energy-aware retention — smallest diff, direct "history→seed
importance") — **implemented**:
1. `fluxWeight` added to `MneeSeedEntry` (CPU `std::atomic<float>`,
   GPU `float`; entry now 44 bytes). Eye-side stores
   `pathThroughput.Y()`, LMNEE side stores the splat `radiance.Y()`.
2. Collision policy: winner-wins — a converged basin that historically
   carried more flux keeps the slot; equal/brighter newcomers displace
   it. Reservoir-style weighted retention and `chainLen`
   stratification deliberately deferred (measure first).

Phase B (photon-vertex seeding — PMS) — **implemented, simpler than
the original plan** (no beam-index query at connect time, no endpoint
pdf bookkeeping):
4. The photon walk snapshots its last strict-delta vertex per
   iteration (`bsdfEvent & SPECULAR` && chain still specular). A
   caustic deposit queues an `MneeSeedRecord` carrying that vertex —
   the light sub-path already traversed the exact manifold an
   eye-side MNEE connection must Newton-solve, so the vertex is a
   zero-cost warm start.
5. Records are keyed like eye-side entries ((light ptr, occluder
   mesh*2+side, quantized occluder-side position)) and replayed into
   the PathTracer seed table via `MneeSeedStore`, so the Phase-A
   energy-aware retention policy arbitrates them for free. The seed
   stays basin-only — the solver verifies the half-vector constraint
   — so NO endpoint-pdf bookkeeping is needed and unbiasedness holds
   automatically (the PMS gotcha below only applies to seeding that
   perturbs the estimate itself).

   Wiring (`include/slg/engines/mneeseedcache.h` is the shared
   entry/key/store): `TracePhotonsThread::TracePhotonPath` records,
   `PhotonGICache::TracePhotons` folds per-thread vectors into
   `mneeSeedRecords`, `MergeMneeSeeds()` drains into the table.
   Engines call `SetMneeSeedCache` + `MergeMneeSeeds` where
   ParseOptions order demands it (PATHCPU/BAKECPU preprocess BEFORE
   ParseOptions — records defer; TILEPATHCPU/PATHOCLBASE/BIDIRCPU
   wire the pointer before Preprocess — records merge at join).
   PATHOCL uploads the seeded CPU table into `mneeSeedsBuff` at
   thread init; GPU-deposit (ingestOnly) sessions skip CPU tracing,
   so their table stays empty — same estimator, GPU keeps
   self-seeding (feature-parity safe; kernel-side injection is the
   Phase B follow-up for the MK_LIGHT_VERTEX deposit path).
   Record staging is bounded at 4x table size (~64k records).

Phase C (dim-reduced solver):
6. 1D-curve solve for single-vertex chains (Granizo-Hidalgo'24) —
   replaces Newton+derivatives on the hot path.

## Gotchas

- Seed cache entries must keep "basin-only" semantics: never let seed
  selection enter the estimator weight — verification already does.
- Photon-seeded walks need the endpoint PDF of the sampled seed —
  PMS's small-cone perturbation is what makes the pdf computable;
  reuse without perturbation is biased-only.
- GPU parity: `mneeSeeds` is a shared CPU/GPU table — the `.cl`
  `MneeSeedEntry` is the ABI mirror for the C++ struct; any field
  change must update both sides (Phase A did, 44B).
- Measured (seedcache.scn, 192x144, 2-seed avg): PATHCPU cache on/off
  mean ratio 1.1415, caustic region 1.0583; PATHOCL 1.1406 / 1.0583 —
  near-identical CPU/GPU, finite everywhere (e17 ALL PASS).

## Phase C findings — solver instrumentation & failure-evidence (e52)

Env-gated diagnostics (`pathtracer_mnee.cpp`, zero cost when off):
- `LUX_MNEE_SEED_STATS=1` dumps at session end (via `PathTracer` dtor ->
  `MneeDumpSessionStats`, since Blender never dlcloses the module):
  solves, fails, iters/solve, failure classification
  (residual/singular/no-step/exhausted), capped count, seed
  lookups/hits.
- `LUX_MNEE_POISON=0` A/B-disables the failure-evidence policy.

Measured on the e52 glass-sphere scene (off-axis point light, 64spp,
PATHCPU): ~3.7M solves, ~47% fail — dominated by `no-step` (~1.1M,
silhouette/mesh-miss line-search exhaustion) and ~0.6M chain-solver
fails. These are *structural* misses (impossible manifold connections),
not basin-selection problems:

- 4-quadrant tangent reseeding: +rescue ~1.4pp fails at ~20-40 extra
  iters per failed solve — net loss, **reverted**.
- Dimension-reduced plane walk (Granizo-Hidalgo'24 style 1D solve on
  the x0/endpoint/caster-centre plane): 1.07M attempts -> ~4.3k rescues
  (0.4%). The scalar walk converges the in-plane component fast but
  stalls ~3e-3 short of the 3e-4 tolerance — it locates the basin but
  cannot finish the last mile; the 2D polish handoff rarely closed it.
  Plane re-refinement rounds did not help. **Removed**: kept the
  finding here instead of the code.
- Mirror-style off-axis seed for refraction (dropping the
  `etaVertex == 1` gate): +3.4pp fails and ~1 extra seed trace per
  cold glass solve — the line seed is genuinely informative for
  refraction. **Reverted**.

What worked — failure-evidence ("poisoned") seed entries:
`MneeSeedEntry.failCount` records per-cell solve failures
(namespaced like seeds). Cells with >= 2 failures run a capped probe
(`MNEE_FAIL_PROBE_ITERS = 8` counter units — `iteration` counts outer
steps AND rejected proposals, so the cap bounds ray casts directly).
Measured: 47.8% fails (baseline-matched) at 23.6M iters vs 27.5M
poison-off (-14% solver work). A positive-seed rescue keeps the full
budget; any converged solve clears the counter (self-correcting).
Iteration cap is a termination heuristic on a verified estimate —
bounded cost, no estimator authority.

Non-findings worth remembering: a "residual halved by iter N" stall
abort never fires usefully (accepted line-search steps strictly
decrease the residual, so non-improvement already exits via no-step);
seed snapshots must be re-read at rescue time (concurrent stores land
between the pre-solve lookup and the failure - a stale snapshot
measurably cut the rescue rate).

Chain solve instrumentation (same env gate): per-reason histograms for
the eye (`rej`) and light (`LMNEE_REJ`) chain paths. e52 decomposition:
light-side Newton failures dominate - ms-newton=638k (chain Newton)
plus newton=636k (single-vertex LMNEE, previously hidden in "other").
Topology-discovery fails (ms-chain<2=189k) and mode/same-side rejects
are cheap early-outs; eye-side chain fails are ~0. The residual
bottleneck is therefore *light-side Newton attempts on physically
unsolvable configurations* - the failure-evidence cap bounds their
cost; a smarter candidate prefilter would be the next lever.
