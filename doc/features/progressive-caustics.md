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
- **C2 progressive GPU merge radius** (audit items B-1/B-2/B-3):
  per-light-subpath sample index `n_t` → `r_t = r0/(n_t+1)^(.5(1-α))`;
  `vmNorm/misVc/misVm` derived per-merge from the vertex's stored pass
  instead of taskConfig constants (`init.cpp:659` already anticipated
  this). Hash cell policy under varying radii. CPU reference:
  `bidirvmcputhread.cpp:111`.
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
- **C4 GPU beam index**: `IndexBvh<PhotonBeam>` (segment AABBs,
  periodic CPU build + upload — same update cadence as the existing
  PGIC BVH upload in `compilephotongi.cpp`). Gives GPU the B2 media
  caustics; later a GPU-side hash index removes the CPU build.
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
