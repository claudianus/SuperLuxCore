# Performance ledger — ranked bottleneck book

Living document for the measurement-gated optimization cycle defined in
`dev-tools/megaplan-production-sota.md`. Each entry records: hotspot,
call path, measurement source + share, hypothesis, correctness
constraints, A/B result, decision, follow-up.

Rules:
- Measure on an idle host (gauntlet prints load warnings; `uptime`
  loadavg should be roughly < core-count before trusting timings).
- Interleaved A/B, min-of-3; a <2% delta is noise unless confirmed on
  repeated runs.
- Anything touching sampling weights / pdfs needs e26-style
  unbiasedness + cpu/gpu parity gates, not just timing.
- Bit-identical transforms are preferred; statistically-identical
  transforms need a stated justification.

## CPU render-thread profile (2026-09, PATHCPU, Apple Silicon)

Sorted by profiled share of render-thread time.

| # | Hotspot | Share | Status | Commit |
|---|---------|-------|--------|--------|
| 1 | Sampler hashing (Sobol/Metropolis) | ~6% | landed | `77368bfc6` |
| 2 | PhotonGI retrace worker contention | ~5.5% | landed (threads knob + saturation backoff) | `023952789`, `8f52f6ffa` |
| 3 | libm transcendentals on hot callers | ~2.5% | landed | `cd568c328` |
| 4 | Spectral `ProjectToRGB` per-field CIE/whitepoint | ~1.5% | landed | `e25c8ffc6` |
| 5 | `DataSet::GetAccelerator` per ray segment | ~0.3% | landed | `e25c8ffc6` |
| 6 | LightBVH `NodeImportance` trig chain | ~0.1% | landed | `e25c8ffc6` |

## CPU render-thread profile (2026-09-30 r2, PATHCPU, portal-interior 640x360)

Portal-interior (sealed-indirect worst case) re-profile after the first
backlog drained. Shares are top-of-stack on render threads.

| # | Hotspot | Share | Status | Commit |
|---|---------|-------|--------|--------|
| 1 | Embree tri+instance traversal | ~20% | natural cost | — |
| 2 | `SobolSampler::InitNewSample` geometry+adaptive | ~11% | **landed** (film cache + magic udivs, -43% leaf) | `537a48c6d` |
| 3 | `SobolSequence::GetSample` ctz-walk | ~8% | residual | — |
| 4 | `MetropolisSampler::GetSample/NextSample` | ~8% | residual (MLT mutation loop amortized) | — |
| 5 | `sincosf` latlong/disk/cone sampling | ~6% | no redundant calls identified; approx breaks parity | — |
| 6 | HitPoint attr chain (`Init`+`GetDifferentials`+interpolate+Buffer[] stubs) | ~9% | open | — |
| 7 | PathVolumeInfo bookkeeping | ~3% | open (has-volumes fast gate) | — |

## CPU render-thread profile (2026-10-01 r3, PATHCPU, prism-conservatory 640x360)

Volume+custic scene after the r2 backlog drained. Shares are
top-of-stack on render threads (35s `sample`, idle cvwait excluded).

| # | Hotspot | Share | Status | Commit |
|---|---------|-------|--------|--------|
| 1 | Embree tri+instance traversal | ~35% | natural cost | — |
| 2 | `MetropolisSampler::GetSample` mutation walk | ~12% | residual (amortized per-stamp) | — |
| 3 | `SobolSampler::InitNewSample` pick loop | ~11% | residual | — |
| 4 | `HomogeneousVolume::Scatter*` HitPoint+virtual tex evals | ~6% | **landed** (const-param fast path) | `242ec2765` |
| 5 | HitPoint attr chain | ~10% | open (as r2) | — |
| 6 | libm `__sincosf/atan2f/expf` on samplers/env lights | ~6% | no redundant calls; approx breaks parity | — |
| 7 | `PathVolumeInfo` bookkeeping | ~3% | **landed** (no-volume fast gates + GPU NULLMAT parity) | `0dd8d6ee7` |
| 8 | LightBVH `NodeImportance`+`SampleLights` | ~3% | already dot-space/trig-free | — |


### r6 PathVolumeInfo fast gates (`0dd8d6ee7`)

- Path: `Scene::Intersect` per-hit `BSDF::Init` volume resolution +
  `ContinueToTrace`/`Update` bookkeeping; GPU twins in
  `pathvolumeinfo_funcs.cl`/`bsdf_funcs.cl`/`scene_funcs.cl`.
- Observation: the volume state machine ran unconditionally on every
  hit even when no volume could possibly be resolved (majority of
  scenes). GPU additionally paid a NOT_INLINE eval-stack round-trip
  per `Material_Get*Volume` call.
- Change: `Material::{HasAnyVolume,CanHaveInteriorVolume}` predicates;
  `PathVolumeInfo::IsIdle`; early-outs in `ContinueToTrace`,
  `SetHitPointVolumes`, `Update`; callsite gates in
  `BSDF::Init`/`Scene::Intersect`. GPU mirrors + evalOpLength==1
  direct-return shortcut in `Material_Get*Volume`.
- Parity fix folded in: GPU `PathVolumeInfo_ContinueToTrace` was
  missing the CPU twin's NULLMAT requirement on condition #2
  (upstream `d4e4a310d` semantic) - media relmean vs CPU improved
  13.1 -> 7.1 (64spp).
- Validation: parity-regression 4/4; e_volfastgate_parity.py
  (volumeinfo/media/luxball-vol/juice/cornell) CPU/GPU means within
  4%, no non-finite pixels; pre-existing media/volumeinfo relmean
  noise confirmed identical on pre-change binary.
- Timing: interleaved 4x20s A/B, shared host with background load -
  cornell median 7.506 -> 7.567 Ms/s (+0.8%), media 10.819 ->
  10.664 (-1.4%); inside run-to-run noise, landed on dead-work
  removal + parity grounds.

### r3 #4 Volume const-param cache (`242ec2765`)

- Path: `HomogeneousVolume::Scatter`, `ScatterEquiangular`,
  `TransmittanceEstimate` — per volume event.
- Observation: each event built a full `HitPoint` (~15 stores) and ran
  3 virtual `Texture::GetSpectrumValue` dispatches to recover values
  that are compile-time constants for a `ConstFloat3`-parameterized
  volume (the near-universal homogeneous fog config).
- Change: cache the clamped sigma_a/sigma_s/emission spectra at
  construction when all three textures are `CONST_FLOAT`/`CONST_FLOAT3`
  and the SSS albedo parametrization is off; serve them on the
  non-spectral path (`!Spectral::Current()`). Spectral renders keep
  per-path wavelength eval (values are path-dependent there).
- Validation: Release build clean; PATHCPU prism-conservatory 64spp
  finite, distribution matches (bit-compare impossible: PATHCPU
  samplers seed off wall-clock). e94 serialization suite 3/3.


### r2 #2 Sobol InitNewSample film cache (`537a48c6d`)

- Path: `SobolSampler::InitNewSample` per eye sample — adaptive
  convergence test (NOISE channel + second moments) + bucket/pixel
  arithmetic.
- Observation: per sample paid 2 `std::set::count` channel lookups,
  ~8 runtime `udiv`s (tileSize² mod/div, tiletWidthCount mod/div,
  overlapping div), dead `GetEngineFilm()` ref.
- Change: `UpdateFilmCache()` snapshots subregion-derived geometry,
  magic divisors and channel flags once per subregion change (channels
  frozen post-`Film::Init`; only subregion can still move via dyn-res).
  `floor(2^32/d)+1` mulhi division is exact for every u32 dividend —
  pixel visit order and RNG draws bit-identical.
- Validation: Release build; `parity-regression` 4/4 on final binary;
  portal-interior smoke render clean.
- Result: leaf 42.7k → 24.5k (-43%), sampler group ~104k → ~73.7k
  (-29%) at identical 25s `sample` window.


- Wavefront queues auto promotion — **rejected** (2026-09-30,
  `dev-tools/wf_ab.py`, PATHOCL Metal, 1280x720, 25s, min-of-2
  interleaved). Unstable in both directions: cornell on=0.27 vs
  off=5.41 Ms/s (stall-class collapse, same signature as the known
  stale-totals 6x regression), classroom on=0.65–10.67 vs off=7.18,
  focused-ring on=2.34–7.44 vs off=5.58, luxball -17% consistently.
  The occasional wins (classroom rep0 +49%, focused-ring rep1 +33%)
  are real but the collapse mode (tasks sitting in queue tails
  beyond stale launch sizes) is a correctness-adjacent stall —
  `pathocl.wavefront` stays opt-in until the stall mode is root-caused
  and fixed. Diagnosis path: instrument queue-totals readback timing +
  per-state launch sizes on Metal.
- GPU crawl-bail for the wavefront MNEE state machine — corrupts state
- Candidate-prefilter round — measured no-win (`24599e018`).

## Entry details

### #4 Spectral RGB projection (`e25c8ffc6`)

- Path: `PathTracer::ProjectSampleResultToRGB` → `Spectral::ProjectToRGB`
  × ~15 `SampleResult` fields.
- Observation: per field the old code re-sampled 9 CIE SPD lookups and
  re-normalized the sampled white point — all functions of the drawn
  wavelengths only.
- Change: `Spectral::RGBProjector` prepared once per sample
  (`PrepareRGBProjection`), per-field projection reduced to dot +
  `ToRGB`; black-spectrum early-out is exact (`ToRGB(0)=0`).
- Constraint: `nY<=0` invalid path preserved (zero spectrum, no early
  return skipping field init); `aliveMask` respected inside the
  projector.
- Validation: Release build, parity-regression 4/4, spectral render
  mean/luminance unchanged.

### #5 `Scene::Intersect` accelerator lookup (`e25c8ffc6`)

- Path: CPU `Scene::Intersect` `for(;;)` shadow-transparency loop.
- Observation: `dataSet->GetAccelerator(ACCEL_EMBREE)` is a
  `std::map::find` executed per ray *segment* — multi-segment hits
  (glossy transmittance, volumes) multiplied the lookup.
- Change: resolve once per call before the loop. No semantic change.

### #6 LightBVH `NodeImportance` dot-space (`e25c8ffc6`)

- Path: light sampling `SampleLights`/`SampleLightPdf` descent,
  CPU `NodeImportance` + GPU `LightBVH_NodeImportance`
  (`lightbvh_funcs.cl`) — kept identical by contract.
- Observation: per evaluation `asin`+2×`acos`+2×`cos` (~5 libm calls).
- Change: `thetaO` cos/sin baked into `LightBVHNode` at build (+8B/node).
  `cos(max(0,acos(d)-t)) == 1` when `d >= cos(t)`, else
  `d·cos(t) + sqrt(1−d²)·sin(t)`; `thetaO+thetaB >= PI` cover-case
  detected as `cO <= 0 && sB >= sO` (sin monotonic on [π/2,π] domain,
  handles omni `thetaO=PI`). Zero transcendentals per call now.
- Constraint: E&K'18 bound must stay conservative — the identity is
  exact, the `cosO >= cBO` gate preserves the inside-cone → 1 case, and
  the `Max(0,·)` wrap preserves the >π/2 → 0 clamp.
- Validation: `e26_lightbvh_test.py` 10/10 — unbiasedness vs LOG_POWER,
  bounded RMSE (0.0017 < flat 0.0027), cpu-gpu parity, finite outputs.
- Timing: ~0.1% profile share, below run-to-run noise without idle
  host; landed on correctness-neutral cost-reduction grounds.

## Backlog (ranked candidates for next rounds)

From gauntlet v2 + audit (`dev-tools/sota-acceleration-audit.md`):

- Sampler hashing residual (~6% → post-`77368bfc6` residual share
  needs re-measure on idle host).
- PhotonGI retrace CPU worker — consider SIMD / batch bvh queries;
  verify `LUX_PGIC_UPDATE_THREADS` tuning default.
- Wavefront queues: still opt-in/auto-off; re-evaluate per-workload
  after queue-fill improvements (see `wavefront-queues.md`).
- GPU PhotonGI deposits → full GPU cache update path (B-series).
- Spectral: 4-bin → wider coverage / n,k table-driven IOR; M6 item.
- `maxDepth`/device-scheduling heuristics for auto mode (M4 gate).

### r4 dispatch-population + taskCount sweep (2026-10-01)

- Added `LUX_TASKSTATE_DUMP` (`pathoclopenclthread.cpp`): per-batch
  state histogram read from `tasksStateBuff` after FinishQueue.
  Cornell PATHOCL 720p dense: ~334K tasks in `RT_NEXT_VERTEX` +
  ~190K in `RT_DL`; wavefront splits them across
  `HIT_OBJECT`/`DL_SAMPLE_BSDF`/`GEN_NEXT_RAY`/etc.
- `opencl.task.count` sweep (cornell/multi-caustic 1280x720):
  64K vs 512K AUTO lands inside ±12% run-to-run noise on M5 Pro -
  AUTO stays. More tasks do not buy GPU occupancy past ~64K on
  these scenes; the per-kernel no-op dispatch tax scales with
  taskCount but remains sub-noise.
- Wavefront re-A/B: cornell 720p dense 11.0 Ms/s vs wavefront
  5.7 Ms/s (no collapse, just slower). Opt-in stays.
- Apple `opencl.gpu.use=1` duplicate device bug fixed
  (`07d856585`): OpenCL GPU + Metal GPU were both selected,
  spawning a second render thread that crashed inside Apple's
  OpenCL->Metal shim (gldExecuteKernel null-deref). Parity suite

### r5 sampleResults keep-sized + pendingTotal wiring (2026-10-01)

- Path: `PathTracer::RenderLightSample` → `AddLightSampleResult` →
  splat loop; `FilmSamplesCounts::AddSampleCount`; Metropolis
  accept-path `currentSampleResults = sampleResults`.
- Observation: `sampleResults.clear()` destroyed every slot's inner
  `SpectrumGroup` vector each light path and `Init()` reallocated it —
  2 heap free/alloc pairs per vertex × ~10M light samples/s.
  `pendingTotal` existed but was never accumulated — the shared
  `total_SampleCountAtomic` still got a `fetch_add` per splat.
  Metropolis accept copied the full `maxPathDepth+2` vector when only
  `used` slots were live.
- Change: light-path `sampleResults` stays at `maxPathDepth+2`;
  `u_int &used` plumbed through `RenderLightSample`, `ConnectToEye`,
  the LMNEE single/multi/tail chain and `ConnectToEyeCallBackType`;
  `Sampler::NextSample` takes a `used` bound (default SIZE_MAX keeps
  other engines unchanged). `pendingTotal` flushed at 64. Accept-path
  element-copies `used` slots (resize-once on first accept).
- Validation: cornell+strands CPU/GPU parity PASS both rounds;
  profile deltas AddSampleCount 19k→12.6k, Metro NextSample
  3.5k→2.2k, `_xzm_free` 2.1k→1.6k.
- Follow-up: `InitNewSample` adaptive re-pick + embree BVH + MLT
  replay are algorithmic floors, not accidental cost — next
  structural win is per-thread splat buffers or SampleResult
  footprint (serialization risk).

### HitPoint shading fusion: correctness gate

- `GetShadingInfo` shares normal/UV inputs with differential evaluation;
  instance and motion CPU meshes retain the transform-aware base path.
- Corrected two fusion regressions: singular UV charts must retain corner
  normals in the static cache; missing GPU UVs must remain `(0, 0)`, not
  the differential-only helper's former arbitrary `(0.5, 0.5)`.
- Cached normal derivatives normalize each corner before differencing,
  matching the uncached CPU contract. GPU fusion likewise normalizes
  transformed corners before differencing.
- Removed the uncalled GPU `ExtMesh_GetDifferentials` helper.
- Release build passed. Actual PATHCPU/Metal PATHOCL renders of a
  smooth-normal emissive plane passed for regular, singular and absent UVs:
  central linear RGB bytes within 2 of `(153, 0, 204)`.
  Nonuniform object scale `(0.5, 1, 2)` produced `(242, 0, 81)` on both,
  matching the normalized inverse-transpose normal within 2 bytes.
  Existing backend parity runner passed all four cases.
- A linked Release C++ probe compared cached fusion against explicitly
  qualified `ExtMesh::GetDifferentials` for raw corner normals
  `(1.2,0,1.6)`, `(0,3,4)`, `(0,0,7)` on a unit UV triangle.
  All position/normal derivative components matched within `1e-6`;
  `dndu=(-0.6,0.6,0)`, `dndv=(-0.6,0,0.2)`.
- Single-triangle, warm-cache microbenchmark (10 million varying
  barycentric hits per pass): fused 8.07/8.15 ns per hit, separate
  interpolation plus cached differentials 8.03/8.12 ns per hit.
  This probe demonstrated no speedup; it does not measure scattered
  normal traffic or end-to-end render throughput.
- Scope limits at this gate: GPU varying-normal derivatives remained
  unverified; reflected/motion interpolation was exercised in the next
  gate below. No speedup or bitwise-equivalence claim.

### Applied handedness: cache invalidation and GPU base-sign composition

- Reproduced with a mesh whose X reflection was baked into positions and
  vertex normals, followed by `Scene.SetMeshAppliedTransformation`.
  Cached static CPU interpolation retained the pre-setter sign.
  `ExtTriangleMesh::SetLocal2World` now invalidates signed-normal cache
  entries only when the handedness changes; no extra per-hit work.
- With GPU native workers disabled, instance and motion shading returned
  `(-0.6,0,0.8)` instead of CPU `(0.6,0,-0.8)`: descriptors lacked the
  base mesh's applied sign. A common per-mesh integer records that sign;
  GPU interpolation and normal derivatives compose it with the
  instance/motion transform sign. Geometry normals already contain the
  base sign and therefore are intentionally unchanged.
- Added `dev-tools/shading-handedness-regression.py`: actual signed float
  SHADING_NORMAL output is checked at every central pixel against an
  analytic normal, including magnitude (no renormalization hiding errors).
  Static, instance and translating-motion wrappers over a baked reflection,
  plus reflected/nonuniform instance and motion transforms, passed:
  five cases on each of PATHCPU and isolated Metal PATHOCL, tolerance `1e-5`.
- GPU isolation requires `opencl.native.threads.count = 0`, not
  `native.threads.count = 0`. Mixed CPU/GPU rendering can dilute an inverted
  GPU normal and is not proof of backend parity. The existing
  `parity-regression.sh` now disables native workers on GPU runs; its four
  cases passed after this change.
- Release build passed. GPU varying-normal derivative behavior is still
  outside the signed interpolation regression's verification scope.

### Normal derivatives: generic bump consumer gate

- `Texture::Bump` perturbs shading normals with `dndu/dndv` before
  evaluating a normal-dependent texture. This gives an observable
  consumer path for testing the differential contract, rather than
  inspecting kernel source or comparing two copies of an implementation.
- Added `dev-tools/bump-differential-regression.py`: three unequal,
  nonunit corner normals, unit UV triangle, and `shadingnormal.x` bump.
  An independent NumPy oracle computes transformed normalized corner
  differences, projected geometry derivatives, finite-difference bump
  slopes and the final oriented cross-product normal at the centre ray.
- Actual signed SHADING_NORMAL outputs passed for static geometry,
  nonuniform scale `(0.5,1,2)` and reflection/nonuniform scale
  `(-0.5,1,2)`, each on PATHCPU and isolated Metal PATHOCL.
  Largest central mean component error: `0.000408`; tolerance `0.002`
  allows differing subpixel hit positions, not an inverted normal.
- No additional renderer change was needed at this gate. Motion-varying
  normal derivatives and native CUDA/OpenCL/Vulkan executions are not
  covered by this Apple Metal run.

### Static shading working-set probe and padding removal

- Linked Release microbenchmark: independent triangles with normals/UVs,
  xorshift-indexed random hits, varying barycentrics, 3 million calls per
  pass, three alternating fused/separate passes for 64/4096/65536/262144
  triangles. Separate evaluation uses interpolation plus the existing
  cached differential path, not an artificially uncached baseline.
- Before padding removal, 262144 triangles: fused 54.17/81.12/88.74 ns
  per hit; separate 104.95/110.44/112.80. Small working sets were noisy
  and did not consistently favor fusion. This supports fusion for
  scattered normal traffic, not an end-to-end renderer speedup claim.
- Removed the unused `TriDifferentialCache::pad` float. Clang's actual
  record-layout dump confirms the resulting private cache is 88 bytes,
  alignment 4 (previous field layout 92 bytes): 4 bytes saved per cached
  triangle, or 8 MiB at the existing 2M-triangle per-mesh cap.
  No derivative arithmetic, serialization fields or GPU layout changes.
- After removal, 262144 triangles: fused 88.58/75.99/94.14 ns per hit;
  separate 119.79/129.90/131.89. Cross-run timing varied substantially;
  no speedup is attributed to padding removal. The memory reduction is
  deterministic, and no extra computation was introduced.
- Release build passed; all 10 handedness and 6 analytic bump render
  cases passed on the rebuilt CPU/isolated Metal module.

### Blender procedural Math semantic cutover

- Native `mathfunc.snap` uses `floor(a/b)*b`, with zero for zero
  increments, replacing the adapter's nearest-multiple rounding.
  Actual scalar/vector Blender export exposed and corrected a missing
  Texture Coordinate branch header (`coord` was undefined in Vector Math).
- Actual clamped Exponent(1) rendering emitted `6.71837` instead of `5`
  after the test's +4 bias: helper returns bypassed the common Clamp
  stage. All supported Math results now reach that stage without an
  identity texture.
- Linked Square Root emitted `5` instead of `4.5` for input `0.25`:
  the generic power helper used `texture1/texture2`, but native power
  reads `base/exponent`. Fixed the schema after the existing fold stage.
- Five signed/boundary render failures against Blender 5.2 shader source:
  Floor(-1)=-2, Ceil(1)=2, Truncate(1.75)=-1, Fraction(-1)=1,
  Round(-1.5)=-2. Added native unary operations and removed the adapter's
  round/shift/sign compositions. Vector Floor/Ceil/Fraction no longer
  warn and pass through.
- The unary cutover initially kept the existing float-conversion wrapper.
  Relative operation-node counts at that stage (excluding input/constant
  leaves): Floor 2→2, Ceil 4→2, Truncate 7→2, Fraction 3→2.
  The typed socket cutover below removes redundant scalar wrappers.
  Constant Round stays native to preserve float32 half-add boundaries.
  These are graph-cost changes, not a measured render-speedup claim.
- Release build linked successfully. 206 actual CPU/isolated Metal
  radiance checks passed through native SDL round-trip and real Blender
  exports, including signed/integer/half-tie and large float32 boundaries.
  Tolerance `0.05` deliberately covers observed GPU radiance residuals;
  this is not evidence of bitwise numerical parity.
- Installed Blender's actual SUPERLUXCORE Generated→signed Vector Floor
  material rendered four distinct colour bands with flat interior
  plateaus. The resulting PNG is published as a manual example.

### Typed scalar sockets without identity powers

- Blender Vector→Float averages the evaluated RGB components; Color→Float
  uses the active OpenColorIO luminance coefficients. Sine incorrectly
  evaluated channelwise: actual CPU biased emission was `[4,4.99753,4]`
  for both Vector/Color `(0,1.5,0)`, instead of `[4.47943]*3` for the
  vector and `[4.87854]*3` for default-config colour.
- The adapter now converts at scalar socket boundaries with the existing
  native dot product. Scalar outputs pass through without a wrapper;
  group boundaries and third operands follow the same rule. RGB to BW
  reads active-config coefficients instead of fixed legacy weights.
- Removed `power(x,1)` conversion and all its callers. An actual paired
  export against the previous committed adapter counted Value→scalar
  Floor/Ceil/Truncate/Fraction graphs at 3→2 explicit textures, including
  the Value leaf. Round→Subtract also changes 3→2. Vector/Color→Sine
  changes 2→3 because a real conversion is required; the old smaller
  graph produced wrong radiance. No native object/layout or separate
  GPU-buffer changes; no measured end-to-end speedup is claimed.
- 222 actual CPU/isolated Metal SDL round-trip/render checks passed,
  including evaluated vector products, colour luminance, third operands
  and both scalar group boundaries. The existing `0.05` radiance tolerance
  does not establish bitwise math parity.
