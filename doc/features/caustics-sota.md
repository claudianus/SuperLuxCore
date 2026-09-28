# Caustics SOTA program — unified coverage across engines

Status: **Stage A landed (media-transparent specular chains)** —
see §Stages. Regression: `dev-tools/e54_media_caustic_test.py` (all 6
gates PASS), scenes `scenes/cornell/cornell-vol-caustic*.scn`,
`cornell-vol-pure.scn`; visual: `dev-tools/e54_visual_demo.py` +
`cornell-vol-caustic-show.scn` (1280×720, PATHOCL, AgX punch).

## Goal

One caustic-quality bar on every estimator — `PATHCPU`, `PATHOCL`
(Metal/Vulkan), `BIDIR*`, light tracing — including the hard classes:
through-glass caustics, rough-glass boundary, **participating media**
(multi-scatter), directional lights, and eventually arbitrary specular
chains at production speed. Unbiased stays the default: biased
estimators (photon caches) are consistent and confined to their class.

## Estimator coverage map (today)

| Path class | Eye pass | Light pass | Media |
|---|---|---|---|
| L S* D E surfaces | NEE/MIS + MNEE (+ photon cache) | LT + LMNEE + caustic photons | n/a |
| L S* D E in media | phase sampling only | LT + caustic photons (1st scatter) | Stage A: full scatter chains |
| boundary-glossy | eye-owned fireflies (fixed threshold) | adaptive partition moves to LT | same rule applies |

## Stage A — media-transparent specular chains (landed)

The classification rule every estimator shares: **a medium scattering
vertex is transparent to the specular chain** — it neither extends nor
breaks it. Physically: a focused beam does not stop being focused by
crossing a fog volume; the caustic class is a property of the
non-medium surface events.

Concrete consequences, applied identically on CPU (`pathinfo.cpp`) and
GPU (`pathinfo_funcs.cl`, `pathoclbase_*`):

- `LightPathInfo::isNearlyS` / `isAdaptiveS`: tail condition gains
  `|| bsdf.IsVolume()`; a depth-1 medium vertex leaves the chain
  vacuously alive (same as an empty prefix). `firstVertex*` records the
  first **non-medium** vertex — it is the light-adjacent terminal whose
  lobe the eye path would have to hit.
- `EyePathInfo::isNearlyCaustic` / `isAdaptiveCaustic`: same
  transparency in the tail; the depth-1 receiver test is unchanged
  (a medium vertex is a valid non-delta receiver).
- New `causticHasSurface` flag (both structs, CPU + OCL): true once
  the prefix contains any non-medium vertex. Pure-medium paths have no
  focusing surface and stay eye-owned — without this guard the
  vacuously-alive chain would mark ambient in-scattering as caustic.
- Adaptive terminal test: a pending terminal that *is* a medium vertex
  counts as hard (its phase lobe can never aim at a small light);
  pure-medium prefixes short-circuit via `causticHasSurface` /
  `firstVertexSeen`, so both sides agree the path is eye-owned.
- PhotonGI caustic deposits: the gate is now
  `depth>0 && IsSpecularPath() && firstVertexSeen` — multi-scatter
  deposits inside media land in the caustic cache (was: first medium
  vertex only). The caustic-fill early-out keeps the same predicate, so
  `light→medium→specular→medium` chains survive to deposit too.

Disjointness is preserved because the rule is a pure function of the
path events evaluated identically on both sides — the same contract
that keeps the adaptive partition unbiased.

### Validation (Stage A, Apple M5 Pro)

`e54_media_caustic_test.py` 320×180 @96spp, PATHCPU/PATHOCL/BIDIRCPU:

| Gate | Result |
|---|---|
| CPU partition disjoint (vol caustic) | mean off=0.0075 on=0.0075 |
| GPU parity (vol caustic, adaptive+LT) | cpu=0.0075 gpu=0.0077 |
| Pure-medium control (no caustic claims) | off=0.2810 on=0.2809 |
| PhotonGI caustic cache (finite, sane energy) | 0.0069 vs plain 0.0075 |
| BIDIRCPU smoke on volume caustic | finite, mean=0.0080 |
| Surface-only non-regression (rough-glass) | off=0.0007 on=0.0008 |

Visual: `e54_visual_demo.py` → `/tmp/e54_media_caustic/` —
`cornell-vol-caustic-show.scn` (laser fan through three dispersion
spheres in fog, 1280×720 PATHOCL adaptive hybrid): volumetric shafts +
spectral caustic fans converge under the light-pass side; AgX punch
display transform, correct orientation.

## Stage B/C — planned (from `dev-tools/archive/gpu_caustics_design.md`)

- **B1 GPU photon shooting**: wavefront photon-trace kernels reusing
  the PhotonGI layout; CPU keeps BVH build per pass. Gate: slab caustic
  parity vs CPU + measured speedup.
- **B2 photon beams for volumes** (Jarosz'11, UPBP): light-path
  specular-prefix segments; eye gathers beam density along the medium
  traversal. The real fix for thin focused shafts where point deposits
  are too sparse. Volume-only first.
- **B3 dual-field product guiding** (own research): light-side exitant
  vMF field trained like M2b-2 records; eye×light product proposal at
  specular-adjacent bounces; MNEE stays the exact fallback.
- **C ReSTIR-BDPT-lite** (Hedström TOG'25): technique-aware GRIS +
  bidirectional hybrid shift + frame-accumulating caustics reservoirs.
  Needs GPU light subpaths (have: GPU LT) + vertex records (have: VC).
- **D MNEE extensions**: multi-start over pruned candidates (Spoly
  evaluation outcome), chain cache for `maxspecular>=2`, clear-medium
  pass-through in `bsdfConn.IsVolume()` gates.

## Consistency contract (engines)

- The classifier is the contract: any change to chain rules lands on
  `pathinfo.cpp` AND `pathinfo_funcs.cl`/light-path kernel mirrors in
  the same commit — CPU/GPU divergence is a parity bug, not a choice.
- `e54_media_caustic_test.py` renders the suite on
  PATHCPU/PATHOCL/BIDIRCPU and asserts whole-image mean agreement
  inside noise plus a pure-medium no-claims control; it is the
  standing gate for every stage above.

## References

- Hedström et al., *ReSTIR BDPT* (TOG 2025) — stage C blueprint.
- Grüschloss et al. (Weta), *Practical Caustics Rendering with
  Adaptive Photon Guiding* — production surface+volume caustics.
- Jarosz et al., *Progressive Photon Beams* (SIGA 2011) — stage B2.
- Zeltner et al., *Specular Manifold Sampling* (SIGGRAPH 2020) —
  MNEE evolution reference.
- Stamm et al., *Photon-Driven Neural Radiance Caching* (I3D 2026,
  SPPC) — photon targets for guiding fields.
