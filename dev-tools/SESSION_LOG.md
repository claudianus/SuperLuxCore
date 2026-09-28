
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
