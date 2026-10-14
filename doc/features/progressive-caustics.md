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
  (single shared evaluation). The caustic radius takes 1/5 of the
  probe (2% of the image plane → ~0.4%, ~3px): the full probe radius
  gathered hundreds of photons per eye vertex at the ~2 photons/pixel
  budget, one BSDF evaluation each (2026-10, pool 800x500 20s:
  24 → 145 spp, cache-off 218; sharper lines; 1/10 shows
  low-frequency blotches). Remaining: `caustic.maxsize=0` →
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
  - `SkipsVisibilityPass()` — caustic-only caches (any camera) skip
    `TraceVisibilityParticles()` entirely. Caustic deposits are
    **not coverage-gated at all** (2026-10 pool SDS fix): the
    original C3a culled them by `Camera::ProjectPointToFilm` + a
    10% border, and the visibility-map path gated them by
    near-entries. Both gates disagreed with the eye side, which
    queries the cache - and cuts the light hit it replaces - at
    every receiver it reaches. Receivers seen only through
    refraction (a pool floor under the water surface lies well
    outside the straight frustum; camera visibility particles stop
    at the first diffuse hit) got neither photons nor the light hit:
    the pool floor rendered at 0.37x (frustum) / 0.24x (visibility
    map). Cost of accepting every deposit: cache capacity spent on
    off-screen caustics (~1/3 on the pool test). Indirect caches
    still build the visibility map for their own deposits.
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
  screen before the cache exists. Until generation 1 lands the eye side
  must not cut the light hits the cache replaces
  (`PhotonGICache::IsCausticReady()` / device
  `causticPhotonTracedCount > 0` gate the chain-cached flag): the
  cut ran from the first sample, so every pre-generation sample
  rendered SDS receivers black (e115 CPU floor 0.94x at 30 s on a
  small film, where those samples are a large share).
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
- **B1′ light-task photon piggyback** (LANDED): GPU light tasks
  already trace the same paths a photon pass would, so
  `MK_LIGHT_VERTEX` appends caustic records — an atomic push into a
  shared-storage buffer (Metal shared mode: host reads after
  `FinishQueue`, no DMA limit) — turning the light pass into the
  photon generator for free. Design + semantics:
  - Deposit mode activates when `caustic.enabled` + projective
    caustic-only cache + `updatespp > 0`. With `lighttracing.enable = 0`
    a tail fraction (`path.photongi.deposit.taskfraction`, default
    0.2) of the task population runs **deposit-only**: the whole
    camera-connect block (visibility ray, film splats, LMNEE) is
    gated on `lightTracing.enabled`, and paths terminate the moment
    the specular prefix breaks (they can never deposit again).
  - Kernel deposits mirror `TracePhotonsThread` exactly: the
    specular prefix is `lpi->isNearlyS && lpi->firstVertSeen`
    evaluated before the vertex folds in; homogeneous-volume flights
    chunk into beams at `max(8·r, len/16)` with length-share flux
    pre-attenuated to the chunk start; point photons take
    post-segment flux, flipped landing normal, no receiver coverage
    test (see C3a).
  - Host `DrainPGIC()` (per thread, inside the queue-synchronized
    film-readback window) reads the 4 counters + records, resets
    cursors, and calls `PhotonGICache::IngestTracedPhotons()` —
    a staging area the update worker turns into the newest batch of
    a **sliding generation window**: each generation is the newest
    batches fitting one combined `maxSize` room (CPU semantics:
    photons+beams share the cap); the batch straddling the budget is
    stride-thinned and its traced count scaled by the kept fraction,
    older batches retire. `ingestOnly` mode skips the CPU re-trace
    entirely. Only 4M deposit-free paths retire the tail tasks
    (clearing `depositEnabled` in `taskConfig`, they park in
    `MK_DONE`). Earlier the population accumulated to `maxSize` and
    then froze (deposit tasks retired on "cache full"): the radius
    kept shrinking over a fixed photon set, and the last partial
    batch counted all its traced paths for the few records that fit
    (density underestimated).
  - Adjoint refraction fix (2026-10): the GPU
    `GlassMaterial_EvalSpecularTransmission` applied the radiance
    `eta^2` factor to light paths too (the CPU `fromLight` branch was
    commented out). Closed glass objects cancel it (entering and
    leaving multiply to 1), but a light path that ends inside water
    lost `1/1.33^2 = 0.565`: every GPU B1' pool-floor photon and LT
    splat through a water surface came out 0.56x.
  - Overflow normalization (2026-10): a drain whose deposits exceed
    `deposit.capacity` used to count every traced path for the
    records that fit (e115 zero-config GPU floor at 0.955x, worse with
    LT on: more light tasks per drain). The drain now keeps a common
    leading fraction of photons and beams and scales its traced count
    by it - overflow costs light-task work, never brightness.
  - Swap barrier exit (2026-10): finished render threads leave the
    generation-swap barrier with `arrive_and_drop` (FinishUpdate), and
    the PATHOCL native lt.only loop now calls FinishUpdate - a kept
    seat hung the GPU thread at its next swap (e102 lt.only).
  - Disney receivers (2026-10): the Blender add-on exports every
    Principled BSDF as `disney`, whose `GetEventTypes` reported TRANSMIT
    unconditionally - PhotonGI never deposited on it (a Disney pool
    floor stored 0 caustic photons). TRANSMIT is now reported only when
    the transmission texture is not a constant <= 0. Disney events are
    GLOSSY-only, so `IsDirectLightHitVisible` (CPU + `pgic_funcs.cl`)
    counts glossy depth too: otherwise the floor took the cache AND its
    own light hit (1.8x). e115 cases 4/5 cover it.
  - Scattering media (2026-10, open): `caustic.auto` skips scenes with a
    scattering volume (constant-zero scattering, e.g. absorption-only
    water, does not count). A glass ball in world fog (showcase fogbeam)
    rendered 3.4x a BDPT reference with the cache on (11x seen through
    the ball), 1.08x with it off. Explicit `caustic.enabled = 1` in a
    fog scene still shows the bias - not yet root-caused (e115 case 6
    pins the gate). The add-on leaves the flag unset under
    `caustic_mode = auto` for the path engines so this gate applies.
  - Deferred gen-1 (`initialUpdatePending`): the first cache is
    launched by the first `Update()` call, after ingest mode is
    resolved — GPU sessions build generation 1 purely from device
    records while rendering starts immediately.
  - Generation refresh fix: `CompilePhotonGI` rebuilds
    `compiledPathTracer.pgic` (traced-path normalization, shrunk
    radius) every swap, but nothing pushed it to `taskConfig` —
    devices kept startup values forever, and only thread 0 ran
    `InitPhotonGI`/`SetKernelArgs` so the second OCL thread kept a
    stale cache. The render loop now detects the swap per-thread
    via `GetCausticPhotonPass()` and refreshes `taskConfig.pgic`
    (preserving the deposit session fields) + re-uploads query
    buffers on every thread.
  - New properties: `path.photongi.deposit.capacity` (per-kind
    append-buffer size, default 262144) and
    `path.photongi.deposit.taskfraction` (fallback tail fraction,
    default 0.2).
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
- Photon culling: ~~reject deposits outside the camera frustum /
  film footprint~~ - retracted for caustics (2026-10): a culling
  test must use the eye side's own coverage (refraction included)
  or it deletes energy. A coverage set built from actual eye-side
  cache queries would bring the capacity saving back safely.
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

## Multi-interface chains and the CPU viewport (2026-10, 2.11.33)

- **Photon depth follows caustic.auto.** The core default
  `path.photongi.photon.maxdepth = 4` leaves three delta interfaces
  before the landing vertex. The eye side cuts the light hit the cache
  replaces, so every longer chain rendered dark: a floor under three
  glass slabs (six interfaces) came out 0.875x the reference on
  PATHCPU. When `caustic.auto` turns the cache on, an unset depth is
  raised to 16, the value the Blender add-on already exported.
  PATHOCL deposits through its light tasks and was not affected.
  The radius probe (`Film2SceneRadius`) caps its eye depth at 4, so
  the auto radius and the probe cost stay as before.
  Regression: `dev-tools/e116_multi_interface_caustic_test.py`.
- **RTPATHCPU builds the cache in the background.** `caustic.auto`
  (and the add-on's CPU viewport) now enables the cache on RTPATHCPU:
  - Its render threads never enter the cache barrier. The draw
    thread polls in `UpdateFilmLockLess()` (`PhotonGICache::PollUpdate`),
    and the swap runs with the threads parked on the engine's own
    pause barrier. Later generations resume without a film or
    sampler restart. The first one restarts the frame, because the
    samples before it path-traced the SDS paths.
  - The auto radius probe moves into the first background
    generation, so viewport starts and rebuilds do not wait for it.
  - `BeginSceneEdit` joins the worker, since the trace reads the
    scene (the camera too). Camera-only edits keep the live
    generation; any other edit creates a fresh cache.
  - Indirect caches and `updatespp = 0` stay off in RTPATHCPU.
    RTPATHOCL has no swap protocol yet.
  - Measured with `dev-tools/e117_viewport_caustic_cache_test.py`
    (96x96, three slabs, M5 Pro):
    - First frame: 14-35 ms.
    - First generation: ~0.8 s.
    - Steady floor: 1.002x the reference. With photon depth 4 it is
      0.874x, so the cache carries the floor.
    - Camera-edit latency: 10.2 ms with the cache, 10.7 ms without.
    - Material edit: 11 ms (it was 1.0 s with a synchronous probe).
  - At 960x720 the pool's first generation lands in 3-4 s on an idle
    machine.

## Pool SDS verification (2026-10)

`scenes/pool` (sun + sky2, water `glass`, matte pool), zero config
(only `path.photongi.caustic.auto` resolving the cache on), 256x150,
bands = mean RGB of image rows. Unbiased references need deep path
limits (`pathdepth.* = 40`): the default diffuse depth truncates the
pool's 0.7-albedo inter-reflection, and the cache (which skips the
water -> light hop) then reads +7% above a truncated reference while
matching a deep one.

| sky only, deep, CPU | ceiling | wall | floor-far | floor-mid | floor-near |
|---|---|---|---|---|---|
| PT, no cache (150 s) | 3367 | 2376 | 2412 | 1872 | 1813 |
| cache (150 s) | 3367 | 2374 | 2406 | 1880 | 1828 |

| sun + sky, deep | ceiling | wall | floor-far | floor-mid | floor-near |
|---|---|---|---|---|---|
| GPU no cache (600 s, reference) | 8737 | 6888 | 8357 | 8002 | 7716 |
| GPU zero-config (120 s) | 8743 | 6937 | 8369 | 8090 | 7816 |
| CPU zero-config (120 s) | 8767 | 6921 | 8415 | 8312 | 7981 |
| before the fixes, GPU cache, 512x300 (900 s) | 7283 | 5217 | 5675 | 4413 | 2262 |

The last row is the default-depth run at 512x300 (bands are
resolution-normalized means; the default depth also truncates, so the
pre-fix deficit vs the deep reference is slightly overstated, but the
0.29x near floor is the frustum + eta^2 loss). Regression:
`dev-tools/e115_pool_sds_caustic_test.py` (zero-config CPU/GPU pool
floor vs a deep no-cache reference, CPU/GPU parity).
