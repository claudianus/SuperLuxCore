# Progressive Caustics — replacing the PhotonGI pre-pass caches

Status: **design + Stage C1 partial landing**. Parent doc:
`doc/features/caustics-sota.md` (estimator coverage map, Stage A/B2
landed). Engineering notes: `doc/engineering/pgic-beams.md`.
This document is the synthesis of a 2019-2026 literature + codebase
audit aimed at one goal: **caustics & hard indirect paths work with a
single checkbox, progressively, identically on CPU and GPU** — the
"Corona" bar.

## What the research says

Corona's "Fast Caustics Solver" (Šik & Křivánek, HPG 2019) is the UX
target decoded: it is a **VCM-lite** — plain path tracing plus photon
density-estimation merged by MIS — with **Metropolis-steered photon
emission**, inside a fully progressive loop. Single checkbox, no
pre-pass settings, ~zero overhead where no caustics exist. V-Ray
Progressive Caustics and Octane Photon Tracing are the same family.
No production renderer ships reservoir/NN caustics yet — an opening.

The audit's key finding: this fork already owns ~90% of that
architecture:

| Corona component | Already in tree |
|---|---|
| VCM merge (PT + photon estimate, MIS) | `bidirvmcpu/` full CPU VCM reference; GPU M7 `VC_MERGE` kernels + `vcMergeHash` (fixed radius today) |
| Steered photon emission | `path.lighttracing.focus.*` caustic focus cache (per-light hotspot ring, unbiased mixture pdf) — CPU+GPU parity, Metropolis-equivalent on GPU; Metropolis photon sampler on CPU |
| Progressive loop | per-iteration kernels already rebuild the merge hash; PhotonGI `Update()` already re-traces + shrinks radius (radius shrink = Knaus-Zwicker-style global schedule, needs no local stats) |
| Beam estimate for media | `PhotonBeam` (Stage B2) — the UPBP B1D estimator variant |

Gaps to a "no pre-pass" solver: the merge radius is constant, the
PhotonGI photon pass still wants a visibility map for Metropolis
targeting and point-deposit gating, and everything per-photon is a
CPU trace.

## Target architecture — Progressive Photon Pipeline (P3)

One estimator family (PPM/VCM), one progressive loop, zero required
settings:

```
eye tasks ─┐                     ┌─ photon/beam deposit buffer
           ├─ merge/gather ◄─────┤  (hash grid, per-iteration)
light tasks┘  (MIS, shrinking r) └─ focus-guided emission
```

- **Deposits** come from light subpaths (CPU `TracePhotonPath`, GPU
  `MK_LIGHT_*` states), steered by the caustic focus cache on both
  sides — no visibility map.
- **Gather** happens at eye receiver vertices (CPU `hashgrid`-style or
  the existing PGIC BVH query; GPU vcMergeHash pattern) with a global
  shrinking radius — consistent (unbiased limit), never unbiased-
  breaking.
- **Beams** join the same hash/index for medium flights (UPBP-style:
  points + beams, disjoint by receiver medium class as in Stage B2).
- **Auto-params**: every radius derives from the film-footprint probe
  (landed), sizes derive from memory/film budget, schedules derive
  from spp.

## Stages

- **C1 zero-config** (landed): `caustic.lookup.radius=0` and
  `updatespp.minradius=0` auto-derive via `Film2SceneRadius`
  (single shared evaluation). Remaining: `caustic.maxsize=0` →
  photons-per-pixel/memory budget (Corona scheme), `photon.maxcount`
  cap sanity, Blender "automatic" collapse.
- **C2 progressive GPU merge radius** (landed): the render loop
  tracks `t = lightSampleCount / lightTaskCount` (mean completed
  sub-paths per light task — the GPU analogue of BIDIRVMCPU's
  iteration counter) and re-derives `mergeRadius` + the three
  MIS constants, re-uploading `GPUTaskConfiguration` only when
  the pass index changes. `path.vertexconnection.mergealpha`
  (default 0.95, `bidirvm.alpha` parity; 1.0 = fixed radius).
  Kernel-side unchanged: merge hash cell size tracks the current
  radius, so queries stay self-consistent.
- **C3 kill the visibility pre-pass** (partially landed as C3a):
  - `UseFrustumCulling()` — caustic-only caches on projective
    cameras skip `TraceVisibilityParticles()` entirely; deposits
    are culled by `Camera::ProjectPointToFilm` + a 10% border
    (RTPM photon culling) instead of near-entry gating. The
    Metropolis bootstrap then self-targets: `usefulPath` =
    "deposited inside the film frame", and local mutations cluster
    around successful caustic paths — the same job the visibility
    map did, without the pass. Indirect caches still build the
    visibility map (their deposits target near-entries), and
    environment/stereo cameras fall back to it.
  - Remaining C3b (deferred): focus-cache emission steering for
    the RANDOM sampler path; Metropolis already self-steers.
- **C4 GPU beam index** (landed): `PGICBeamIndex` is a flat
  `IndexBVHArrayNode` tree over radius-inflated segment AABBs —
  the same array drives CPU `Query()` and the device traversal
  (`PGICBeamBvh_ConnectAllNearEntries`, mirrors
  `ConnectCausticBeams` one to one). Buffers upload in
  `InitPhotonGI` and refresh through the existing
  `Update()`→`RecompilePhotonGI`→`InitPhotonGI`+`SetKernelArgs`
  path each generation. Verified +12.6% medium-caustic energy on
  GPU vs +14% CPU (fog+glass Cornell, inside noise).
- **C8 deferred initial cache** (landed): when
  `caustic.updatespp>0`, `Preprocess` no longer traces the first
  caustic cache synchronously — it launches the C7 background
  worker immediately and rendering starts on an empty cache
  (~0.3s preprocess on the fog scene). Generation 1 lands via the
  normal pending-swap; `Update()` adopts thread 0's callback so
  GPU recompilation still fires. `updatespp=0` keeps the
  synchronous build (final-render/fixed-cache mode). This is the
  "no pre-cache" half of the Corona-UX goal: first pixels are on
  screen before the cache exists.
- **C7 stall-free periodic updates** (landed): `Update()` used to
  park every render thread at a barrier while thread 0 re-traced
  photons and rebuilt BVH + beam index (seconds of viewport freeze
  every `updatespp`). Now thread 0 launches a background
  `UpdateWorker()` (jthread) that fills a shadow copy — photons,
  beams, BVH, beam index, shrunk radius — while render threads keep
  querying the live cache. The swap runs inside the `std::barrier`
  **completion step** (`completion_t`), where all render threads are
  already parked and no query can be in flight — atomic, no spin
  locks, no torn state. `TracePhotons`/`BuildCausticBeamsIndex` take
  output buffers so the live containers are never touched by the
  worker. Failure sets a retry flag; `FinishUpdate`/destructor join
  the worker first. Related: empty-cache early-out in
  `TracePhotonsThread` — scenes with no cacheable transport traced
  the full `photon.maxcount` (100M paths, ~30 s) every update;
  threads now bail after 4M paths when nothing was stored at all
  (~22× less wasted work on non-caustic scenes).
- **C5 SPPM per-pixel photon pass** (the structural endpoint):
  per-pixel state `{τ, N, R, hitpoint}` (RestirGI/samplerSharedData
  appended-array pattern), photon deposit tasks, hash+gather kernels.
  Replaces BOTH indirect+caustic caches with one convergent
  per-pixel estimator and removes every global map. Largest change.
- **C6 caustic reservoirs (moonshot SOTA)**: frame-accumulating
  per-pixel reservoirs of caustic-contributing light subpaths —
  ReSTIR-BDPT-lite (Hedström TOG'25) reusing GRIS machinery + our
  VC connect paths for shift reconnection. No production renderer
  has this; it is the differentiator.

## Cross-cutting optimizations (standing mandates)

- Knaus-Zwicker 2011 global radius schedule — keeps consistency
  without per-point statistics (already the shape of `updatespp`
  radius reduction; formalize it).
- Photon culling: reject deposits outside the camera frustum /
  film footprint before they consume cache budget.
- Stochastic evaluation in dense regions (RTPM) once density is high.
- `SpillableArray` already backs photon vectors — keep memory bound
  under the auto-budget rule.

## Non-goals / constraints

- Estimator classes stay disjoint per the Stage-A partition contract;
  merging must never double-count with eye-side sampling.
- CPU and GPU must produce equivalent images at scale (parity gates
  in e54 + `vc_merge` regression family).
- Serialization compatibility: new persistent state needs
  BOOST_CLASS_VERSION bumps.

## References

- Šik & Křivánek, *Implementing a Fast Caustics Solver in Corona*,
  HPG 2019 — the single-checkbox blueprint.
- Knaus & Zwicker, *Progressive Photon Mapping: A Probabilistic
  Approach*, TOG 2011 — statistics-free global radius schedule.
- Hachisuka et al., *SPPM*, SIGGRAPH Asia 2009 — per-pixel
  progressive merging.
- Georgiev et al., *VCM*, SIGGRAPH Asia 2012 — MIS merge; our
  `bidirvmcpu` is a complete implementation of this.
- Kern et al., *RTPM*, JCGT 2023 — GPU photon mapping engineering
  (inverse radius search, photon culling, stochastic evaluation).
- Moonen & Jalba, *GPUUPBP*, CGF 2023 — GPU points+beams reference.
- Jarosz et al., *Progressive Photon Beams*, SIGA 2011 — landed B2.
- Hedström et al., *ReSTIR BDPT*, TOG 2025 — Stage C6 blueprint.
- Stamm et al., *Photon-Driven Neural Radiance Caching*, I3D 2026 —
  neural amortization direction (deferred; SHaRC-style non-neural
  variant is the cheaper intermediate).
- Krivánek et al., *UPBP*, TOG 2014 — points+beams+paths MIS.
- Production precedents for auto-tuning: Corona "photons per pixel",
  V-Ray screen-scale 0.02 (= Film2SceneRadius imagePlaneRadius),
  Mitsuba `initialRadius=0` auto, RenderMan `photonEstimationNumber`,
  Octane progressive gathering radius.
