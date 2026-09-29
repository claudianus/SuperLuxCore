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

Phase B (photon-vertex seeding — PMS):
4. Extend `Photon`/`PhotonBeam` records with the specular vertex that
   produced the deposit (one extra Point+Normal per beam).
5. On a blocked LMNEE connect, query `causticBeamsIndex` for beams
   near the receiver; use their stored specular vertex as a Newton
   seed with the Bernoulli/uniques bookkeeping PMS uses for
   unbiasedness (biased variant first — simpler, still useful).

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
