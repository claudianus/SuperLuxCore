# Heterogeneous volume null-collision tracking — delta/ratio tracking over a majorant grid

Status: **functional on CPU (`PATHCPU`) and GPU (`PATHOCL`,
Metal-validated)** — regression coverage in
`pyunittests/pysuperluxcoreunittests/tests/materials/testvolumetracking.py`.

## What and why

`HeterogeneousVolume` used to integrate medium interactions with jittered
fixed-step ray marching (`steps.size`/`steps.maxcount`, default 1.0/32).
Every step evaluates the full texture stack (sigma_a, sigma_s, emission)
plus three `exp()` calls, and — worst — the step count scales with the
segment length, so rays through large or mostly-empty volumes burn most of
their time evaluating zero density. Shadow rays paid the same march for
what is effectively a binary visibility decision.

The new implementation replaces the fixed-step walk with the standard
null-collision formulation used by production renderers (PBRT-v4,
RenderMan, Mitsuba 2):

- **Residual delta tracking** (minorant decomposition applied to
  collision sampling; Novák et al. 2014) for free-flight sampling on
  camera/light path segments: each cell's density is split as
  `sigma_t = sigma_c + sigma_r` where `sigma_c` is the per-cell
  **minorant** (a conservative lower bound). The minorant component is a
  homogeneous exponential stream of real collisions — sampled
  analytically with zero texture evaluations — while residual
  candidates arrive at rate `maj - sigma_c` and are accepted with
  `(sigma_t - sigma_c) / (maj - sigma_c)`. The two Poisson streams
  reproduce the exact analog `sigma_t * T` collision marginal, so the
  estimator stays unbiased with the same vertex weight convention
  (`sigma_t,lambda / sigma_t,f` times the spectral correction `R`).
  Cells where `sigma_c ~ maj` (dense, smooth media) produce almost no
  residual candidates: this is the regime where plain delta tracking
  was slowest, so the gain is largest exactly where it was needed.
  Overestimated minorants are compensated by a rate-ratio weight
  correction (Galtier et al. 2013). On the Ember Plume showcase the
  residual vertex tracker cut the 720p GPU render from 27.8s to 23.6s
  (-15%, cumulative -31% vs the original plain delta tracker).
- **Residual ratio tracking** (Novák et al., SIGGRAPH Asia 2014) for
  shadow-ray transmittance: instead of a binary "scattered / not
  scattered" event, `Volume::TransmittanceEstimate()` returns a smooth
  unbiased estimate of `T = exp(-int sigma_t)`. The grid stores a
  per-cell **minorant** `sigma_c` (a conservative lower bound on
  `sigma_t`) used as the control extinction:
  `T = prod over cells of [ exp(-sigma_c * l) *
  prod over candidates of (1 - (sigma_t - sigma_c) / (maj - sigma_c)) ]`.
  Cells where the density is nearly constant (`minorant ~ majorant`,
  e.g. the interior of dense media) degenerate to a deterministic
  exponential: zero texture evaluations and zero variance. Unbounded
  texture types simply use `sigma_c = 0` and fall back to plain ratio
  tracking.
- **Per-cell majorant grid + 3D DDA traversal**: a coarse grid (default
  32^3 over the volume domain, `scene.volumes.<name>.majorantres`) stores a
  conservative per-cell `sigma_t` bound, so the tracker takes exponential
  steps through empty cells at majorant 0 instead of dense evaluation.

## API

```
scene.volumes.<name>.tracking    = delta | march   (default delta)
scene.volumes.<name>.majorantres = <uint>          (default 32, cubic cells
                                                  along the largest axis)
scene.volumes.<name>.phase       = schlick | hg    (default schlick; both
                                                  homogeneous and
                                                  heterogeneous volumes)
```

`march` keeps the legacy fixed-step path intact (`steps.size`,
`steps.maxcount`) as a debugging/fallback mode.

```
scene.volumes.<name>.distancesampling = equiangular | transmittance
                                       (homogeneous volumes only,
                                       default equiangular)
```

## Distance sampling: equiangular + transmittance MIS

For **homogeneous** volumes the free-flight pdf
`sigma_s * exp(-sigma_s * t)` is analytic, so it can be mixed exactly
with the equiangular distribution (Kulla & Fajardo, EGSR 2012), which
concentrates scattering vertices in the glow around a point-like light
(`p(t) ~ D / (D^2 + (t - delta)^2)` over the segment, where `delta` is
the light's projection on the ray and `D` its distance to the ray).

`Scene` caches the world positions **and powers** of position-defined
lights (`equiangularLightPoints` / `equiangularLightLuminances`: point,
mappoint, sphere, mapsphere, spot, projection, laser — filled in
`Scene::Preprocess`). For every camera / GI segment inside an eligible
`HomogeneousVolume`, `Scene::Intersect` calls
`HomogeneousVolume::ScatterEquiangular`, which spends the first draw of
the `passThrough` event stream on a **contribution-aware** light pick:
`w_i = lum_i * (thetaB_i - thetaA_i) / D_i` — the light's angular
footprint on the segment (the integral of its `1/(D^2 + x^2)` glow
mass), times its power. The next two draws form a one-sample MIS:
50% equiangular vertex, 50% transmittance vertex (or pass-through,
which only the transmittance strategy can produce). The vertex weight
stays `sigma_t * T(t) / p_mix(t)` exactly as the legacy `Scatter`, so
all downstream code is unchanged. The GPU kernels
(`HomogeneousVolume_Scatter` in `volume_funcs.cl`) mirror the same
weighted pick and MIS; positions and powers ride in `eqLightPoints`
as float4 (xyz + power).

Two subtleties worth recording:

- The MIS is **conditioned on the picked light**: the light selection
  probability `q(l)` multiplies *both* strategies' joint densities and
  cancels in the balance heuristic — including `q` in only the
  equiangular pdf biases the estimator whenever more than one light is
  cached.
- The pass-through event belongs to the transmittance strategy only, so
  its density is `0.5 * exp(-sigma_s * L)`; an earlier version of this
  code conflated "equiangular strategy chosen" with "collision
  happened" and silently dropped half of the transmittance-strategy
  vertices (systematic ~5% darkening, caught by the mean-agreement
  regression test).

Heterogeneous volumes keep the delta tracker. **Negative result (measured
and reverted):** an equiangular partner for heterogeneous *path vertices*
was implemented via the majorant free-flight density `sigma_bar * T_bar`
(analytic per cell) as the transmittance-strategy proposal plus a
ratio-tracked `T_hat` weight correction — a provably unbiased estimator.
Across five regimes (thin wisps, moderate/dense gaussian clouds,
embedded point light) it was consistently 5-50% *worse* in MSE than the
residual delta tracker, never better. The reasons are structural:

- The analog delta tracker's vertex marginal *is* `sigma_t * T` — the
  ideal unguided density — so equiangular can only re-weight, not
  improve, vertex placement; half of its draws then land in empty or
  low-density space where `sigma_t ~ 0` and the vertex is wasted.
- The weighted estimator multiplies the vertex weight by the
  multiplicative ratio-tracked `T_hat`, whose variance compounds —
  while the analog vertex weight stays ~1.

This is the same reason PBRT-v4 applies equiangular sampling in
heterogeneous media only to NEE virtual samples, never to path-vertex
placement. GPU kernels (`PATHOCL` etc.) fall back to the transmittance
sampler unchanged, so CPU and GPU still converge to the same image.

`distancesampling = transmittance` disables the feature per volume
(homogeneous volumes only).

Measured on a dense-fog + embedded-point-light scene (PATHCPU, 64 spp):
MSE vs a converged reference is ~1.6x lower in the bright halo region
and ~1.2x lower overall.

## Phase function: Henyey-Greenstein

`phase = hg` switches `SchlickScatter` from the Schlick approximation
(`k = 1.55 g - 0.55 g^3`) to the exact Henyey-Greenstein function
`(1 - g^2) / (4 pi (1 + g^2 - 2 g cos)^1.5)`, evaluated, sampled by
closed-form inversion, and pdf'ed consistently in `Evaluate`, `Sample`
and `Pdf` (CPU and OpenCL/Metal alike). `g` is clamped to
`[-0.999, 0.999]` because HG is singular at `|g| = 1`. LuxCore's
`localEyeDir` is reversed vs. the standard scattering-angle convention,
so the implementation flips the sign of `cos` throughout (sampling and
evaluation stay consistent). As in the Schlick path, sampling and pdf
use the filtered scalar `g` while `Evaluate` returns the spectral value.
Schlick remains the default for backward compatibility; both modes are
normalized phase functions, so results converge to nearly the same
image (they differ only in the approximated kernel shape).

## Weighting and unbiasedness

The tracking channel is the spectrum filter `sigma_t,f =
Filter(sigma_a + sigma_s)`; a per-channel correction `R` keeps the result
unbiased for colored media (Kutz et al., SIGGRAPH 2017 — spectral tracking
without hero wavelengths):

- null candidate: `R *= (maj - sigma_t,lambda) / (maj - sigma_t,f)`
- accepted collision: `R *= sigma_t,lambda / sigma_t,f`

`w` is the Galtier/Coleman weighted-delta-tracking correction
(`w *= sigma_t,f / maj` when the majorant underestimates `sigma_t,f`),
which keeps procedural textures unbiased even when the sampled bound is
too low.

`Scene::Intersect()` routes shadow rays to `TransmittanceEstimate()`;
clear and homogeneous volumes use the analytic `exp(-sigma_t * L)`.

## Majorant construction

`Scene::PreprocessVolumes()` (in `scenepreprocess.cpp`) runs during
`Scene::Preprocess()` after geometry and materials are final. The grid
domain is the union of the bounding boxes of objects using the volume as
interior; world/exterior/camera volumes get the scene bbox. Per-cell
bounds come from `TextureBoundInBoxOrSampled()`:

- `DensityGridTexture::GetMaxInWorldBBox()` computes the exact bound from
  voxel maxima (affine mappings only — UVW box corners are mapped and the
  covered voxel range is scanned; wrap modes BLACK/WHITE/CLAMP/REPEAT are
  handled). `GetMinInWorldBBox()` is the conservative counterpart used
  for the minorant cells: it never scans fewer voxels than needed, so it
  can only underestimate — which is the safe direction for a control
  extinction.
- Constant textures and simple arithmetic (`scale`, `add`, `subtract`,
  `mix`, `abs`, `clamp`) are bounded recursively — both the majorant and
  the minorant.
- Other procedural textures fall back to a 3x3x3 sampled estimate with a
  1.25 safety margin (majorant only); the minorant is 0 since a sampled
  estimate cannot prove a lower bound. Residual underestimation is
  corrected at render time by the `w` weight above.

Outside the grid domain, `globalMajorant`/`globalMinorant` apply (exact
bounds for grid-backed textures, cell extrema / 0 otherwise). A
zero-cell or zero-domain majorant segment is skipped without evaluation.

The serialized GPU buffer stores each cell as an interleaved
`(minorant, majorant)` float pair in `CompiledScene::volMajorants`.

## Notes / gotchas discovered while integrating

- `SchlickScatter::GetColor` (CPU `volume.cpp`,
  `materialdefs_funcs_heterogeneousvol.cl`) returned albedo **1** when
  `sigma_s = 0`. Under the legacy sigma_s-rate sampler that branch was
  dead code (no scatter vertex is ever created where sigma_s = 0), but
  sigma_t-rate delta tracking *does* create vertices at absorption-only
  points — where they behaved as perfect scatterers and produced a fake
  fog filling the whole volume. Fixed to return 0 when `sigma_a > 0`.
- `Spectrum::Black()` requires an exact 0; `exp()` underflow needs
  thousands of steps at typical densities, so transmittance marches now
  early-out on `Filter() <= 1e-4`.
- On Metal (cl2msl), pointer parameters need an explicit `thread` address
  space; `VolMajorantWalk` was added to the translator's type list
  (`src/slg/utils/cl2msl.py`).
- The majorant cells are cubic: `cellSize = maxExtent / majorantres` and
  `res[i] = ceil(extent[i] / cellSize)`, so the grid may extend slightly
  past `majorantBBox` on the shorter axes. The GPU kernel uses the
  serialized `majorantCellSize`; CPU and GPU must stay consistent (fixed
  after an initial `extent.x / resX` mismatch).

## GPU plumbing

- `HeterogenousVolumeParam` gained `deltaTracking`, the grid domain
  (`majorantBBox*`), `majorantCellSize`, `majorantRes*`, `majorantOffset`,
  `globalMajorant`/`globalMinorant` and `phaseFunc`
  (`material_types.cl`); `HomogenousVolumeParam` gained `phaseFunc`.
- `CompiledScene::volMajorants` concatenates all volume cell arrays;
  `volMajorantsBuff` is allocated in `pathoclbaseoclthreadinit.cpp` and
  bound as the new `TEXTURES_PARAM` tail (`texture_types.cl`) in
  `pathoclbaseoclthreadkernels.cpp::SetAdvancePathsKernelArgs`, which every
  OCL engine (PATHOCL/TILEPATHOCL/RTPATHOCL) routes through.
- `volume_funcs.cl` mirrors the CPU code: `VolWalk_*` (DDA),
  `HeterogeneousVolume_DeltaTrackScatter`,
  `HeterogeneousVolume_RatioTrackTransmittance`,
  `Volume_TransmittanceEstimate`; `scene_funcs.cl` calls the transmittance
  path for shadow rays.

## Measured results (Apple M5 Pro)

Sparse-wisp densitygrid scene (64^3 grid, ~90% empty space), 640x480,
32 spp, PATHCPU:

| mode  | time | samples/sec |
|-------|------|-------------|
| march | 5.07s | 2.0 M |
| delta | 3.04s | 3.4 M |

Camera-inside-volume variant: march 6.59s -> delta 3.05s (~2.2x). The
speedup grows with emptier domains and finer legacy step sizes; on
VDB-class sparse data (where most of the domain is empty) the gain is
largest, and the shadow-ray noise drop from ratio tracking compounds it.

## References

- Woodcock et al., "Techniques used in the GEM code for Monte Carlo
  neutronics calculation", 1965.
- Novák et al., "Residual Ratio Tracking for Estimating Attenuation in
  Participating Media", SIGGRAPH Asia 2014.
- Henyey & Greenstein, "Diffuse radiation in the galaxy", 1941.
- Novák et al., "Monte Carlo Methods for Physically Based Volume
  Rendering", SIGGRAPH 2018 course.
- Kutz et al., "Spectral and Decomposition Tracking for Rendering
  Heterogeneous Volumes", SIGGRAPH 2017.
- Miller et al., "A Null-Scattering Path Integral Formulation of Light
  Transport", SIGGRAPH 2019.
- Kulla & Fajardo, "Importance Sampling Techniques for Path Tracing in
  Participating Media", EGSR 2012 (equiangular sampling).
- Galtier et al., "Integral formulation of null-collision Monte Carlo
  algorithms", 2013.
- PBRT-v4 `DDAMajorantIterator`/`MajorantGrid` (Pharr, Jakob, Humphreys).
