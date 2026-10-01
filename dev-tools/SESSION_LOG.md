## Session — zero-config PSR auto-seed + intersect dedup (72b42a4b7, 7178ad2e0)

**path.regularization.auto (default on)**: caustic-capable signature
(the same scan that promotes light tracing + MNEE) now also seeds
sigma=0.03 + halflife=64spp when the artist pinned neither property.
The Kaplanyan decay halves the blur every 64spp and snaps to 0 below
1e-5, so first frames resolve SDS energy and the render converges
unbiased - the "turn it on and caustics show up" Corona-style
behaviour without a pre-cache. Verified: focused-caustic-ring seeds
sigma=0.03, cornell (fully diffuse) stays unset, explicit
`path.regularization.sigma=0` and `path.regularization.auto=0` both
win. BLC pairs it with a `psr_auto` checkbox next to Filter Glossy
Sigma (commit in SuperBlendLuxCore).

**Scene::Intersect dedup (7178ad2e0)**: `GetSceneObject(meshIndex)`
ran twice per intersecting hit (bevel probe + IsCameraInvisible).
Hoisted to one fetch per path segment via a loop-scope pointer.
Parity 4/4 clean, zero semantic change.

**Path-guiding A/B (pg_ab.py, opt-in stays)**: 20s x2 reps,
640x480 PATHCPU - cornell +4.6% sps, pg-indirect +0.8%, pg-gallery
rep1 collapsed 0.374x (noise-dominated; relative-stddev proxy cannot
separate convergence benefit). Auto-promotion needs an RMSE-vs-ref
gate, not sps; stays opt-in.

**Pending measurement**: `psr_ab.py` renders a 4096spp unbiased
(lt+mnee, PSR off) reference for focused-caustic-ring, then A/Bs
auto-stack vs auto-no-psr vs psr-only RMSE at equal wall time.



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

## PSR light-pass blackout + Sobol pixel-fold — 2026-10-01

- mirror-sphere-psr.scn, `path.regularization.mindepth=0` +
  `sigma=0.15`, PATHCPU: 100%-black output (mean 0.00028, 12 px lit).
  Delta and `mindepth=1` rendered fine. Root-cause chain: light
  deposits reached `ConnectToEye` (`vis` 36k/48k connects, deposits
  rad ~0.05-2.7) but `MetropolisSampler::NextSample` drops every
  non-caustic result (`addonlycaustics=1` on the light pass).
  `sampleResult.isCaustic` comes from the adaptive partition: at the
  regularized mirror the sampled event is `GLOSSY|REFLECT`, so
  `LightPathInfo::AddVertex` recorded `firstVertexDelta=false` ->
  `IsAdaptiveTerminalHard` failed (`vertexGloss=0` -> solidAngle test
  can't pass) -> every deposit classified non-caustic -> dropped.
  FILMS probe = 0 at md0 vs 4.1M at md1.
- Fix A (`pathinfo.cpp`): `firstVertexDelta` now ORs the sampled event
  with the *static* material delta flag - classification must track
  the transport chain (delta), not the widened shading lobe.
- Fix B (`pathtracer.cpp` ConnectToEye): the LMNEE blocker gate read
  `bsdfConn.IsDelta()` (dynamic - false under PSR) and skipped the
  specular-manifold solve for every mirror-blocked connect; now
  `GetMaterial()->IsDelta()` (static), matching the doc split
  introduced for PSR.
- Fix C (`sobol.cpp`, second bug surfaced by cornell black):
  `FastDivByCached` magic = floor(2^32/d)+1 is unrepresentable for
  d=1 - with `overlapping=1` (default) `*bucketIndex / 1` evaluated
  to `(*bucketIndex * 1) >> 32` = 0 forever, folding every eye sample
  into bucket 0's pixel range (12-28 px lit strip, 1000x black).
  Same latent bug for tiletWidthCount=1. Added the `d <= 1` fast
  path. Also `pixelTileIndexX/Y` were swapped in commit 537a48c6's
  magic-div conversion (`/` where `%` belongs): X = tileIdx/tilesX
  clamped the sweep to ~96px and Y = tileIdx%tilesX skipped 40% of
  buckets - restored row-major order (X = index - Y*tilesX).
- Verification (160x90 PATHCPU haltspp 48-64): cornell 0.00028 ->
  0.377 full-frame; mirror-sphere md0 0.00028 -> 0.330 vs delta
  0.321 (PSR lobe blur, expected); mirror-maze reg/delta both lit;
  cols 0-159 rows 0-89 everywhere.
- Note: `path.lighttracing.only` is dead on PATHCPU (field set, never
  read) - flag only implemented on GPU task split.
- Also swept all stale fprintf probes (C2E/DEP/GEO/LID/FILM/FILMS/
  SC/PSR-MIRR/PSR-GLASS/PSR-SAMP/BSAMP/EVERT/EMISS/DHI/LASTV2/BLK/
  TAIL) added during debugging.

## SobolSequence byte-LUT + GPU firstVertexDelta parity — 2026-10-01

- Re-profiled e53 gauntlet (prism-conservatory 640x360 PATHCPU,
  `sample` 60s): sampler group ~7.1% total (SobolSequence::GetSample
  2.6%, InitNewSample 2.4%, Metropolis::GetSample 2.1%), embree
  intersect 7.2%, instance intersector 1.5%.
- SobolDimension: replaced the popcount-walk (up to 32 dependent
  iterations, data-mispredicted) with byte-blocked XOR LUTs - 4
  unconditional 1KB-row lookups per dimension, built once per
  RequestSamples for both directions tables. Bit-identical by
  construction (XOR distributes over XOR; LUT[d][b][v] is exactly the
  XOR of the table rows indexed by v's set bits in byte b).
- Result: SobolSequence::GetSample 51,926 -> 12,426 top-stack
  samples (-76%); sampler group ~142k -> ~89k (-37%). The remaining
  12k is the Owen ReversedBitOwen + BlueNoiseHash per dimension,
  which the LUT can't fold (nonlinear hash).
- GPU parity fix for the md0 PSR blackout: CPU pathinfo.cpp
  firstVertexDelta used the static material flag, but the CL twin
  (LightPathInfo_AddVertex, pathoclbase_kernels_micro.cl:2831) still
  read (event & SPECULAR) - on PATHOCL/TILEPATHOCL a regularized
  mirror vertex breaks the eye-hard chain and light deposits get
  classified non-caustic exactly like the CPU bug. Mirrored with
  Material_IsDelta(bsdf->materialIndex). PATHOCL md0 render verifies:
  mean 0.328 (CPU 0.330), no NaN, kernel compiles through cl2msl.

## Distribution1D hinted segment search — 2026-10-01 (cont.)

- Distribution1D::SampleContinuous: replaced the full-range
  std::upper_bound (log2(65) ~ 7 mispredicted iters) with a hinted
  linear walk (offset = u*count, bounded to 8/16 steps) + binary-search
  fallback when the hint misses (spiky env-map tables). Bit-identical:
  the returned segment satisfies cdf[offset] <= u < cdf[offset+1]
  either way.
- Callers: pixel FilterDistribution::SampleContinuous (every eye
  sample, ~1% profile share) and env-light Distribution2D sampling
  (sky2/infinite light NEE - bounded walk guards their spiky CDFs).
- Sanity: cornell 160x90 PATHCPU @48spp mean 0.37672 - matches
  pre-change 0.37671 within FP-drift band.

## ScatterEquiangular single-pass light weights — 2026-10-01 (cont.)

- HomogeneousVolume::ScatterEquiangular: the contribution-aware light
  selection evaluated weightOf(i) twice per eligible light (once for
  weightsSum, once in the pick loop) - each eval costs sqrt + 2*atan2.
  Now computes once into a stack array (lightCount <= 64 covers all
  realistic scenes; a fallback keeps the old two-pass loop for larger
  counts). Bit-identical: weightOf is pure, the values are reused.
- Context: post-Sobol-LUT profile shows a ~34k-sample libm cluster
  (sincosf_stret 14.5k + atan2f 14.0k + expf 5.5k) and
  ScatterEquiangular top-stack 22k; this halves its per-light trig.
- Sanity: cornell-vol-caustic 160x90 @32spp PATHCPU renders clean
  (mean 0.4104, no NaN).

## SobolSampler pixel-pass batch claims — 2026-10-01 (cont.)

- InitNewSample: passPerPixel claims are a per-sample atomic RMW on a
  false-shared counter array (16 counters / 64B line); in the filmless
  light-pass path every thread RMWs the SAME slot (index 0). Now
  claims runs of PASS_BATCH=4 per pixel via GetNewPixelPassBatch()
  (one AtomicAdd) and serves the other 3 locally; the adaptive gate
  evaluates once per run instead of per sample (bounded: a pixel
  converging mid-run sees <=3 extra samples, the same bound the
  unbatched loop applies on its next pick). runHit also skips the
  redundant noise/moments re-read for a pinned pixel.
- Statistics-preserving: the set of (pixel -> pass) assignments is
  unchanged; only claim granularity differs. Film accumulation is
  commutative, so a converged render is bit-equivalent; per-sample
  thread->pixel pairing shifts (already non-deterministic).
- SobolSamplerSharedData::GetNewPixelPassBatch added
  (AtomicAdd(&passPerPixel[i], k) - same contention slot, 1 RMW per k).
- Sanity: cornell 160x90 @32spp PATHCPU mean 0.3767 (matches pre-change
  0.3767), prism-conservatory mean 0.3380, no NaN.

## GPU PSR parity for delta materials + debug cleanup — 2026-10-01 (cont.)

- Path-space regularization (PSR) existed only on CPU for the DELTA
  materials: MirrorMaterial / GlassMaterial returned perfect-specular
  results on PATHOCL/TILEPATHOCL while PATHCPU answered the
  GlassMicrofacet GGX lobe (alpha = sigma). Caustic-path regularization
  therefore diverged CPU vs GPU on any mirror/glass-heavy scene.
- Mirrored the CPU contract on GPU:
  - MirrorMaterial_Sample: isotropic GGX-conductor lobe
    (allowTransmit=false -> F=1, threshold=0, result = kr * G2/G1).
  - MirrorMaterial_Evaluate: reflect branch (D*G/4cosI)*kr,
    directPdfW = VNDF reflection pdf.
  - GlassMaterial_Sample: full dielectric port (Transmit + Reflect,
    threshold = 0/.5/1 by kt/kr blackness, DispersiveIOR/Sellmeier /
    Cauchy IOR, Spectral_CollapseToHero on dispersive transmit,
    fromLight = rayFlags&LIGHT_RAY for the transmit pdf/Fresnel
    convention).
  - GlassMaterial_Evaluate: new GlassMaterial_GGXDielectricEval
    helper - transmit and reflect branches with VNDF half-pdf /
    reflection-pdf (renamed from a prototype overload; OpenCL C has
    no overloading).
  - Forwarding kernels/callers unchanged: regularization <= 0 keeps
    the old delta behavior bit-identical.
- PATHOCL mirror-sphere-psr @32spp: mean 0.33584 vs PATHCPU 0.33013,
  no NaN - same parity band as the pre-fix build (the lobe change is
  statistical, not bit-level).
- Removed debug fprintf spam left inside
  GlassMicrofacet_Sample (CPU header): _sdbg/_rdbg counters printed
  every 16th PSR sample - stderr noise + lock contention on the hot
  path.
- Verification (dispersive glass): prism-spectral-caustic @32spp with
  PSR sigma=0.15, mindepth=0 - PATHOCL mean 0.3709 vs PATHCPU 0.3720
  (0.3% band, inside spp noise). GPU kernel compiles through cl2msl,
  no NaN. Dispersive transmit (cauchyb=0.08) exercises the
  Spectral_DispersiveIOR + CollapseToHero branches.

## PMJ02 pixel-pass batching — 2026-10-01 (cont.)

- PMJ02Sampler::InitNewSample: same run-batched pixel-pass claims as
  Sobol (inherits SobolSamplerSharedData::GetNewPixelPassBatch).
  PASS_BATCH=4, adaptive gate once per run, filmless path batches the
  single shared slot.
- Sanity: cornell @32spp SOBOL 0.3767 / METROPOLIS 0.3766, no NaN.

## Volume const-param cache + GPU Owen memo + profiler r3 — 2026-10-01

- `HomogeneousVolume`: sigmaA/sigmaS/emission evaluated through virtual
  `Texture::GetSpectrumValue` + a fully built `HitPoint` per volume
  event (Scatter, ScatterEquiangular, TransmittanceEstimate), but
  homogeneous volumes are almost always `ConstFloat3`-parameterized.
  Ctor now caches the clamped spectra (`constSigmaParams` gate, off
  under the SSS albedo parametrization) and the RGB path
  (`!Spectral::Current()`) serves them without HitPoint/virtuals.
  Spectral renders keep per-path wavelength eval - values are
  path-dependent there.
- GPU `SobolSequence_GetSample`: Owen path recomputed the per-pass
  nested-uniform shuffle + 2 `BlueNoiseHash` on every dimension call
  (~16 dims/sample). `SobolSample` gains `shuffledPass`/`shuffledPassKey`
  (appended fields; `pass` offset unchanged for the `sampler_funcs.cl`
  peek). Keyed memo mirrors the CPU `SobolSequence` design;
  `SobolSampler_InitNewSample` invalidates the key on pass change.
  TilePath callsites pass NULL slots. Bit-identical scramble values,
  just computed once per (pass,pixel) instead of once per dimension.
- `Mutate`/`MutateScaled`: `static const` -> `constexpr` constants -
  drops the per-call static-init guard in the per-dimension mutation
  loop. Same values, same rounding.
- Verification: PATHCPU prism-conservatory 64spp finite, same
  distribution (PATHCPU seeds off wall-clock, no bit-compare possible).
  e94 serialization 3/3. e34 volume guiding parity cpu/gpu ratio
  0.9978/0.9987 (Metal/OpenCL). cpu-gpu-parity cornell 0.0009 reldiff
  PASS. e50 zero-config defaults + auto-caustic routing (e51) ok.
- Fresh profiler round (PATHCPU prism-conservatory 35s sample, busy
  threads): Embree ~35%, Metropolis GetSample ~12%, Sobol
  InitNewSample ~11%, HitPoint attr chain ~10%, libm ~6%,
  PathVolumeInfo ~3%, LightBVH ~3% (already dot-space). Logged as
  ledger r3 - remaining open items: hitpoint chain, PathVolumeInfo
  has-volumes gate, wavefront queue-totals stall root cause, GPU PGIC
  KD-tree update (big), GPU Sobol dimension LUT (needs GPU profile).

## Lt-depth parity re-check + wavefront sanity — 2026-10-01 (cont.)

- `lighttracing-depth-parity.sh` PASS 2/2 on d51df9036 (depth 2:
  cpu 78.95 / gpu 81.08, ratio 1.027; depth 4: 89.11 / 89.84,
  1.008). An earlier FAIL trace in this session was a mixed-source
  intermediate build from stash-revert churn, not a repo regression.
- Wavefront sanity: cornell PATHOCL 400spp, `LUXRAYS_WAVEFRONT_QUEUES=1`
  -> 12.1s vs dense 12.9s (+6%). The stall-class collapse from the
  09-30 A/B does not reproduce on the current tree; wavefront stays
  opt-in (auto promotion still gated on a consistent multi-scene win).
- e50 zero-config defaults + e51 auto-caustic routing ok in Blender 5.2.
- cpu-gpu-parity.sh cornell 0.0009 reldiff PASS on the final binary.

## Apple OpenCL-GPU duplicate selection fix — 2026-10-01 (cont.)

- cpu-gpu-parity.sh exposed a real segfault on `strands` (hair.scn):
  `Segmentation fault: 11` on the GPU render thread. lldb backtrace
  showed `clEnqueueNDRangeKernel -> gldExecuteKernel ->
  AGX::ComputeContext::prepareForEnqueue` null-deref on the
  opencl_runtime queue - Apple's deprecated OpenCL->Metal shim
  crashing on a kernel enqueue.
- Root cause: `opencl.gpu.use=1` selected BOTH `Apple M5 Pro
  OpenCLIntersect` (deprecated shim) and `Apple M5 Pro MetalIntersect`
  (same hardware, two render threads). The OCL-path thread hit the
  known translator buffer-arg-limit crash at first dispatch.
- Fix (oclrenderengine.cpp): on __APPLE__, after device selection, if
  a METAL_GPU was chosen, filter selectedDeviceDescs down to
  METAL|CUDA|NATIVE|OCL_CPU|VULKAN - drops the duplicate OpenCL GPU.
  No OpenCL-only GPU can exist on Apple; explicit
  `opencl.devices.select` still overrides. Mirrors the existing
  CUDA-only filter semantics.
- Verify: `Starting 1 OpenCL render threads` (was 2); strands
  renders 100% spp; full cpu-gpu-parity.sh now 6/6 PASS in 28s
  (cornell 0.0010, caustic_many 0.0004, mirror_maze 0.0673,
  vol_caustic 0.1745, spectral 0.0052, strands 0.0008).

## e17 MNEE-seedcache fix (vacuity pin) — 2026-10-01 (cont.)

- e17 T0 went vacuous after `path.regularization.auto` landed: PSR's
  seeded sigma blur resolves the same caustic class the test uses to
  prove MNEE activity, so mnee on/off was a wash ("scene does not
  exercise MNEE"). Correct product behavior - the test's PINS now also
  carry `path.regularization.auto = 0`. Re-run: 4/4 PASS,
  T0 mnee=on 0.2681 vs mnee=off 0.0000.
- e26_lightbvh_test.py: 10/10 PASS on the OpenCL-dup fix build
  (T3 mesh cpu-gpu parity 0.171920 vs 0.171330, T4 unbiased
  0.171920 vs ref 0.171714).

## GPU task-state histogram (LUX_TASKSTATE_DUMP) — 2026-10-01 (cont.)

- Added an env-gated per-batch state readback in
  pathoclopenclthread.cpp: after each FinishQueue, drains
  tasksStateBuff (sizeof(GPUTaskState)*taskCount) and logs a
  histogram. Gives the first occupancy signal for the wavefront
  stall investigation noted in perf-ledger.
- Correct-stride fix landed in the same commit: the buffer element
  is a GPUTaskState STRUCT (PathState at offset 0), not a u32 state;
  an earlier u32-stride read showed 181K/524K "other" - reading
  per-task struct fields, not states.
- Observed (cornell PATHOCL 720p, taskCount 524288): dense batches
  sit RT_NEXT_VERTEX≈334K + RT_DL≈190K; wavefront splits the same
  population into HIT_OBJECT/DL_SAMPLE_BSDF/GEN_NEXT_RAY/etc as
  designed - no stranded-tail distribution. A/B: dense 11.0 Ms/s vs
  wavefront 5.7 Ms/s on cornell (wavefront still opt-in; its win
  needs heavy-divergence scenes where the dense launch is dominated
  by the longest state).

## ExtTriangleMesh per-tri differential cache — 2026-10-01 (cont.)

- `sample` on prism-conservatory PATHCPU put `ExtMesh::GetDifferentials`
  at ~19% of render-thread hits: per hit it re-derives geometry
  dpdu/dpdv + dn1/dn2 from six vertex/normal/UV loads that are pure
  per-triangle constants on a baked-space mesh.
- `ExtTriangleMesh::BuildTriDiffCache()` (hooked into Preprocess, also
  re-run by serialization load) stores uv0/uv1/uv2, geometryDpDu/Dv,
  invdet, dn1/dn2 per triangle - every float operand order identical
  to the base path, so output stays bit-identical. dn* fetched via
  GetShadeNormal so appliedTransSwapsHandedness is preserved.
  Invalidate on ApplyTransform; skipped on no-UV/no-normal meshes and
  >2M-tri scenes (96B/tri page pressure not worth it).
- `ExtTriangleMesh::GetDifferentials` override serves layer-0 hits
  from the cache; instance/motion meshes keep the per-hit
  local2World path (not bit-safe to hoist).
- Verify: cpu-gpu-parity 6/6 PASS (unchanged reldiffs); prism PATHCPU
  128spp throughput 4.6-6.0 -> 5.8-6.3 Ms/s (+~5-25%,
  host-noise-bound). e26 LightBVH 10/10 unchanged.
- Cache gate added: `buffersFromFileMapping` skips proxy meshes -
  building it would fault every vertex/normal page at load and
  defeat the ray-driven residency path. Parity 6/6 re-verified.

## Metal offline metallib - PATHOCL cold-start 23.5s -> ~10s

- `newLibraryWithSource` JITs every process (~2.5s warm, ~20s cold
  for the 3.6MB PATHOCL program). Metal-1.x `atomic_uint`/
  `atomic_fetch_add_explicit` shims were the only blocker for the
  offline `xcrun metal` front-end - migrated the cl2msl shim to
  `metal::atomic<T>` (semantic-identical rename, same memory model).
- `OfflineCompileMSL` (metaldevice.mm): first cache-miss runs
  `xcrun metal -std=macos-metal2.4 -ffast-math` + `xcrun metallib`,
  stores `<key>.metallib` next to the `.msl`/`.json` translation
  files. `newLibraryWithURL` loads it in ~40ms.
- PATHOCL cold boot: 23.5s -> ~10s (cl2msl 3s + xcrun ~7s, no more
  per-process JIT). Warm boot: kernels compile 3190ms -> 40ms.
- Fall back to JIT silently when xcrun is absent (CI, stripped
  installs); the .metallib is written under the same pid-temp +
  rename scheme as the translation cache.
- Verify: luxball PATHOCL 320x180 boots clean cold/warm; parity
  re-run green (vol_caustic 0.19 reldiff on retry - MC noise).

## Scene::Intersect - pinned Embree accelerator pointer

- `AcceleratorConstSPtr` shared_ptr copy on every Scene::Intersect
  call paid two atomic refcount updates on the hottest call in the
  engine. Pinned `cachedEmbreeAccel` on Scene at dataSet build;
  Intersect now dereferences a raw pointer (falls back to the old
  lookup when the cache is unset - e.g. first Intersect before
  Preprocess).
- prism PATHCPU: 6.27 -> 6.76 Ms/s (~+8% on top of the diff cache).
- Parity: cornell + strands PASS.

## sampler.sobol.bluenoise.enable on by default

- SobolSequence::BlueNoiseHash already implemented (per-dim Owen-style
  scramble) but the prop defaulted off. Flip: same Sobol point set,
  permuted - zero bias, better pixel decorrelation at low spp.
- luxball PATHOCL 32spp: mean identical to 2.5e-5 rel; per-pixel
  variance redistributed (visually cleaner at preview counts).
- Speed: 4.93M vs 4.80M samples/s - within measurement noise on the
  busy host.
- Parity: cornell + strands PASS.

## Zero-config render audit (2026-10-01)

- luxball, no engine/sampler/denoiser props set: PATHOCL + SOBOL +
  auto-LT + auto-MNEE + PSR all select themselves, 24 passes render
  in ~6s, EXR means (0.28/0.30/0.37) physically sensible.
- pysuperluxcore API quirk noted: `Properties::Get(key)` throws on
  absent key (no default-return overload for strings); Film::Save()
  takes no args (uses film.outputs paths).
- Task-state histogram on prism PATHOCL: zero MK_DONE accumulation,
  all lanes cycling - dense launch is GPU-compute-bound (taskCount
  sweep confirmed ±12% noise floor at 64K-512K on M5 Pro), not
  stall-bound; the residual dispatch overhead is not recoverable
  without fusing advance+trace into a single kernel launch.

## Distribution1D::SampleDiscrete - hinted CDF walk (2026-10-01)

- SampleContinuous already had the u*count + bounded linear-walk
  fast path (8 down / 16 up) + upper_bound fallback; SampleDiscrete
  still ran upper_bound on every call. Same hint applies - result
  identical by construction (same segment either way).
- prism PATHCPU 6.76 -> 6.82 Ms/s (borderline noise, keeps the win).
- Parity PASS.

## opencl.task.count AUTO: 512K -> 128K (2026-10-01)

- luxball PATHOCL 256x256: 64K vs 512K identical means (reldiff
  7.8e-4 - MC noise), 5.11M -> 5.59M samples/s. The 512K pool was
  memory only, not throughput.
- AUTO now caps at 128K (still >1 task/px at 720p; RT viewport keeps
  its per-pixel formula). On luxball: GPUTaskMnee 954 -> 238MB,
  SampleResult 288 -> 72MB, GPUTaskState 299 -> 75MB - ~1.5GB
  device-side RAM saved per session with MNEE on.
- 8/4/2GB card caps unchanged (256/128/64K): AUTO users on low-VRAM
  hardware were already capped lower than the new default.
- Parity cornell + strands PASS.

## tilepathocl: light-task tail disabled on small/previewed tiles (2026-10-01)

- Symptom: `RTPATHOCL 320x180 + path.lighttracing.auto` printed
  "task count leaves no light-task tail ... disabling light tracing,
  hybrid and vertex connection" even though the intended split would
  leave a usable tail.
- Cause: `taskCount = tilePx / resolutionReduction^2` came out
  3600, `RoundUp(8192)` -> 8192, `taskCount > 8192` failed ->
  `lightTaskCount = 0` -> LT/hybrid/VC demote.
- Fix: bump `taskCount` floor to 16384 when LT/hybrid/VC is wanted
  (two workgroups: one eye tail + one light tail minimum). Cost:
  +8192 task slots (a few MB) when the tile is small - absorbed
  by GPU budget.
- Verified: 320x180 luxball now shows `path.lighttracing.auto`
  engaging without the demote warning.

## Metal EnqueueWriteBuffer: scope the conflict wait (2026-10-01)

- `EnqueueWriteBuffer` on a buffer conflict called `FinishQueue()`,
  which drains every committed CB - serializing unrelated in-flight
  work for one buffer's write.
- Now waits only on command buffers whose `buffers` set contains the
  target (transitively covered by in-order queue). Unrelated kernels
  keep overlapping the host memcpy.
- Parity cornell + strands PASS.

## Metal write/alloc hazards now use scoped waits (2026-10-01)

- Added `MetalDevice::WaitOnBuffer(buff)` - walks inFlightWork, waits
  only on committed CBs referencing `buff`, leaves unrelated work
  running. Same conflict-scan pattern EnqueueReadBuffer already used.
- Applied to: `EnqueueWriteBuffer` conflict path (was FinishQueue),
  `AllocBuffer` overwrite-with-same-size path, `AllocBuffer` free-
  on-resize path, `AllocBuffer` free-on-empty path.
- `FreeBuffer` keeps `FinishQueue` (the wrapper dies - all its users
  must drain).
- Effect: a write to one buffer no longer serializes other buffers'
  in-flight kernels. On PATHOCL this mostly shows up when film
  upload / debug counter drain overlaps trace dispatches.
- Parity cornell + strands PASS.

## PATHCPU hot-path micro-scan (2026-10-01, prism-conservatory)

`sample` profile on `prism-conservatory.scn` (PATHCPU, METROPOLIS, 640x360):
- Scene::Intersect 664, MetropolisSampler::GetSample 628,
  ScatterEquiangular 360, ExtMesh::GetDifferentials 295, BSDF::Init 157.

Findings, all already-optimal after earlier passes:
- `Mutate` uses constexpr-hoisted constants, division-only (no exp/log).
- `triDiffCache` caches per-triangle differentials - hit means few
  flops; misses only on instance/motion meshes (correct to skip cache).
- `GetLightSourceByMeshAndTriIndex` is O(1) table lookup.
- Light-path extra non-specular bounce (diffuse+glossy > 1) is a
  documented Metropolis stabilization - removing it breaks the
  average-luminance estimate; the earlier bail does not waste work
  (ConnectToEye runs before the bail).
- MNEE auto-enable fires on this scene (caustic signature), so a
  large share of `Scene::Intersect` is solver iterations - a solver
  iteration is ~1 shadow ray by design.
- ScatterEquiangular already evaluates weights in a single pass with
  a 64-entry small-array cache for the typical light count.

No new wins landed. Moving to startup-time wins next.

## Framebuffer: weight-channel atomicity (2026-10-01)

- `GenericFrameBuffer<4,1>::AtomicAddWeightedPixel` used `+=` on the
  weight channel while the RGB channels were `AtomicAdd`. Concurrent
  splats on the same pixel lost weight updates (a real race - the
  radiance sums stay correct but the per-pixel weight drifts low,
  which biases normalization on shared pixels in Metropolis /
  hybrid paths).
- Now `AtomicAdd(&pixel[CHANNELS-1], weight)` - all channels atomic.
- Non-atomic `AddWeightedPixel` unchanged (single-thread use only).
- Parity cornell + strands PASS.

## Splatter: skip zero-weight splat (2026-10-01)

- `AtomicSplatSample` filtered path called `AtomicAddSampleResultColor`
  even when `weight * filterWeight == 0` - 29 atomic channel writes
  per wasted splat.
- Blackman-Harris / box / gaussian filters all produce zero or
  near-zero weights outside the filter radius on many LUT cells.
- Skipped with `filteredWeight == 0.f` continue.
- Parity cornell + strands PASS.

## Metal useResource dedup (2026-10-01)

- `EnqueueKernel` re-called `useResource` on every `marshalTable`
  buffer, every dispatch. Metal's residency hint is idempotent
  within a command buffer - N× the work for identical effect.
- Added `pendingResident` (cleared with `pendingBuffers` on
  commit): `useResource` now only fires once per buffer per CB.
- Effect: small but uniform - PATHOCL kernels have ~10-20
  table-bound buffers (film, tasks, textures, lights) and run
  64 dispatches per pendingCB, so this removes hundreds of
  driver calls per batch.
- Parity cornell + strands PASS.

## wavefront auto-on: verified OFF is right (2026-10-01)

- `wf_auto_bench.py` claims "auto → on for GPU" but the code reads
  `(wavefrontMode == "on")` - `auto` actually resolves to OFF.
  Comment in pathoclbaseoclthreadinit.cpp already says "AUTO
  currently resolves to OFF".
- Measured on prism-conservatory 720p PATHOCL haltspp=32:
    off: 9.38s wall, ~4.15 Ms/s
    on:  110.8s wall, ~4.9 Ms/s -> ~12x slower on divergent scenes
- Conclusion: wavefront stays opt-in. On divergent scenes
  (caustics, volume scatter, deep refraction chains) the per-state
  task-queue compaction doesn't pay for its bookkeeping.
- `pathocl.wavefront = on` remains available for bench scenes where
  it might win (uniform transport).

## wavefront investigation + misc small wins (2026-10-01)

- `pathocl.wavefront = auto` resolves OFF (correctly - measured 12x
  slowdown on prism-conservatory vs dense); stays opt-in for scenes
  where it pays off. The `wf_auto_bench.py` docstring was misleading
  (auto didn't turn on wavefront).
- `EnqueueKernel` marshal dedup: `useResource` for `marshalTable`
  buffers now runs once per command buffer (was: per dispatch).
- `pathoclopenclthread`: fused eyeTask/lightTask sample-count loops.
- Film-splatter: skipped zero-weight splats (was paying 29 atomic
  writes per dead splat through the filter LUT).
- Framebuffer race fix (separate commit): `AtomicAddWeightedPixel`
  now atomically updates the weight channel.
- New helper `MetalDevice::WaitOnBuffer`: EnqueueWriteBuffer and
  AllocBuffer conflict paths now wait only on CBs referencing the
  target buffer - previously drained the whole queue.

## FilmSamplesCounts: O(1) total + SetSampleCount bugfix (2026-10-01)

- `GetSampleCount()` summed over `threadCount` doubles per call. On
  PATHCPU+Metropolis it ran once per splat during BCD warmup checks.
  Replaced with an atomic `total_SampleCountAtomic` fast-path;
  `AddSampleCount`/`SetSampleCount`/`Init`/`Clear` all maintain it.
- Found a real bug in `SetSampleCount`: the loop wrote
  `total_SampleCount[0] = 0.0` instead of `total_SampleCount[i]` -
  every GPU taskStats drain zeroed slot 0 (the GPU count) instead of
  clearing the native-thread slots. Total count could read stale or
  miss the GPU half. Fixed.
- `total_SampleCountAtomic` is the only authoritative read for
  total samples; the per-thread vector still feeds the 3 channel
  splits for stats reporting.
- Parity cornell + strands PASS; luxball PATHOCL clean.

## PathGuiding: single tree descent per bounce (2026-10-01)

Each bounce issued 3-5 ReadLeafAt calls (CanGuide + ReadCount + ReadPeak +
Pdf + IncidentEstimate + Sample) - every one walked the SD-tree. Public
Leaf* overloads (CanGuideLeaf / LeafCount / LeafPeak / LeafPdf /
LeafIncidentEstimate / SampleLeaf / ReadLeafAt / Warmup) let callers hoist
the leaf once per vertex. Refactored the three hot sites in pathtracer.cpp:
DirectLightSampling (DL-MIS pdf eval), RenderEyeSample RIS candidate loop
(K<=8 candidates share one descent), and the guide-side bounce branch.
Correctness: Leaf* evaluators are bit-identical to the path-based
wrappers (same CompWeights + LobePdf math on the same leaf). cornell +
strands parity PASS.

## PathGuiding::Record: drop redundant atomic load (2026-10-01)

`fetch_add + load` was two atomic RMWs where the returned previous value
suffices. Now one fetch_add + compare.

## TileRepository::NextTile - filmMutex-only merge (2026-10-01)

The old shape held tileMutex across Film::AddFilm, serializing all
TILEPATHCPU workers through the O(tile-pixel) merge. Split NextTile
into three phases: (1) tileMutex for queue bookkeeping, (2) filmMutex
alone for the pixel merge, (3) tileMutex for the convergence + next-
tile checks. Correctness: pendingTiles/todoTiles/convergedTiles access
is unchanged (still under tileMutex in phases 1+3); film.AddFilm is
still mutually exclusive across threads via filmMutex. TILEPATHCPU
luxball renders 4spp clean in 2.4s; no deadlock under 20 threads.

## Camera: cache worldToRaster / worldToCamera (2026-10-01)

`PerspectiveCamera::GetSamplePosition` ran `Inverse(camTrans.rasterToWorld)`
per call - a 4x4 matrix inverse on every ConnectToEye (light-tracing
vertex) and every shadow-camera query. Same for `OrthographicCamera`
(two sites) and `EnvironmentCamera::GetSamplePosition`
(`Inverse(camTrans.cameraToWorld)` per call). Added
`worldToRaster` / `worldToCamera` to `CameraTransforms` in
`projective.h` and `environment.h`; populated once in
`InitCameraTransforms` (called by `Update` on camera motion).
Correctness: identical matrix math, just hoisted. cornell + strands
parity PASS.

## Camera: cache inverse transforms, eliminate per-call Inverse() (2026-10-01)

`ProjectPointToFilm` did two 4x4 Inverse() per call (world->camera +
camera->raster). `PerspectiveCamera::GetSamplePosition` did another
Inverse(rasterToWorld) per ConnectToEye. Same on ortho/env.

Added `worldToCamera` + `worldToRaster` + `cameraToRaster` to
`CameraTransforms` (projective + environment; populated once per
InitCameraTransforms). New virtual getters `GetWorldToCamera` /
`GetCameraToRaster` / `GetWorldToRaster` on `Camera`; `StereoCamera`
forwards to the active eye. `ProjectPointToFilm` uses the cached
transforms now. Same math, hoisted.

cornell + strands parity PASS.

## SpotLight: cache alignedWorldToLight (2026-10-01)

`SpotLight::Illuminate`, `IsAlwaysInShadow` and `LightFocusEmit`'s spot
branch each ran a 4x4 Inverse() of `alignedLight2World` per call (once
per NEE sample + once per focused emission on point/spot). Cached the
inverse on Preprocess as `alignedWorldToLight`, exposed via
`GetAlignedWorldToLight`, and rewired all three callsites. Identical
math, hoisted.

cornell + strands parity PASS.

## Light sources: cache worldToLight / inverse transforms (2026-10-01)

Every NEE `Illuminate()` and env `GetRadiance`/`GetEnvUV` ran a 4x4
`Inverse(lightToWorld)` per call. Added `worldToLight` to
`NotIntersectableLightSource` (populated in `Preprocess` where the
parser has already set `lightToWorld`). Rewired:
- InfiniteLight::GetEnvUV + GetRadiance
- ConstantInfiniteLight::GetRadiance
- MapPointLight::Illuminate
- MapSphereLight::Emit + Illuminate
- EnvLightVisibilityCache (visibility map lookup per ray)

SpotLight got `alignedWorldToLight` (already committed) plus
ProjectionLight got `worldToAlignedLight` + `inverseLightProjection`
for Emit + Illuminate.

cornell + strands parity PASS.

## Shadow-transparent hits: skip BSDF::Init dead work (2026-10-01)

Scene::Intersect's transparent-shadow path still ran the full
BSDF::Init - triangle-light lookup, Bump evaluation, Frame
construction, and the HitPoint::Init GetDifferentials inverse. All
dead work: the path continues, GetPassThroughShadowTransparency reads
a cached Spectrum field, and transparent textures need only
defaultUV.

- BSDF::Init(throughShadowTransparency=true) now skips IsLightSource
  lookup + Bump + GetFrame.
- HitPoint::Init(throughShadowTransparency=true) replaces
  GetDifferentials with InterpolateTriUV (needed by transparency
  textures) and a canonical CoordinateSystem frame; dndu/dndv zeroed.

cornell + strands parity PASS.

## Spectral::ProjectToRGB - fold XYZ→RGB into per-bin coefficients (2026-10-01)

Every light-splat path ran PrepareRGBProjection once + ProjectToRGB
on ~14 spectral fields, each call doing bins→XYZ dot + 3x3 XYZ→RGB
matmul + 3 divides.

The matrix and white normalization are linear over wavelength - hoisted
into per-bin `cr/cg/cb` coefficients in PrepareRGBProjection. Per-field
cost drops to one masked 3-FMA dot per channel (9 FMAs + no division).
Statistically-identical (same linear operator; ULP-level associativity
change, well inside the 0.2% parity tolerance).

cornell + strands parity PASS.

## GPU spectral projection - shared RGBProjector (2026-10-01)

Device mirror: SampleResult_ProjectSpectralToRGB re-ran
Spectral_ProjectToRGB per field (~30x), each redoing the 3-bin CIE
SPDs, XYZ→RGB matmul and white-point divide.

- spectral_funcs.cl: new SpectralRGBProjector +
  Spectral_PrepareRGBProjection (per-bin cr/cg/cb coefficients, one
  time per sample result) + Spectral_ProjectToRGBWith (masked 3-FMA
  dot per field). Original Spectral_ProjectToRGB kept for the
  micro-kernel single-field path.
- sampleresult_funcs.cl: build one proj, reuse over all fields.
- `thread` address-space qualifier on the projector pointer - Metal
  rejects unqualified pointers.

cornell + strands parity PASS.

## SampleResult: full-field Init/Reset (2026-10-01) - correctness fix

`SampleResult::Init` only zeroed radiance + a handful of scalars;
`ResetEyeSampleResults` covered only `sampleResults[0]` and still left
alpha/depth/position/normals/materialID/objectID/uv/rayCount/isCaustic/
isHoldout/directShadowMask/indirectShadowMask/useFilmSplat unset. Every
`AddLightSampleResult` splat (light-path connect, MNEE, SSP tail) wrote
stale heap values into the film, AOVs and the denoiser.

- `SampleResult::Init` now defaults all splat fields (alpha=0,
  depth=inf, position=inf, normals=0, ids=0, uv=inf, shadow masks=1,
  isHoldout/isCaustic=false, all spectra=0, lpeRadiance=0).
- New `SampleResult::Reset()` - Init with stored channels + radiance
  size. `ResetEyeSampleResults` calls it (idempotent, single call).
- `useFilmSplat` intentionally not in Init (set once by
  InitEyeSampleResults; light-path SRs use default true).

cornell + strands parity PASS. This also fixes latent garbage in
RADIANCE_PER_SCREEN_NORMALIZED light splats on multi-connection paths.

## MNEE: fuse residual + Jacobian per Newton iteration (2026-10-01)

MneeSolveSingleVertex ran MneeResidual then
MneeGeometricTermWithJacobians per iteration - both recomputed
wi/wo/eta flip/h/~sqrt/Normalize. ~10-15 vector ops + sqrt duplicated
per iteration on the dominant PATHCPU light-sample path (MNEE is the
top hotspot per profile).

Added `residualOut` + `residualOk` params to
MneeGeometricTermWithJacobians. The function now emits the constraint
residual alongside J1 from the same half-vector work; a new hLen
degenerate-guard replaces the residual-side early-outs (bit-identical
fail surface). `residualOk` distinguishes "no valid h" (fail) from
"Jacobian singular" (retry-able) - the previous `g == 0.f` gate
conflated them and could terminate a solve that had a valid descent.

cornell + strands parity PASS.

## MNEE: reuse solver's last-iteration g/dets - drop redundant post-solve Jacobian (2026-10-01)

After MneeSolveSingleVertex converged, both the eye-side
MNEEDirectSampling and light-side LMNEEConnectToEye ran
MneeGeometricTerm(x0p, ep, vtx) at the solved vertex - recomputing
wi/wo/eta/h/J1/J2/dets that the final Newton iteration had already
produced.

- MneeSolveSingleVertex gains gOut/det1Out/det2Out, filled on the
  converged iteration (resNorm < 3e-4f) before returning true.
- Both callsites bind them and pass to the contribution assembly;
  MneeGeometricTerm() is now dead and removed.

Saves one full Jacobian eval per successful connect (the heaviest
path of MNEE).

cornell + strands parity PASS.

## MNEE chain: reuse converged Jacobian for the post-solve geometric term (2026-10-01)

Same pattern as the single-vertex solver, one level up: after
MneeSolveChain converged, both the eye-side MNEEMultiConnectToEye and
the light-side LMneeChainSolveAndEval re-ran MneeChainJacobian at the
solved chain to build geoBlocks for the dx_1/dy geometric term.

The Newton loop had already produced that Jacobian on the iteration
where maxResidual fell below 1e-5f. MneeSolveChain now writes its
converged blocks into an optional blocksOut; both callsites pass a
local geoBlocks and skip the re-eval (and the per-block reprojection
cost of MneeChainJacobian).

cornell + strands parity PASS.

## luxrays::Buffer accessors moved inline (2026-10-01)

operator[], Count(), Data(), operator bool() and the span conversion
were defined in buffer.cpp and bound by `template class Buffer<…>`
explicit instantiation - so the .h decl was the only one visible to
callers and every normal/vertex/UV/triangle fetch paid a PLT stub +
out-of-line call (sample-visible as `Buffer::operator[]` +
`DYLD-STUB$$…` entries in CPU profiles).

Moved the five accessors into the header inline. The .cpp retains the
explicit instantiation (ODR still satisfied; cold functions like
Allocate/SpillToFile stay out-of-line).

cornell + strands parity PASS.

## HitPoint::Init sceneObject overload - one NamedObjectVector::GetObj saved per hit (2026-10-01)

BSDF::Init already fetched the SceneObject (rayHit.meshIndex); HitPoint::Init
fetched it again to reach objectID + GetExtMesh. Added an overload taking
SceneObjectConstRef; the meshIndex signature is now a delegating shim
(used by the point-on-surface BSDF::Init variant).

cornell + strands parity PASS.

## SampleResult::HasChannel -> u64 bitmask (2026-10-01)

AtomicAddSampleResultColor ran ~20 HasChannel() calls per splat; each was a
std::unordered_set<FilmChannelType>::count() bucket walk. SampleResult now
carries a channelsMask computed once in Init() and HasChannel is a shift+and.
cornell + strands parity PASS.

## FilmSamplesCounts: pad per-thread counters to separate cache lines (2026-10-01)

The old layout packed three doubles per thread into adjacent vector slots:
threads i and i+1 shared a 64B cache line, so every per-sample
AddSampleCount() bounced the line between cores. Replaced with an
alignas(64) PerThreadCounts struct (one line per thread).

cornell + strands parity PASS.

## FilmSamplesCounts: pendingTotal revert (2026-10-01)

The batching deferred the shared atomic push but added a pendingTotal write
per call. On ARM (no pause hint in AtomicAdd) the shared-line RMW is a CAS
spin - batching didn't help because the CAS still ran per splat when the
batch wasn't hit, and the added per-call write cost more than the save.

Kept: aligned_alloc + 64B-per-thread struct padding (that part works -
removed neighbour-thread line sharing).

## Sobol PASS_BATCH 4 -> 16 (2026-10-01)

passPerPixel is a shared u_int array - 16 pixels per 64B line. Each
GetNewPixelPassBatch() does an atomic RMW on one slot; with batch=4 that
was one invalidating write per 4 samples per thread. 16 cuts the RMW rate
and line bouncing 4x. Adaptive-gate drift bound grows from 3 to 15 samples
per converged pixel - still negligible at production spp.

Note: absolute throughput numbers today are unreliable - vitest workers
were running on the box during measurement.

## Light-path sampleResults: keep vector sized, track `used` (2026-10-01)

RenderLightSample ran sampleResults.clear() then AddLightSampleResult's
resize(size+1) per vertex - every clear() destroyed each SampleResult's
inner SpectrumGroup (heap free) and every Init() reallocated it. With
~10M light samples/sec on PATHCPU that was 2 malloc/free pairs per
light-path vertex.

Now: vector stays at maxPathDepth+2 capacity; AddLightSampleResult writes
used slots in place. Plumbed `u_int &used` through RenderLightSample,
ConnectToEye, the LMNEE single/multi/tail chain and the
ConnectToEyeCallBackType typedef; added a `used` bound on
Sampler::NextSample (default SIZE_MAX keeps other engines' callsites
unchanged) and AtomicAddSamplesToFilm.

BakeCPU's bind callback gets the extra arg via placeholders::_6.
cornell+strands CPU/GPU parity PASS.

## Round: splat-path + accept-path micros (2026-10-01 cont.)

- `ProjectSampleResultToRGB`: 13 Spectrum fields each ran the 4-bin
  dot even when they had never been written - Black() early-out added
  (the ProjectToRGB inner check saved the loop but not the call).
- Metropolis accept: `currentSampleResults = sampleResults` copied the
  whole maxPathDepth+2 vector including stale slots; now bounded by
  `used` + resize-once at accept.
- `FilmSamplesCounts::AddSampleCount`: `pendingTotal` field existed but
  was never accumulated - fetch_add ran per-splat. Wired to flush at
  SAMPLE_COUNT_BATCH=64.
- Audited and found already-optimal: GetNewBucket atomic (amortized by
  bucketSize*superSampling), cachedEmbreeAccel pinned pointer (no map
  lookup), FilmDenoiser::AddSample early-out, AtomicSplatSample LUT
  walk + zero-filterWeight skip + subRegion clamping.

Remaining leaf hotspots are structural: InitNewSample adaptive re-pick
loop (algorithmic), embree BVH traversal (intrinsic), MetropolisSampler::
GetSample replay math (MLT-inherent).

## Round closeout + structural candidates (2026-10-01)

Profile deltas on prism-conservatory 640x360:
- AddSampleCount 19k->12.6k (33%) - pendingTotal now actually batched
- Metropolis::NextSample 3.5k->2.2k (37%) - accept copy bounded by used
- _xzm_free 2.1k->1.6k (26%) - light-path vector keep-sized kills churn
- ProjectToRGB hoist - PrepareRGBProjection once per path, was per splat

Audited, already optimal (no action):
- LightBVH::NodeImportance - dot-space trig, no asin/acos
- GetAccelerator - cachedEmbreeAccel pinned pointer, no map lookup
- SobolSampler::GetNewBucket - amortized bucketSize*superSampling
- SobolSampler::UpdateFilmCache - 6-load + 4-compare early out
- RandomGenerator::floatValue - 2048-uint buffered taus113

Structural candidates for next round (multi-hour each):
1. Per-thread splat accumulation buffers + merge at Film::Update -
   collapses the per-pixel AtomicAdd RMW chain entirely.
   High risk: changes convergence accounting + memory ordering.
2. SampleResult size reduction (~480B) - lpeRadiance array + 14
   Spectrum fields are ~150B; pool+compact saves every vertex's
   copy cost. Serialization-version bump required.
3. Metropolis GetSample replay loop - vectorize the stamp-delta
   walk (SIMD across the 4 Sobol dims); needs a path-length
   histogram first to confirm the distribution favors it.

## Re-profile under lighter load (2026-10-01)

Load 4.60/8.83/13.27, cleaner than the earlier 46+ round:
- SobolSampler::InitNewSample 24.1k - algorithmic (adaptive re-pick
  + bucket walk + Sobol hash)
- embree BVHN 21.5k - intrinsic
- MetropolisSampler::GetSample 20.8k - stamp replay loop (MLT core)
- AddSampleCount 14.5k - now the 3 per-thread field stores +
  pendingTotal batch check; the fetch_add is amortized
- Distribution1D::SampleContinuous 6.2k - CDF binary search, inherent
- AtomicAddSampleResultColor 4.5k - per-channel atomic RMW chain,
  needs per-thread buffers to kill

Audited (no wins found): BSDF::Init hit/shadow gating, EmbreeAccel::
Intersect field-copy, MetropolisSharedData atomics (correctly
cooldown-gated), GetNewBucket, UpdateFilmCache, taus113 buffer,
scene::Intersect pinned accel.

CPU micro floor confirmed. Structural next: per-thread splat
accumulation + merge, or SampleResult size cut.

---
### 이후 후보 (backlog)

- PATHCPU deferred splat queue: `FilmSampleSplatter::AtomicSplatSample` -> per-thread `(x,y,weight,SampleResult)` queue, flush at convergence-check boundary. 데이터 채널(last-write-wins)은 즉시 splat 유지, 색 채널만 지연. 메모리 ~16B×depth×in-flight samples. TilePathCPU 스타일이지만 큰 film이 아니라 queue를 쓰는 이유 = full-film 복사 비용 회피. **실구현 복잡도 M-H.** queue 크기 상한 = filterWidth²×예상 SPP, flush 시 subRegion 클램핑 포함.
- SpectrumGroup packed bools: `firstPathVertex` 등 bool 필드 → 1B bitmask로 합쳐 `SampleResult` 크기 추가 감소.
- SampleResult 성장 패턴: `maxPathDepth.depth + 2` 고정 cap인데 실제 경로가 이보다 깊으면 realloc — `used`는 쓰인 슬롯 카운터지만 capacity는 한 번 확장되면 그대로니, `sampleResults.reserve()`를 엔진 init에서 한번.
- `channel_LPEs[i]` 접근: `lpeRadiance`가 `nullptr`일 때 안전하게 early-out 하는지 Film 측 검증 — 현재는 `HasChannel(Film::LPE)`가 gate하니 ok.

---
### r6 — atomic fetch_add (1a6450564, 82dd150e9)

`AtomicAdd(float*)` was a `boost::interprocess` CAS loop — retry storm when
18 threads splat the same texel. Replaced with `std::atomic_ref<float>::fetch_add`
(ARM `ldadd`, x86 `lock xadd`) via `__cpp_lib_atomic_float`. `AtomicAdd(T*)`
same treatment. `AtomicMax(float)` swapped to `compare_exchange_weak` (max
has no single instruction) — still CAS but one branch less per retry.

`AtomicAddIfValidWeightedPixel` already had NaN/Inf pre-filtering;
`AtomicAddWeightedPixel` keeps it. The `pixels` array stays `float*` (not
`atomic<float>`) — that's the boundary between GPU merge (untouched, uses
OCL side) and CPU scatter.
