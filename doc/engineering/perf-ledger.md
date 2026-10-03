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

### Inclusive Compare and precise, cheaper SDL number formatting

- Blender Compare uses `abs(a-b) <= max(epsilon,1e-5f)`. The old adapter
  used strict less-than with no epsilon floor: actual CPU Compare(1,1,0)
  emitted biased radiance 4 instead of 5. Added `mathfunc.max/lessequal`
  binary ops (IDs 20/21), preserving existing IDs and texture layouts.
  Constant differences round to float32, matching native evaluation.
- Paired real Blender exports: two linked operands with constant epsilon
  remain five explicit textures, including input leaves (three operations).
  A linked epsilon changes six→seven textures because its minimum must
  be evaluated. Native maximum needs one operation rather than a
  less-than/select composition; inclusive comparison needs no inversion.
- The large linked Compare fixture exposed a separate serialization defect:
  Python double properties rendered `16777216` as `1.67772e+07`
  (16777200). Native float `ToString` used seven significant digits;
  the direct C++ round-trip changed 16777216 to 16777220.
- Float/double `ToString` uses locale-independent, compile-time `fmt`
  shortest round-trip formatting in a fixed 32-byte stack buffer.
  Integer and generic formatting remain unchanged. No stream or runtime
  format-string parser is used by these overloads; owned result strings
  can still allocate when their accurate representation exceeds SSO.
- The initial `std::to_chars` implementation failed the macOS Intel wheel
  build: floating-point overloads require macOS 13.3, while the supported
  Intel deployment target is 11.0. The existing fmt dependency preserves
  that target; the public LuxRays header propagates `fmt::fmt` to consumers.
- Current Release C++ smoke: 19898 finite float and 19990 finite double bit
  patterns plus 14 signed-zero, unit-spacing, maximum, normal/subnormal
  boundary cases round-tripped exactly. The same source compiled for
  Intel's macOS 11.0 target. Earlier installed Property SDL validation
  also round-tripped 5004 finite doubles.
- Eight representative floats, 1000000 calls per path on M5 Pro:
  compiled fmt 11.51 ns/value; classic-locale stream 111.46 ns/value.
  This single paired run measures formatting, not rendering or allocation.
- The full Release rebuild linked. 268 actual CPU/isolated Metal render
  checks passed through SDL round-trip, including equality at epsilon,
  zero/negative epsilon, float32 spacing and unordered NaN differences.
  Installed-extension Compare graphs also passed; radiance tolerance
  remains 0.05 and does not establish bitwise shader parity.

### Native extrema and range-safe texture division

- The old Math Minimum selection returned black on PATHCPU for linked
  finite inputs `3e38` and `-3e38`: its intermediate subtraction
  overflowed before selection. Scalar/vector Minimum and Maximum now
  use native `mathfunc.min/max`; `min` appends operation ID 22 without
  changing any texture fields or previously serialized IDs.
- Four real Blender graph probes (Math/Vector Math Minimum/Maximum,
  two linked inputs each): explicit textures fall from 6 to 3,
  arithmetic operations from 4 to 1. No measured render-speedup claim.
  Constant Vector Math extrema also fold independently by component.
- Keeping the full-range normalization fixture exposed Metal division:
  `3e38/3e38` returned 0 instead of 1 because fast reciprocal arithmetic
  underflowed. Divide textures now request Metal's precise intrinsic,
  without disabling fast math throughout the renderer.
- Small-denominator probes exposed a separate CPU reciprocal overflow
  in `Color::operator/`. Only Divide texture spectrum evaluation now
  divides components directly; the shared colour operator is unchanged.
- Actual Metal kernels returned NaN for subnormal-input quotients with
  both fast math enabled and disabled. The Divide shim decodes exact
  integer significands for subnormal inputs, then divides/rescales them.
  Ordinary inputs retain the precise intrinsic path. Bitwise zero guards
  distinguish actual zero from a subnormal denominator.
- No new texture storage, buffers or worker threads. Extra bit tests and
  precise division are a correctness tradeoff; no division throughput
  improvement is claimed. Results that remain subnormal are still
  subject to Metal's arithmetic limits, not a bitwise-parity guarantee.
- Development synchronization now updates the bundled Metal translator
  beside the extension and inside its cached wheel, not just the binary.
  Installed binary/exporter/translator rendering passed HDR, tiny signed
  quotients, zero divisors and linked/folded extrema.
- Final actual Blender export → SDL round-trip → CPU/isolated Metal gate:
  302 checks passed, with unchanged `±3e38` extrema fixtures. Seventy
  installed-package render checks passed. Radiance tolerance remains
  0.05; CPU workers are disabled on the Metal gate.

### CPU startup without optional GPU discovery

- The ARM macOS wheel's CPU render smoke failed in optional OpenCL device
  enumeration with `CL_INVALID_VALUE`. CPU engines only consume native
  intersection devices, so their render contexts now skip OpenCL, CUDA,
  Metal and Vulkan discovery. GPU engines and explicit device-list APIs
  retain discovery and its errors; no exception is suppressed.
- Fault injection replaced the actual exported OpenCL device-query pointer
  with a callback returning `CL_INVALID_VALUE`: rebuilt PATHCPU rendered
  the expected `[4.5,4.5,4.5]` radiance, while explicit GPU discovery still
  reported the injected driver error.
- The next wheel run rendered successfully but `Film.Save()` still created
  an optional GPU image-pipeline context. Film configuration now defaults
  to software processing for CPU engines and hardware processing for
  PATHOCL/TILEPATHOCL/RTPATHOCL. Explicit `film.hw.enable` requests (and the
  existing legacy setting) retain precedence.
- With the actual OpenCL query pointer returning `CL_INVALID_VALUE`,
  default PATHCPU rendered `[4,4,4]` and saved a valid 32×32 PNG without
  calling that pointer. Explicit hardware processing still raised the
  driver error. Isolated Metal rendering and its default hardware pipeline
  saved the same scene successfully; pipeline hardware memory was 28 KiB.

### Transformed Generated bounds without per-hit mesh scans

- An actual Blender RGB Min/Max emission diagram exposed mixed spaces:
  hit positions were inverse-transformed, but normalization used the
  already-baked base mesh's bounding box. Translated planes lost their
  intended ramps. Rotating a world-space AABB back is not an exact fix.
- Each base mesh now caches a 3×4 baked-to-normalized authoring map.
  Authoring bounds come from inverse-transformed vertices, once when
  needed. Scene preprocessing warms only meshes whose material references
  Generated, before shading workers start; unrelated proxy meshes are
  not scanned for this cache. Geometry/applied-transform edits invalidate it.
- Direct meshes use their baked hit position. Instance/motion wrappers
  undo only their wrapper transform before applying the shared base map.
  Flat axes evaluate to 0.5, matching Blender's texture-space center.
  Shading performs no geometry scan or bounds division; no per-vertex
  coordinate array is added. The GPU mesh payload replaces a 24-byte
  bbox with a 48-byte map; texture layouts are unchanged.
- Blender 5.2 texspace values independently populate reference vertex
  colours. Translation and rotation/nonuniform-scale fixtures, both direct
  and instanced, subtract this reference from Generated and amplify by
  1000 before emission. All 16 checked pixels retain the existing 0.05
  radiance tolerance, corresponding to roughly 0.00005 coordinate error.
  A constant-colour control and a 100× amplified diagnostic separated
  existing Metal radiance residuals from coordinate errors.
- The full actual Blender export/SDL/CPU/isolated Metal corpus passed
  310 checks, including live transform edits. The installed extension rendered the unchanged 384×192
  RGB extrema diagram: raw EXR samples verified the minimum's upper
  plateau, maximum's lower plateau and distinct blue components.
- This validates transformed base-mesh bounds, not complete Blender
  undeformed ORCO, custom texspace or multi-material whole-object bounds.

### Noncommuting applied-transform edits

- A live rotation after a baked rotation/nonuniform scale exposed a
  composition-order defect: vertices received `new × old`, while stored
  applied-transform metadata received `old × new`. Generated's 1000×
  amplified residual rendered `[5.31,8.80,4]` instead of `[4,4,4]`.
- Applied-transform metadata now uses the same order as vertex updates.
  No new allocation or transform multiplication is introduced.
- The same reproduction passed on PATHCPU and isolated Metal. A permanent
  Blender-reference fixture warms the scene, then applies rotation and
  translation through two BeginSceneEdit/EndSceneEdit transitions. Both
  backends retained `[4,4,4]` and passed every original 4×4 ROI check.

### Published Blender 5.2.1 artifact cutover

- Release wheel run 37121201329 built and smoked all four cp313 platforms.
  `wheels-latest` now contains 2.11.8 built from
  `dbf65e08329ac60c12280833bc8f299352a83521`, not the prior 2.11.7 artifacts.
- The downloaded macOS-arm64 wheel matched its public SHA-256
  `f3f1952bdb4285798cb817cb002c14bb008a5b9b281d39fefdbc6348762a21b9`.
  A factory-startup Blender 5.2.1 process imported it from an isolated site
  and passed all 310 export/SDL/CPU/isolated Metal gates, including live edits.
- Bundle run 37123241893 used Blender 5.2.1 and published four matching
  offline platform ZIPs. The public ARM ZIP matched SHA-256
  `f43ef07c46b116bd85a29a4a39125977453328f1f8491a1fee49f579a4f40467`.
  Blender's extension installer installed it into a separate profile
  without an engine download.
- Actual SUPERLUXCORE CPU and Metal renders of sine-controlled emission
  cubes passed bright/dark PNG-region checks; the Metal run used no native
  CPU workers. These are installation and behavior checks, not a speedup,
  cross-vendor GPU or production-certification claim. Rolling bundle
  releases retain their pre-release warning.

### Authored Generated texture space and mapped emissive proxies

- Blender 5.2 custom texture space and material partitioning exposed
  separate normalization errors: 1000× residual emission had red values
  59.21 and 239.43 instead of 4 on CPU, with matching Metal failures.
- Export now builds one whole-mesh affine normalizer before material
  splitting and passes it to each native submesh. It reuses the existing
  48-byte cached map and GPU descriptor; no per-vertex ORCO array, per-hit
  scan, bounds division or new texture layout is introduced. Explicit maps
  compose with inverse baked edits and survive SDL/Boost serialization.
- `.lxm` v5 stores that map in existing reserved header bytes, preserving
  the 128-byte header and v4 geometry sections. Mesh area uses former
  padding, avoiding a full scan for v5 emissive proxies. Legacy files
  calculate area lazily when requested, matching existing instance caches.
- Real mapped-proxy rendering exposed uninitialized bevel pointers and
  zero/uninitialized mesh area: one load aborted in preprocessing; a
  constant-emission control then rendered black. Constructor initialization,
  stored/lazy area and mapped-state-before-preprocess fix those causes
  without copying mapped geometry or rebuilding UV caches.
- Actual Blender `mesh_converter.convert` plus native CPU/Metal rendering
  passed 12 cases each in memory, through PLY/SDL reload and through mapped
  v5 `.lxm`/SDL reload. Cases include custom space, material splits, live
  edits and scene archives. The full permanent Blender export/SDL corpus
  passed 328 CPU/isolated Metal checks, including legacy v4 emissive proxies.
- An actual 2178-triangle auto-proxy test changed only texture-space
  location. The cache key now includes auto/location/size; it replaced the
  file and changed X offset from 0.5 to the expected 0.375.
- The public 2.11.9 offline bundle's actual `bpy.ops.render.render` CPU
  and isolated Metal images of a two-material custom-space mesh were
  compared with Cycles on Blender 5.2.1. Interior raw EXR maximum absolute
  RGB errors were 0.003421 and 0.007908; material-boundary continuity passed.
  Its genuine CPU image is SuperBlendLuxCore
  `docs/assets/ex_generated_texspace.png`.
- These checks do not establish undeformed ORCO for deforming modifiers,
  legacy Generated-to-UV texture mapping parity, cross-vendor GPU parity,
  or a measured render-speedup claim.

- Existing cluster smoke verified all 44,851 self-contained ranges of a
  717,602-triangle v5 proxy and rendered both mapped and PLY inputs at
  1280×720. Mapping added 1.3 MB in that process; this is not a controlled
  before/after memory-speedup measurement.
- The existing proxy compatibility smoke now compares source positions
  and triangle multiplicities without assuming equal vertex counts across
  cluster duplication. Incidental exact-version/flag/file-end assertions
  were removed. Normal/UV/color/alpha/vertex-AOV/triangle-AOV preservation
  and malformed/truncated-file rejection passed.
- Its former indirect-lighting fixture produced 2.112% independent MC
  image noise against a 1% bound. Constant-emission geometry isolates
  proxy preservation instead; the same bound is now enforced, and the
  observed image delta was 0.02038%. No tolerance was widened.

### Public 2.11.9 authored-coordinate artifact proof

- Wheel run 37131924537 built all four cp313 platforms and published
  native source `e4801e234b0a0bbfba890e65800c67a2831f0cfe`.
  The downloaded Apple-silicon wheel matched public SHA-256
  `3a6b879606199a953b973a88fb979d13616b9a783c448273e574908a5de0b8cb`.
- Factory-startup Blender 5.2.1 imported that wheel from an isolated site
  and passed all 328 CPU/isolated Metal renders. The existing 0.05
  radiance gate and 1000× coordinate amplification were unchanged.
- Initial bundle run 37133389528 published add-on `f0b9bfb6` with the
  matching 2.11.9 wheel. Its ARM ZIP matched public SHA-256
  `1580e21be564e96df6b986aad0c49ccb6b369bd67bba2352a4ceae27b65646c4`;
  the embedded wheel matched the same public wheel digest.
- Blender's extension installer installed that public ZIP into a separate
  profile and explicitly skipped the engine download. Runtime module and
  add-on paths were asserted to remain inside that profile. Actual CPU/
  isolated Metal custom-space/material-split renders passed against Cycles.
  These rolling artifacts remain pre-releases, not production certification.

- Documentation-aligned bundle run 37135052545 published add-on
  `6df58ad22211f59098ab84be1bd0784b3d7c8e36`. The final public ARM ZIP
  matched SHA-256
  `938547e09515bb4ffa2878a689157fa758a16dcba3fe2fb6d699f24f6997c80e`;
  its embedded native wheel retained digest `3a6b879606199a953b973a88fb979d13616b9a783c448273e574908a5de0b8cb`.
  Another fresh-profile install and actual CPU/Metal/Cycles render passed
  with maximum absolute interior RGB errors 0.003404 and 0.009549.
- Pages run 37135051555 deployed the matching manual from the add-on's
  `main/docs`, at `https://claudianus.github.io/SuperBlendLuxCore/`.
  Actual Chromium views verified the 2.11.9 version pair, 328-check
  provenance, remaining coordinate limitations and loaded genuine images.

### Continuous camera raster coordinates

- Independent Blender 5.2.1 `Camera.view_frame` projections and a
  world-position emission ramp reproduced a one-pixel vertical shift on
  both PATHCPU and physical Metal for orthographic and perspective cameras.
  An equirectangular incoming-direction ramp reproduced the same CPU shift;
  Metal's environment ray generator was already centered correctly.
- The samplers pass continuous positions `pixel + 0.5 + filterOffset`.
  Camera code incorrectly reflected those coordinates as `height - y - 1`,
  an integer-index rule. Forward rays now use `height - y`; reciprocal
  sample-position, image projection and point-to-film paths use the same
  convention. CPU environment PDF latitude follows the corrected ray.
  Existing integer-index/filter clamps are unchanged.
- Before correction, measured mean Y offsets were 0.999989 pixels for
  orthographic CPU, 0.998188 for orthographic Metal, 1.000001 for
  perspective CPU, 0.993567 for perspective Metal, and 0.999611 for
  environment CPU. The independent Cycles references were within 0.001
  mean Y pixels. Each renderer used 512 samples and raw 32-bit EXR output.
- The permanent SuperBlendLuxCore
  `dev-tools/camera_raster_parity_test.py` exercises centered and shifted
  orthographic/perspective cameras plus full equirectangular projection:
  15 actual Cycles/CPU/isolated GPU renders passed. Maximum per-row mean
  Y error was 0.018039 pixels; every image passed the 0.003 maximum RGB
  and 0.05 per-row pixel bounds. These gates reject the original full-pixel
  displacement; no existing tolerance was widened.
- A deterministic native API smoke additionally checked independent center
  rays and world-point projections, 48 fractional/first/last-pixel reciprocal
  sample positions, orthographic image projections, and environment ray/PDF
  latitude with a 0.001 bound. It exposed a separate 0.018608-pixel longitude
  error in the environment inverse's `acos`/sine reconstruction. `atan2`
  now recovers azimuth directly without the meridian precision loss.
- The existing 328 Blender export/SDL/CPU/Metal coordinate and math checks
  also passed with the corrected native module. No new camera fields,
  allocation, per-pixel buffers or layout changes were added; one redundant
  subtraction was removed. No render-speedup or cross-vendor GPU claim.

### Public 2.11.10 camera artifact proof

- Native run 37139385800 built and attested all four cp313 platforms from
  source `8573d1cc9ad2a16359bd521e506b089e752b4555` and published
  `wheels-latest`. The downloaded Apple-silicon wheel matched public SHA-256
  `499fdb7dfaa570fff5698a267e1be611d06d0a1aeb2316e499183a34141aa93a`.
- Factory-startup Blender 5.2.1 imported both the package and native binary
  from an isolated downloaded-wheel site. All 15 camera renders and 328
  coordinate/math CPU/isolated Metal checks passed. The rendering child
  independently asserted and printed that same public native-module path.
  Maximum camera per-row Y error was 0.018817 pixels, below the unchanged
  0.05 gate.
- Initial bundle run 37140601741 published add-on
  `36f8572b9ebeb0e9b9798ba32c7c7ed8b0a82bf1`. The downloaded ARM ZIP
  matched SHA-256
  `4ddbf9f9582fea9c92b62a4cb9914ae4a6f5edf70fefe606aad9158622afa0c5`;
  its embedded native wheel matched the same public wheel digest.
- A clean, separate Blender profile installed the offline ZIP, explicitly
  skipped the engine download, and loaded add-on/package/native paths
  inside that profile. All 15 actual camera renders passed, with maximum
  RGB error 0.001358 and maximum per-row Y error 0.017232 pixels.
  SuperBlendLuxCore `docs/assets/ex_camera_raster.png` is its genuine
  384×192 CPU equirectangular incoming-direction emission render.
- Isolated Blender resource override directories must exist before
  startup: a first probe with missing directories installed the package
  but could not load its repository module. Creating the config/scripts/
  extensions directories before startup produced the successful offline
  installation above. These artifacts remain rolling pre-releases.

- Documentation-aligned bundle run 37141749403 published add-on
  `1ee921d62691d2652c4d3a191c3077cf7c76378b`. The final public ARM ZIP
  matched SHA-256
  `06ec7e9ba4752a81a9dfd55c99d6747f25935f4ee9807667b84aac0be686b567`;
  its embedded native wheel retained digest
  `499fdb7dfaa570fff5698a267e1be611d06d0a1aeb2316e499183a34141aa93a`.
  Another fresh-profile offline install and all 15 actual camera renders
  passed against Cycles with the unchanged gates.
- Pages run 37141748550 deployed the matching public manual from add-on
  `main/docs`. Actual Chromium views verified the 2.11.10 version pair,
  camera/reference bounds, genuine loaded 384×192 panorama, remaining
  camera limitations and install-to-camera section link at
  `https://claudianus.github.io/SuperBlendLuxCore/manual/`.

### Final-render transfer and Metal diagnostics — local evidence

- Blender 5.2.1, Apple M5 Pro, 1024×512 film: the large-film
  `RGBA_IMAGEPIPELINE` conversion advanced shared source/destination
  pointers inside parallel pixel lambdas. Indexed, disjoint writes restore
  RGB and alpha without another allocation. The two `RADIANCE_GROUP`
  accumulation branches had the same shared-destination race; destination
  pointers now belong to each pixel invocation.
- Four actual Blender CPU/Metal opaque/transparent renders matched their
  independently extracted native RGB and alpha exactly after the RGBA
  correction. The light-group correction restored agreement with native
  RGB/emission. This is channel-transfer evidence, not a claim that the
  remaining high-resolution Metal emission discrepancy is fixed.
- Opaque Blender Combined packing now allocates one RGB buffer and one
  RGBA buffer, fills the alpha column in place, and releases RGB before
  AOV transfer. NumPy payload peak changes from `32N` to `28N` bytes:
  16 MiB to 14 MiB for this film, 12.5%. Seven live final-draw calls per
  device measured approximately 16,778,504 → 14,683,704 traced bytes on
  CPU and 16,778,507 → 14,683,590 on Metal. Minimum draw time changed
  2.229 → 2.157 ms on CPU and 2.646 → 2.728 ms on Metal. These mixed
  timings do not establish a general speedup; neither the payload nor
  traced peak is process RSS, VRAM, or whole-render peak memory.
- An isolated native SDK render reproduces the Metal defect without
  Blender: literal emission `0.5 0.5 0.5` stays at 0.5, while the texture
  VM graph `0.25 + 0.25` produces rare black contributions. One 512-spp
  run returned blue 0.496805–0.5 in an interior ROI. Do not explain this
  deterministic constant-expression deficit as sampling noise or widen
  the Blender regression's RGB gate to conceal it.
- Apple documents that Shader Validation is incompatible with Metal
  binary archives:
  <https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage>.
  Loading an existing archive under validation crashed inside Apple's
  `_MTLBinaryArchive` loader. Archive use is now skipped when the
  `MTL_SHADER_VALIDATION` environment variable enables instrumentation.
  Pipeline creation retains function labels and uses initialized
  descriptor-based creation with or without an archive. The actual native
  `metal_kernel_smoke` passed all 64 values (1, 3, …, 127) with both API
  and shader validation enabled. Full render API validation separately
  exposed a missing optional `Film_Clear` buffer binding; this smoke
  does not certify full-render validation.
- A scalar-free helper-ID GPU probe returned zero for every work-item
  before correction: 63/64 outputs wrong. The early direct-pointer-only
  path now propagates helper IDs too, and body substitutions run from
  the end so shortened bodies cannot invalidate later spans. Actual
  Metal outputs passed all 64 expected `3*gid` values, including
  0, 3, 189; a scalar-bundle case passed `3*gid+1`, including 1, 4, 190.
  These are executed GPU results, not assertions about generated text.
- The helper's redundant `const size_t gid = get_global_id(0);`
  declaration is removed independent of indentation. A four-space
  declaration previously became the invalid self-shadowing `gid = gid`.
  After correction, a separate device-stack GPU scenario evaluated
  `0.25 + 0.25` 8,388,608 times across 131,072 work-items with zero
  wrong results and both API/shader validation enabled. That reduced
  case does not reproduce or resolve the full renderer's deficit.
- Clean-render resolution: pass helper IDs as scalar values, and do not
  force `noinline` on the small float/spectrum reader loops. Large VM
  dispatchers remain separate call targets. Changing only ID passing
  still returned blue 0.497470–0.5 with 10,227 deficient ROI pixels;
  removing the reader boundary returned 0.4999999702–0.5 with zero
  deficient pixels and zero measured noise. The latter run contained
  no diagnostic shader writes, scratch locks, or retries. The precise
  Apple compiler/backend mechanism is not established by these results.
- The synchronized Blender 5.2.1 developer runtime then passed nine
  actual 1024×512, 512-spp Cycles/CPU/Metal renders: opaque full plane,
  transparent full plane, and transparent half plane. Independent
  camera-geometry RGB and alpha gates remain 0.003 and 1e-5. Maximum
  observed RGB residual was 4.2945147e-5; alpha residual was 5.9604645e-8.
  The same live final-draw observer checks the native radiance group.
  Local runtime proof is complete; public artifact proof is recorded
  separately after publishing, not inferred from these developer runs.
- Reconfigured and completed all 459 native build steps for 2.11.11, then
  synchronized the version-checked binary and translator into Blender.
  The release-candidate run passed the nine large-film renders, all 15
  camera-reference renders, and the existing 328 coordinate/math checks.
  Camera maximum row-Y residual was 0.0172316 pixels; no acceptance
  tolerance, renderer default, or diagnostic shader override was changed.

