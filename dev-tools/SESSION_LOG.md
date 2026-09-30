
## Session — stall-free PhotonGI updates (C7) + progressive GPU merge (C2)

**C2 (commit 8c3158067)**: GPU VCM merge radius now follows a
progressive global schedule — host loop derives `t =
lightSampleCount/lightTaskCount`, re-derives mergeRadius + the three
SmallVCM MIS constants, re-uploads `GPUTaskConfiguration` (640B) only
on pass change. `path.vertexconnection.mergealpha` default 0.95;
1.0 = old fixed radius. Kernels untouched (hash cell size already
tracks the radius). e54 PASS.

**C7**: `Update()` no longer parks all render threads for the whole
photon re-trace. Thread 0 launches `UpdateWorker()` (jthread) → shadow
copy (photons/beams/BVH/beam index/radius) built while rendering
continues → `std::barrier` completion step (`completion_t` with a
cache back-pointer — the type already existed but ran an empty fn)
swaps pointers atomically with zero in-flight queries. TracePhotons
parameterized on output buffers; BuildCausticBeamsIndex(src,dst).
Failure → retry flag, no crash. Verified: cornell render, update ran
32.6 s in background, swap + radius shrink applied, clean exit.

**Empty-cache early-out** (TracePhotonsThread): scenes with zero
cacheable transport traced the full 100M-photon budget every update
(cornell: 32.6 s/update). Now bail after 4M traced paths with zero
deposits → 1.1–1.5 s/update (~22×). Changes PhotonGI bias only in
truly-empty scenes (sub-1/4M-rate caustics would be cut — negligible).

e54 suite: all gates PASS after both changes.

## Session — review fixes A/B (commits 27353b84d, f08766a75, 93f9d7f6f)

Strict-review follow-ups on the C1–C8/B1' caustic work; the three
commits above plus doc/engineering/pgic-review-fixes.md:

- traced-count race: UpdateWorker wrote causticPhotonTracedCount
  while ConnectCausticBeams read it. Now updateCausticPhotonTracedCount
  (shadow) seeds at launch, accrues in the worker, publishes in
  ApplyPendingUpdate. CPU pgic ratio to one-shot ~1.0.
- updateThread.reset() raced across FinishUpdate (all threads) and
  the barrier completion step. updateThreadMutex serializes all three
  lifecycle sites; reset precedes updateInFlight clear (avoids the
  completion joining the NEXT worker).
- lightTaskCount unsigned wrap at taskCount <= 8192 could arm
  ingest-only with zero light tasks -> permanent empty cache.
  Guarded in pathoclbase + tilepathocl.
- engine->taskConfig was mutated by every OCL thread (VC schedule,
  pgic refresh, deposit retire) while siblings memcpy'd it to their
  devices. threadTaskConfig per-thread snapshot in InitGPUTaskBuffer;
  all mutations + taskConfigBuff uploads now use it (CL_TRUE).
  Kernel arg/enqueue read sites follow.
- tilepathocl threads != 0 now refresh on GetCausticPhotonPass()
  bump (Update() returns true only on thread 0) - same fix class
  as C8 for pathocl.
- pgicDepositWanted gated to PATHOCL: tilepath never calls
  DrainPGIC, so deposits would starve the cache.
- GPU IsDirectLightHitVisible calls EyePathInfo_IsCausticPath
  (depth>1 parity; signature const-qualified). directPdfW==0 and
  zero-length PhotonBeam guards on both sides.

Verified: parity-regression 4/4, e90 all PASS (GPU parity ~1.0,
progressive pgic self-consistent), e54 8/8 gates PASS, 720p GPU
visual render correct.

## Session — review round 2 fixes (barrier deadlock, abort, CL_TRUE)

Second strict review of the C1-C8/B1' caustic stack found one blocker:

- **FinishUpdate/Update barrier deadlock** (pgicupdate.cpp): Update()
  arrived once on a pending swap while FinishUpdate() drains in two
  phases (arrive -> flag -> arrive). A phase needs ALL parties; a
  single Update arrival could consume a finisher's phase-1 while the
  flag it then set blocked phase-2 arrivals -> stranded barrier ->
  Stop() hang. Interleaving-dependent: needs a swap pending exactly
  when threads exit at different times. Fixed: Update() arrives twice
  (paired with FinishUpdate's two phases); every barrier entry now
  contributes an even arrival count within one call.
- finishUpdateFlag -> std::atomic<bool>{false}: cross-thread read
  outside barrier phases (data race) and uninitialized in the
  serialization ctor path.
- Update worker cancellation: jthread callable now takes stop_token,
  std::stop_callback arms updateAbortRequested; UpdateWorker checks it
  before index builds and TracePhotonsThread::RenderFunc polls it in
  all three work loops (their own jthread token is never signaled -
  Join() only waits). Engine stop no longer waits out a full photon
  trace (up to photon.maxTracedCount).
- DrainPGIC zeroCounters write CL_FALSE -> CL_TRUE: stack source must
  not outlive a nonblocking enqueue (same class as the taskConfigBuff
  fix).
- nVM/vmNorm after deposit-task retirement analyzed: NOT a bug.
  Shared tasks (lightTracing/VC) keep running (only deposit append
  stops); a dedicated tail parks whole tasks, freezing VM population
  AND lightSampleCount together -> normalization stays consistent.
  Documented in pgic-review-fixes.md so nobody "fixes" it.

Verified: ninja Release build clean; e90 all PASS re-run (5 scenes,
progressive pgic vs one-shot within ~1%); new e91_pgic_update_smoke.py
3/3 (PATHCPU updatespp=4 haltspp, PATHOCL deposits+swaps, mid-flight
Stop = 0.00 s). Doc: doc/engineering/pgic-review-fixes.md gained the
barrier-pairing, abort, drain-ownership, retire-coherence sections.

## Session — review round 3 fixes (SSP tail OOB, sspTails init, WaitForDone bound)

External review of the SSP GPU port found one blocker plus hardening:

- **vtx[] index underflow -> OOB device read** (pathoclbase_funcs.cl):
  the MS_DISCOVER replay indexed `sspTail->vtx[specN - chainN]` with a
  LIVE `specN` read. The paired eye task rewrites its record between
  launches (a new path resets specN to 0, and diffuse-start paths keep
  it 0), so specN could shrink below chainN -> unsigned underflow ->
  vtx[~4G] wild global read (Metal command-buffer trap territory).
  Fixed: index with `chainMaxV` — it already IS the StartTail-time
  specN snapshot, so indices stay in [0, chainMaxV) ⊆ [0,4] regardless
  of record generation. Content staleness still guarded by the
  per-vertex objectID match. Documented in doc/engineering/ssp-tail.md.
- **sspTailsBuff zero-init**: was AllocBufferRW(nullptr) — a light task
  could read a garbage record (specN/flags) before the paired eye
  task's first write, aiming the replay at uninitialized anchors.
  Now allocated from a zeroed host image (all-zero = specN 0 = gate
  rejects).
- **WaitForDone pump bounded by IsStarted()**: the 200ms UpdateFilm
  pump now only runs while the engine is started; a never-started or
  just-stopped engine falls straight to WaitForDone()'s join. Same
  externally visible behavior for the halt path; no pumping on a dead
  engine.
- **MNEE_MS_MAX_VERTICES hoisted to pathtracer.h** (inline constexpr):
  the CPU SSP gate's literal 4 was a silent duplicate of the TU-local
  solver capacity in pathtracer_mnee.cpp. Both now share one constant;
  the GPU #define mirror is documented to stay in lockstep.
- Removed the LUX_SSP_DBG printf block (bring-up instrumentation; e93
  covers the feature).

Verified: e93_ssp_tail 6/6 (GPU parity 0.99994, tail replay active),
e52 adaptive-noise 5/5, WaitForDone haltspp=8 returns 0.41s on a bare
caller, Release build clean.

## Hot-loop allocation round (post-RTTI)

Second profile pass on `prism-conservatory` PATHCPU: allocator hits
concentrated in `PGICPhotonBvh::ConnectAllNearEntries` (102),
`PhotonGICache::ConnectWithCausticPaths` (69), `RenderLightSample` (42),
`RenderEyePath` (30), `DirectHitInfiniteLight` (34).

Root cause: `SpectrumGroup::Add` auto-grows an internal
`std::vector<Spectrum>` — every photon/beam connect allocated. Fixed by
pre-sizing the result to the light count once per query and
accumulating in place (helpers now take `SpectrumGroup &`), plus
`sampleResults.reserve(maxPathDepth.depth + 2)` in
`RenderLightSample`.

prism-conservatory PATHCPU: 0.256 -> 0.31 Ms/s (+21%; ~72% cumulative
vs 0.18 Ms/s baseline). e90/e91/e54 PASS. Doc:
`doc/engineering/hot-loop-allocations.md`.

## RTTI round 3: per-hit defs-container casts eliminated

`sample` re-profile showed residual `__dynamic_cast` (~1.6%) rooted at
`HitPoint_t::Init` -> `GetSceneObject(meshIndex)`: every defs accessor
(`SceneObjectDefinitions`, `MaterialDefinitions` index overloads,
`TextureDefinitions`) downcast the `NamedObject` base per call.

- All defs Get*(name|index) accessors now `static_cast` — the private
  `NamedObjectVector` is only populated through typed `Define*(UPtr&&)`
  entry points, so the element type is invariant.
- `ExtTriangleMesh::FromMesh` replaced its 3-cast chain with a new
  `Mesh::GetAsExtTriangleMesh()` virtual (Mesh is a *virtual* base —
  static downcast is impossible; a virtual getter is the cheapest
  legal dispatch). Overrides: ExtTriangleMesh->this, instance/motion
  -> wrapped base mesh.

Post-change profile: dynamic_cast family = **0** hits.
e9 vertex-motion parity (MBVH/BVH/EMBREE): all PASS.

## Corona-parity defaults (BLC) + `Get<float>` lexical_cast bugfix

Adapter `scene.superluxcore` stock defaults flipped to fire-and-forget
(the "turn it on and it works" UX target):

- `config.mnee_enable` True (SDS caustics out of the box; ~zero cost
  without delta occluders)
- `config.envlight_cache.enabled` True (final renders only, env-type
  lights/worlds)
- `denoiser.enabled` True (OIDN COMPONENTS + periodic refresh — every
  render carries a DENOISED pass)
- `halt.enable` True, `halt.use_noise_level` True @ 3%,
  `halt.samples` 2048 as backstop (Corona default-time equivalent)

e49 extended: 154/154 PASS including the new halt/denoiser pins
(`photongi.caustic_updatespp_minradius` pin corrected to 0.0 =
automatic, an intended engine-side default).

### E50 zero-config e2e found a real engine bug

`RenderSession` ctor died with `bad lexical cast` whenever
`film.adaptiveerror.target > 0` **only via the API path** — `.cfg`
file loads worked. Bisect + lldb (catch-vs-throw separation — the
`GetAllUniqueSubNames` sort comparator deliberately throws/catches)
traced it to `Film::Parse -> PropertyValue::Get<float>`:
`boost::lexical_cast<float>(double)` demands an exact roundtrip and
throws on `0.03`. Python floats are stored as DOUBLE_VAL; `.cfg` files
store STRING_VAL -> istringstream, which never threw — a latent
API-path landmine for every `Get<float>` consumer.

Fix `8ff73e24b`: `lcast<T,S>` narrows arithmetic sources to float via
`static_cast`; all other `Get<T>` targets keep lexical_cast (integer
signedness/exactness guards preserved). Doc:
`doc/engineering/property-variant-casts.md`.

Post-fix: full session creation OK; e50 zero-config render completes —
adaptive error 47.7% -> 2.48%, OIDN refreshes periodically, auto-halt
fires, DENOISED pass uploaded, ACES 2.0 + 1280x720 artifacts kept
under `/tmp/e50/`.

### MPG-lite Phase A (earlier this session, `940664923`)

Energy-aware MNEE seed retention: `MneeSeedEntry.fluxWeight` on CPU
and mirrored `.cl` layout; colliding slots keep the higher-flux basin.
Basin-selection only — estimator weights untouched. CPU/GPU parity:
cache-on/off mean 1.1406 (GPU) / 1.1415 (CPU), caustic energy
preserved 1.0583 both sides.

### MPG-lite Phase B: caustic-photon -> MNEE seed injection

`tracephotonsthread.cpp` now snapshots each walk's last strict-delta
vertex (`bsdfEvent & SPECULAR` on an unbroken specular chain); every
caustic deposit queues an `MneeSeedRecord` (vertex p/n, light ptr id,
occluder mesh*2+flipped-side bit, bucket key, photon fluxWeight).
`MneeSeedEntry`/`MneeSeedKey`/`MneeSeedStore` moved to the shared
`include/slg/engines/mneeseedcache.h`; `PhotonGICache` collects
per-thread records and replays them into the PathTracer seed table
(deferred, because PATHCPU preprocesses before ParseOptions allocates
the table). PATHOCL uploads the seeded CPU table into `mneeSeedsBuff`
at thread init; ingestOnly (GPU-deposit) sessions skip CPU tracing ->
no records, GPU keeps self-seeding (basin hints only, estimator
unchanged). Side-bit flips vs the eye convention (opposite traversal
direction through the interface); reflect events are approximate - a
miss costs nothing.

Verified: PATHCPU + photongi.caustic logs `injected 65536
caustic-photon MNEE seed(s)` per generation (4x table cap engaged);
e17 GPU ALL PASS; no crashes on PATHOCL ingest mode.

## Session — gauntlet v2 + R1 hotpath cuts (`e25c8ffc6`)

**Gauntlet v2** (`dev-tools/g1_gauntlet_bench.py`): 18-row scene table
(+3 new stress scenes `multi-caustic-chain`, `portal-interior`,
`glossy-caustic-mix`; meshes via `gauntlet_geo.py`), modes: fixed-wall
(default), `--spp N` fixed-spp, `--reference`, `--parity` (cpu-gpu),
`--quick`. Reports: spp, samples/s, mean/median luminance, peak RSS,
finite-pixel check, RMSE vs reference, unique output names, OCIO ACES
2.0 hero PNG, per-scene cwd (pool needs scene-dir rel paths), machine
load warning. `vol-bunny.scn` dropped from the table — references
missing `bunny_cloud.vdb`; replaced by self-contained
`vol-densitygrid.scn`.

**R1 CPU hotpath cuts** (from the sampler/photongi/libm profile round):
- `Spectral::RGBProjector` — CIE weights + sampled white point are
  wavelength-only quantities; `PrepareRGBProjection(sw)` once,
  `ProjectToRGB(bins, proj)` per field. `ProjectSampleResultToRGB` ran
  9 spd.Sample + whitepoint normalize + 2 ToRGB per each of ~15
  fields; now once per sample + per-field dot+ToRGB. Black-field
  early-out (ToRGB(0)=0 exact). Bit-identical.
- `Scene::Intersect` — `dataSet->GetAccelerator(ACCEL_EMBREE)` was a
  map::find per ray *segment* inside the for(;;) transparency loop;
  hoisted to once per call.
- LightBVH `NodeImportance` — dot-space rewrite removes all
  transcendentals per call (was asin+2acos+2cos): node now stores
  `cosThetaO`/`sinThetaO` baked at build (struct +8B, GPU layout
  shared). `cos(max(0,acos(d)-t))` = 1 when `d >= cos(t)` else
  `d*cos t + sqrt(1-d^2)*sin t`; `thetaO+thetaB >= PI` cover-case
  detected as `cO <= 0 && sB >= sO`. CPU and `lightbvh_funcs.cl`
  kept identical (parity contract).

Validated: Release build clean; `parity-regression.sh` 4/4;
`e26_lightbvh_test.py` 10/10 (unbiasedness vs LOG_POWER, bounded RMSE
— bvh 0.0017 < flat 0.0027 — cpu-gpu parity, finite outputs).
Timing A/B deferred: machine load 3.8-47 during session, micro-op
(~0.1% profile share) below run-to-run noise without idle host.

Pending backlog notes: wavefront remains auto-off (dense mode faster
on measured workloads); GPU crawl-bail stays reverted (state machine
boundary rule); spectral parity e28 e2e pending a clean idle run.

## Session cont. — M4 promotions + CI film gate (`5be8ade72`, `0e4f7ccfc`, `67b15c522`)

**LIGHT_BVH -> default `lightstrategy.type`** (3 sites in
lightstrategy.cpp): e26 10/10 gates (unbiased vs LOG_POWER, RMSE
0.0017 < flat 0.0027, cpu-gpu parity). Flat distribution still built
via LogPower base for emit/infinite-only tasks. Escape:
`lightstrategy.type=LOG_POWER`.

**Auto light tracing** (`path.lighttracing.auto`, default on):
`RenderConfig::ApplyAutoLightTracing()` runs in `Parse()` right after
`GetConfig().Set(props)` — the resolved `lighttracing.enable` is
*injected into cfg* so every raw reader (6 film-channel sites in
pathcpu/pathocl/tilepathocl/pathoclbase/bakecpu + light strategy
setup) sees one value. Signature: any non-NULLMAT material with
SPECULAR|GLOSSY events, or scattering volume (CLEAR_VOL excluded), +
>=1 emitter; engines restricted to PATH/TILEPATH/RT* (light-task
populations). NULLMAT is SPECULAR|TRANSMIT but focuses nothing —
excluded. Verified: luxball on / bigmonkey off / vol-densitygrid on /
auto=0 and enable=0/1 honored; luxball PATHOCL 16spp finite,
mean 0.0985 == non-LT baseline.

Gotcha encoded: ParseOptions has no scene; film channel decisions read
raw cfg BEFORE pathTracer exists (pathoclbase InitFilm) — that's why
injection happens in RenderConfig::Parse, not ParseOptions.

**CI**: wheel-builder already runs `pysuperluxcoretest` on
ubuntu/windows/macos(2) — added `AssertFilmSane` gate (all pixels
finite + mean > 1e-4) to SimpleRender/StrandsRender -> real render
regression gate on every OS, no GPU needed.

**Blender**: `sync_dev_install.sh` ran — site-packages .so + dev wheel
+ addon sources synced, smoke-import OK.

Parity (Release console): 4/4 PASS under the new defaults.

## Session cont. — MNEE auto-enable (`72369ed5f`)

**MNEE -> auto on caustic-capable scenes** (`path.mnee.auto`, default
on): the same `SceneHasCausticCapablePaths` signature that gates light
tracing now also resolves `path.mnee.enable` when unpinned. Rationale:
MNEE covers the eye-side half of the caustic class (delta/glossy chain
blocking a direct-light connect), the solver gates itself per connect
so non-caustic scenes pay ~nothing, and on GPU the LMNEE probe path
piggybacks the light tasks the LT gate already provides. Both flags
stay independent: `path.lighttracing.auto=0` keeps MNEE auto and vice
versa; explicit `.enable` always wins.

Verified: luxball lt+mnee on / bigmonkey both off / `mnee.auto=0` off /
`mnee.enable=0` off / bigmonkey `mnee.enable=1` on. luxball PATHOCL
16spp finite mean 0.0984 (vs 0.0985 LT-only — MNEE overhead inside
noise). caustic-stress-many PATHCPU 24spp zero-config renders visible
under-sphere caustic pools. Parity (Release): 4/4.

Matrix updated: megaplan row "MNEE 자동" -> landed. Remaining ◐ rows
need wall-clock gates (guiding auto-condition, ReSTIR GI auto,
SSP tail, adaptive caustic partition) — next: measure guiding
overhead on diffuse-only vs indirect-dominated scenes to design the
auto signature.
