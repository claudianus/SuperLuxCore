
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
