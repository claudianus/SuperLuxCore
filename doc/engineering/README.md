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
| [restir-pt-design.md](restir-pt-design.md) | ReSTIR PT — estimator design, payload, staged plan |
| [gotchas.md](gotchas.md) | Build / platform / debugging gotchas |
| [openpbr-sss-findings.md](openpbr-sss-findings.md) | OpenPBR / SSS debugging findings |
| [huang-hair-findings.md](huang-hair-findings.md) | Huang hair — porting findings |
| [progressive-fill-order.md](progressive-fill-order.md) | Progressive fill order (viewport) |
| [diffraction.md](diffraction.md) | Diffraction grating material — implementation notes |
| [kernel-compile-performance.md](kernel-compile-performance.md) | Cold GPU kernel-compile time: parallel GetKernel, cache layout, benchmark gotchas |
| [hot-loop-allocations.md](hot-loop-allocations.md) | SpectrumGroup per-connect allocs, light-path result vector growth; PATHCPU +21% |
| [manifold-path-guiding.md](manifold-path-guiding.md) | MPG/PMS research survey + seed-importance design (E4 last item) |
| [threadfilm-transfers.md](threadfilm-transfers.md) | ThreadFilm SendFilm/RecvFilm layout mismatch — padded tile widths vs engine film (RTPATHOCL Metal crash) |
| [rtcpu-barrier-lifecycle.md](rtcpu-barrier-lifecycle.md) | RTPATHCPU pause barrier deadlocks: stale session Pause, edit-parked Stop |
| [pgic-beams.md](pgic-beams.md) | PhotonGI caustic beams: estimator math, chunking, volume photongi.enable=false gotcha, visibility/query gate split |
| [e90_pgic_update_crash.md](e90_pgic_update_crash.md) | Progressive PhotonGI update SIGSEGV: dangling `IndexBvh::allEntries` after shadow-cache swap — fixed by `SetEntries` rebinding |
| [pgic-gpu-deposits.md](pgic-gpu-deposits.md) | GPU photon deposits (B1′): light-task piggyback, drain/ingest plumbing, Apple arg-limit + shared-storage gotchas, taskConfig staleness fixes |
| [pgic-review-fixes.md](pgic-review-fixes.md) | Review-fix invariants: live/shadow fields, updateThread lifecycle, per-thread threadTaskConfig, PATHOCL-only deposit gate, parity guards |
| [metal-cb-batching.md](metal-cb-batching.md) | Metal deferred command-buffer batching: pending encoder batch, sync-point flush rules, ordering argument |
| [gpu-task-memory.md](gpu-task-memory.md) | Per-task GPU buffer diet: GPUTaskMnee split, ~115MB/thread saved when MNEE off, KERNEL_ARGS + NULL-slot pattern |
| [adaptive-error-review-fixes.md](adaptive-error-review-fixes.md) | Adaptive-error/NaN-collapse fixes, film-pointer serialization rebind (v2), PGIC update u_int wrap |
| [serialization-roots.md](serialization-roots.md) | Archive-root record symmetry: .flm/.rsm/.rst/config round-trip repair (T* roots both sides), pyluxcore unique_ptr/tuple binding gotchas |
| [cl2msl-scanner.md](cl2msl-scanner.md) | cl2msl translator span-scanner invariants: comment-paren trap, offline repro recipe, ReSTIR tail-ray coverage |
| [restir-pg-design.md](restir-pg-design.md) | ReSTIR PG (Zeng et al. SA2025) design — reservoir winners feed the GuideTree vMF fit, no new infrastructure |
| [path-space-regularization.md](path-space-regularization.md) | PSR/OPSR design (Kaplanyan'13, Weier'21) — BSDF α-inflation via BSDF.regularization, depth-gated, shared microfacet helper |
| [vc_pool_multiplicity_bias.md](vc_pool_multiplicity_bias.md) | VC pool connect-sum bias (thebox4 +28%): subpath-average normalization fix, replay winner's-curse residual |
| [property-variant-casts.md](property-variant-casts.md) | `Get<float>` on API-written doubles threw bad_lexical_cast (exact-roundtrip check); lldb catch-vs-throw recipe |
| [perf-ledger.md](perf-ledger.md) | Ranked bottleneck ledger: profile shares, hypotheses, constraints, A/B results, decisions |
| [light-pass-channel-matrix.md](light-pass-channel-matrix.md) | LT/HBF suppression→deposit contract: per-engine channel/sampler matrix, PATHCPU InitFilm bug, TILEPATHCPU exclusion |
