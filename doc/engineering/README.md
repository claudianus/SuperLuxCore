# Engineering notes

Deep implementation notes, debugging findings and platform gotchas
extracted from the old monolithic `AGENTS.md`. Feature/user-facing
docs live in `doc/features/` (see its README for the index).

| File | Contents |
|---|---|
| [out-of-core.md](out-of-core.md) | Out-of-core memory: spilling, .lxm proxies, streaming |
| [light-bvh.md](light-bvh.md) | Light BVH strategy |
| [adaptive-clamping.md](adaptive-clamping.md) | Adaptive Robust Clamping |
| [light-linking.md](light-linking.md) | Light linking |
| [wavefront-queues.md](wavefront-queues.md) | Wavefront task queues |
| [lpe.md](lpe.md) | Light Path Expressions (LPE) |
| [cryptomatte.md](cryptomatte.md) | Cryptomatte AOVs |
| [path-guiding.md](path-guiding.md) | Path guiding — implementation notes |
| [gotchas.md](gotchas.md) | Build / platform / debugging gotchas |
| [openpbr-sss-findings.md](openpbr-sss-findings.md) | OpenPBR / SSS debugging findings |
| [huang-hair-findings.md](huang-hair-findings.md) | Huang hair — porting findings |
| [progressive-fill-order.md](progressive-fill-order.md) | Progressive fill order (viewport) |
| [diffraction.md](diffraction.md) | Diffraction grating material — implementation notes |
| [kernel-compile-performance.md](kernel-compile-performance.md) | Cold GPU kernel-compile time: parallel GetKernel, cache layout, benchmark gotchas |
| [threadfilm-transfers.md](threadfilm-transfers.md) | ThreadFilm SendFilm/RecvFilm layout mismatch — padded tile widths vs engine film (RTPATHOCL Metal crash) |
| [rtcpu-barrier-lifecycle.md](rtcpu-barrier-lifecycle.md) | RTPATHCPU pause barrier deadlocks: stale session Pause, edit-parked Stop |
| [pgic-beams.md](pgic-beams.md) | PhotonGI caustic beams: estimator math, chunking, volume photongi.enable=false gotcha, visibility/query gate split |
| [e90_pgic_update_crash.md](e90_pgic_update_crash.md) | Progressive PhotonGI update SIGSEGV: dangling `IndexBvh::allEntries` after shadow-cache swap — fixed by `SetEntries` rebinding |
| [pgic-gpu-deposits.md](pgic-gpu-deposits.md) | GPU photon deposits (B1′): light-task piggyback, drain/ingest plumbing, Apple arg-limit + shared-storage gotchas, taskConfig staleness fixes |
| [pgic-review-fixes.md](pgic-review-fixes.md) | Review-fix invariants: live/shadow fields, updateThread lifecycle, per-thread threadTaskConfig, PATHOCL-only deposit gate, parity guards |
| [metal-cb-batching.md](metal-cb-batching.md) | Metal deferred command-buffer batching: pending encoder batch, sync-point flush rules, ordering argument |
| [gpu-task-memory.md](gpu-task-memory.md) | Per-task GPU buffer diet: GPUTaskMnee split, ~115MB/thread saved when MNEE off, KERNEL_ARGS + NULL-slot pattern |
| [adaptive-error-review-fixes.md](adaptive-error-review-fixes.md) | Adaptive-error/NaN-collapse fixes, film-pointer serialization rebind (v2), PGIC update u_int wrap, broken standalone .flm/.rsm load finding |
