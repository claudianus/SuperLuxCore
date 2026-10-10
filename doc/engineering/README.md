# Engineering notes

Deep implementation notes, debugging findings and platform gotchas
extracted from the old monolithic `AGENTS.md`. Feature/user-facing
docs live in `doc/features/` (see its README for the index).

| File | Contents |
|---|---|
| [cuda-kernel-gate.md](cuda-kernel-gate.md) | 2.11.21 CUDA compile break (Metal-only idioms), GPU-less NVRTC CI gate, macOS lint + pre-push hook |
| [2026-10-09-checker-coordinate-portability.md](2026-10-09-checker-coordinate-portability.md) | 2.11.15 portable UV payload, actual Metal validation and stable object-space Checker coordinates |
| [2026-10-09-vector-mapping.md](2026-10-09-vector-mapping.md) | 2.11.16 direct linked TRS, Point/Texture/Vector/Normal semantics and CPU/Metal spectral validation |
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

[Texture-driven image coordinates](2026-10-09-image-vector-coordinates.md) documents optional image Vector input, raw spectral scope and CPU/Metal validation.

- [Normal data vectors for shader inputs](2026-10-09-normal-data-vectors.md)

- [Cycles Normal Map spaces and MikkTSpace data](2026-10-09-cycles-normal-map.md)

- [Rough matte transport and MIS directions](2026-10-09-roughmatte-transport.md)

- [Cycles Bump direction and linked-input semantics](2026-10-09-cycles-bump-direction.md)

- [Cycles Add Shader closure sums and transparent transport](2026-10-09-cycles-add-shader.md)

- [Imported OpenPBR lobe normals and independent coat](2026-10-09-openpbr-lobe-normals.md)

- [Cycles vector displacement spaces and Mikk corner data](2026-10-10-cycles-vector-displacement.md)

- [Verified private Cycles Vector Displacement Incoming direction](2026-10-10-cycles-displacement-incoming.md)

- [Cycles Backfacing spectrum](2026-10-10-cycles-backfacing.md): front/back emission color/strength fix; 28 private CPU/Metal checks; the scoped fix is included in verified2.11.24 deployment.

- [Current 2.11.26 standalone positive SSS boundary diagnostic](2026-10-10-cycles-positive-sss-diagnostic.md): confirmed remaining defect; auxiliary models excluded from production acceptance.

- [Verified2.11.24 Incoming, Backfacing, valid-zero and Microfiber deployment](2026-10-10-deployment-2.11.24.md): 591 guarded CI/fresh/actual CPU and Metal checks; full-scene goal remains active.

- [Cycles Bump zero Filter Width](2026-10-10-cycles-bump-zero-filter.md): 78 private CPU/Metal, spectral and smooth geometry checks; scoped fix included in verified 2.11.25 deployment.

- [Verified 2.11.25 Bump zero-width and SSS local diffuse deployment](2026-10-10-deployment-2.11.25.md): 627 guarded CI/fresh/actual checks; goal remains active and incomplete.

- [Cycles SSS all-channel local diffuse limit](2026-10-10-cycles-sss-local-limit.md): adapter-only fix; ordinary positive SSS remains open.

- [Cycles SSS constant RGB/Vector Scale coercion](2026-10-10-cycles-sss-scale-coercion.md): 46 private CPU/Metal checks and6 reviewed sheets; verified public2.11.26 deployment with86 guarded checks and9 sheets.

- [Verified 2.11.26 SSS RGB/Vector Scale coercion deployment](2026-10-10-deployment-2.11.26.md):86 guarded CI/fresh/actual checks; full goal remains active and incomplete.
