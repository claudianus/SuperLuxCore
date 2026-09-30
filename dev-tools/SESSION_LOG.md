
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

## Session cont. — GPU zero-light-task fallback (uncommitted->next)

**Bug:** auto light tracing on `taskCount <= 8192` produced a ~black
film (mean ~4e-5). Light tasks are carved in 8192-chunks, so a small
population yields `lightTaskCount == 0` — but the eye side still ran
hybrid caustic suppression, so the caustic class was removed with no
estimator to deposit it. e17 caught it only because auto-lt newly
exposed the latent hole.

**Fix** (`pathocl.cpp`, `tilepathocl.cpp` — demote before
`ParseOptions`/`taskConfig` consume the flags):

- PATHOCL: GPU lt pins native threads eye-only (`partition=1.0`), so
  they cannot compensate while lt stays on. native>0 -> demote to
  `lt=0, hbf=1` (CPU Metropolis light pass + splatter deposits
  instead); native=0 and no PhotonGI caustic cache -> disable
  lt/hbf/vc (VC would re-promote lt in StartLockLess).
- TILEPATHOCL: no partition pin — native threads still run the light
  pass via splatter under hbf promotion, so native>0 or
  `photongi.caustic.enabled` keeps lt on; only GPU-only+cache-less
  demotes.
- CPU engines unaffected: light pass is a per-sample probability
  split, not a block-quantized population.

**e17 fixes while debugging:** the test hard-coded the sibling
`LuxCore/` checkout (stale binary) and measured MNEE vacuously —
auto-lt promotes hbf, which by design suppresses eye-side MNEE.
Now pins `lt=0/hbf=0` explicitly for the MNEE-isolated asserts, and
gains T-1: auto-demote-not-black (unpinned auto config at
taskCount=8192 must render finite, mean>0.001).

Verified: e17 ALL PASS (T-1 + T0..T3); Release parity 4/4;
TILEPATHOCL smoke finite/valid; PATHOCL demote log "handing the
light pass to the native threads" + mean 0.0626.
Docs: `doc/features/gpu_lighttracing.md` auto-enable/zero-tail
section; `doc/features/mnee.md` `path.mnee.auto` property.
Also: parity-regression.sh now prefers Release over Debug (Debug
silently won twice, masking fresh binaries).

## Session cont. — PSR halflife decay (`eadcf8319`)

`path.regularization.halflife` (>0): Kaplanyan's decaying-schedule
scheme — sigma_eff = sigma*2^(-spp/halflife), spp from
`Film::GetTotalEyeSampleCount()/pixelCount` (eye-only counter; light
splats must not inflate it). Each path is seeded with the sigma in
effect at its birth — consistent mixture of a shrinking blur field,
unbiased in the limit.

CPU: `EffectiveRegularizationSigma(spp)` seeds both eye and light
paths. GPU: per-batch host recompute + taskConfig re-upload (same
site/pattern as the VCM merge-radius rewrite — PATHOCL
dense+wavefront, TILEPATHOCL/RTPATHOCL).

Also fixed in passing: `path.lighttracing.only` now implies
`enable=1` independent of the caustic signature (explicit debug-mode
request was silently ignored on diffuse scenes).

e99 T4: sigma=0.06+hl4 at 32spp lands ~5-8x closer to the sigma=0
anchor than the static image on PATHCPU and PATHOCL.
BLC: "Filter Glossy Half-life" exposed (visible when sigma>0).
Deployed via sync_dev_install.

Correction (`d0b2722f0`): the first cut of the zero-tail fallback
wrongly counted `photongi.caustic.enabled` as a compensating pass.
Under hbf the PGIC caustic cache is only consulted at depth != 0
(pathtracer.cpp IsCausticEnabled gate) so the depth-0 pool - the
dominant term - still needs a light pass. PATHOCL now demotes to
native-thread hbf whenever native>0 and disables lt/hbf/vc otherwise;
TILEPATHOCL keeps lt only when native>0. Verified taskCount=8192/4096
finite (mean ~0.042) and 65536 normal path intact.

Suppression-deposit contract fixes: e102 exposed two holes where
eye-side caustic suppression ran with no light-pass deposit.
PATHCPU/RTPATHCPU auto-LT rendered black because InitFilm gated
RADIANCE_PER_SCREEN_NORMALIZED on raw hbf only (auto injects lt, raw
hbf stays unset; InitFilm precedes ParseOptions promotion) - now
hbf || lt. TILEPATHCPU was in the auto-LT whitelist with no light-pass
machinery at all - removed from ApplyAutoLightTracing and explicit
lt/hbf requests now warn+ignore in StartLockLess; MNEE auto stays
(eye-side). e102 also fixed (bigmonkey .scn needs repo-root cwd):
matrix 11/11 PASS, e17 5/5, Release parity 4/4. Doc:
doc/engineering/light-pass-channel-matrix.md.

Review pass on the suppression-deposit contract exposed three more
holes, all in the same family:
- TILEPATHOCL/RTPATHOCL zero-tail demote was a no-op: InitTaskCount
  runs AFTER ParseOptions (tileRepository needs parsed values), so its
  cfg.Set() calls never reached CompilePathTracer, which reads the
  parsed pathTracer members. Suppression stayed on with
  lightTaskCount==0 in every case. The demote now clears
  pathTracer.lightTracingEnable/hybridBackForwardEnable/
  vertexConnectEnable directly, unconditionally - the native>0 keep
  rested on a false premise: TilePathNativeRenderThread is eye-only
  (no light sampler/splatter; identical to upstream). hbf->lt
  promotion on tile engines is now unconditional too: hbf+natives>0
  previously suppressed at ANY task count since only lt spawns light
  tasks.
- PATHOCL demote sat inside the camera-supported else, so lt plus a
  non-perspective/ortho camera left the tail at 0 with lt on -
  hoisted the lightTaskCount==0 check out of the else.
- InitFilm channel gates missed the third promotion source:
  path.vertexconnection.enable alone promotes hbf and runs the light
  pass. pathcpu.cpp and pathoclbase.cpp now gate
  RADIANCE_PER_SCREEN_NORMALIZED on hbf || lt || vc.
- TILEPATHCPU warn+ignore now also clears vertexConnectEnable.
e102: +3 TILEPATHOCL zero-tail rows (tile.size=32, aa=1 -> taskCount
8192; lt / hbf / natives=2 variants; TILEPATHSAMPLER required -
CheckSamplersForTile rejects Sobol on tile engines).
Docs: gpu_lighttracing.md, light-pass-channel-matrix.md corrected.

## Sobol InitNewSample film-cache round (landed 537a48c6d)

- Re-profiled on a different workload per megaplan protocol:
  portal-interior (sealed-indirect worst case), PATHCPU 640x360,
  `sample` 30s. Top-of-stack: SobolSampler::InitNewSample ~11% of
  render-thread samples — per sample it paid 2 std::set channel
  lookups, ~8 runtime udivs and a dead GetEngineFilm() ref.
- UpdateFilmCache() now snapshots subregion geometry, magic divisors
  (floor(2^32/d)+1 mulhi — exact for all u32 dividends) and channel
  flags once per subregion change; channels are frozen post-Film::Init
  (AddChannel/RemoveChannel throw on initialized films), only the
  subregion can still move (dyn-res). Bit-identical pixel order and
  RNG consumption; moments-validity checked live.
- Result (same-scene 25s sample): InitNewSample leaf 42.7k -> 24.5k
  (-43%); sampler group ~104k -> ~73.7k (-29%).
- Gates: Release build; parity-regression 4/4 on the final binary;
  portal-interior smoke render clean.
- New tool: dev-tools/profile_render.py (single-scene renderer for
  `sample` attribution).
- Next-ranked: HitPoint attr chain ~9% (GetDifferentials +
  InterpolateTri* + Buffer[] PLT stubs), PathVolumeInfo bookkeeping
  ~3% (has-volumes fast gate candidate), Metropolis
  GetSample/NextSample ~8% residual.

## Wavefront auto-promotion A/B — REJECTED (stall-class instability)

- Ran dev-tools/wf_ab.py (new interleaved off/on harness, PATHOCL
  Metal, 1280x720, 25s/render, min-of-2) after the M3a device-prefix
  rework promised a ~2.5x cornell win.
- Results are unstable in BOTH directions and include stall-class
  collapses: cornell on=0.27 vs off=5.41 Ms/s (7 spp in 25s — same
  signature as the documented stale-totals 6x regression: tasks
  landing past the stale launch size wait for the next resync);
  classroom on=0.65 vs off=7.18 in rep1 while rep0 won at 10.67;
  focused-ring on=2.34 then 7.44 vs off=5.58-5.81; luxball -17%
  consistently (3.42-3.60 vs 4.12-4.19).
- Verdict: `pathocl.wavefront` stays opt-in. The occasional +30-49%
  wins are real, but the collapse mode is correctness-adjacent
  (queued tasks starve) — promotion blocked until the stall is
  root-caused. Diagnosis: instrument queue-totals readback cadence
  and per-state launch sizing on Metal (EnqueueReadBuffer drains
  queues on this backend; a totals read that lands before
  BuildQueues completes may race the prefix write).
- Also this round: mesh fused hit-UV fetch into GetDifferentials
  (2767b2398) — removed a duplicate triangle+3-corner-UV read per
  intersect; smoke render + parity 4/4.
