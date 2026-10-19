# Exact reuse of constant isotropic SSS phase evaluation

The dedicated CPU SSS loop still transformed directions into the generic BSDF frame, dispatched the volume material, read phase textures and reconstructed the same albedo at every guided vertex. For a medium already proven spatially constant by `HomogeneousVolume::InteriorScope`, with exactly zero evaluated anisotropy, all of that returns the same phase value and density during the scope.

`InteriorScope::IsotropicPhase` now lazily calls the original material evaluator once and reuses its exact value/PDF. It uses the original HG/Schlick evaluation arithmetic, including the existing scattering/absorption albedo reconstruction; it does not replace it with an approximate or separately rounded formula. The first requested evaluation initializes fixed stack-owned storage. Direction/flight sampling, physical coefficients, roulette, depth, boundary BSDF, global intersection and film accumulation remain unchanged.

The existing constant-input proof and spectral match checks are reused. Nonconstant textures and any nonzero anisotropy, including 1e-7, retain generic evaluation. A changed wavelength, hero/live mask, spectral pause or upsampling mode misses the scope. Nested scopes retain separate values and restore the previous coefficient context. No growing cache, heap allocation or radiance cache is introduced. `SUPERLUXCORE_SSS_NO_PHASE_SCOPE=1` is the diagnostic baseline.

## Evidence

Workspace evidence is `test-scenes/validation-2026-10-19/sss-volume-connect/`.

- `phase-cpu-comparison.json`: 14 CPU fixtures, 256 exact samples; 112 RGB/AOV/count buffers bit-exact against the previous delivered library. Includes spectral, textured, anisotropic, nested, interior-light and shadow-transparent controls.
- `phase-abba-summary.json`: full artist RGB and spectral images, 144 exact samples; all four runs in each mode bit-exact between the optimization and its diagnostic opt-out, in the same library.
- `phase-probe.log`: original evaluator equality plus nested-context/negative guards: 20 successful exact evaluations, 16 required misses. Source is `dev-tools/sss_phase_scope_probe.cpp`.
- `phase-abba-admission.json`: other CPU/UI activity contaminated the timing run. Provisional wall-time reductions are not adopted as a performance ratio or equal-time convergence result. CPU usage time is now recorded separately by the core benchmark for later diagnostics; it does not substitute for a clean wall-time gate.

The physical target is preserved; the broad Cycles convergence gap remains open. The installed-addon and Metal receipts must be read separately for the final candidate identity and actual outcomes.

## Proposal screens that were not promoted

This phase also tested three changes to the path proposals in the same authored scene. Their source snapshots and raw buffers remain in the evidence folder; their renderer code was removed before the phase reuse implementation.

1. Extra SSS boundary light candidates: each candidate retained the original light PDF and MIS partition; selection had a separate reciprocal-probability correction and remapped independent visibility draw. A 4,194,304-point conditional estimator test recovered its mean to 3.5e-6 relative error, and render energy controls passed. At 144 samples, 4 and 16 candidates only changed relative SSS RMSE from .460706 to .456541 and .455413, with extra candidate cost. Not promoted.
2. Ellipsoid-boundary direction proposal: reused object bounds as a proposal approximation only, while tracing real global geometry. Three RGB seeds reduced traced-ray workload about 2.5%, but interseed variance was 0.184% higher, within the small screening uncertainty. No broad convergence advantage was established. Not promoted.
3. SSS-local roulette normalization: relative SSS-entry importance reduced one-seed relative RMSE from .460706 to .431771, while traced-ray workload rose from 1013.04 to 1259.64 in the recorded buffer. The legacy roulette control was faster but much noisier (.956306). Neither proposal was adopted.

A separate RGB white-furnace diagnostic rejected an immediate BIDIRCPU switch: at 8192 requested samples, dense-sphere means were .79630 versus PATHCPU .82433. Coarser mid/thin screens also disagreed. This is an unresolved integrator/PDF/depth-compatibility investigation, not a verified bidirectional speed/quality comparison or a certified bug repair. Existing bidirectional connection MIS uses surface-style measures at volume vertices and applies roulette factors differently from stored recurrence PDFs; it needs a complete measure-consistent audit before using it for SSS convergence claims.

References used for the resampling experiment: PBRT 4e appendix A.2 (https://www.pbr-book.org/4ed/Sampling_Algorithms/Reservoir_Sampling), and Talbot, Cline, Egbert, Importance Resampling for Global Illumination (https://diglib.eg.org/bitstream/handle/10.2312/EGWR.EGSR05.139-146/139-146.pdf). The conditional-average correction was tested independently rather than treating a reservoir's output weight as an ordinary MIS PDF.

## Comparison-fidelity issue found during the installed repeat gate

The actual-profile control also varied between two runs with the same library, script, output path and phase-cache opt-out. Therefore those reconstructed-scene images do not establish a cache-induced difference; the frozen core scene and all phase A/B buffers remain exact. The actual-profile smoke is functional only.

A subsequent audit found that the matched Blender harness had not explicitly disabled Cycles Filter Glossy (RNA default 1) or indirect sample clamp (RNA default 10). Native configuration also auto-seeded PSR sigma .03 / halflife 64, confirmed in the installed render log. Earlier cross-engine convergence evidence is conditional on those settings and must not be described as an unclamped, unregularized comparison. That measurement contract is being repaired separately, with new references; it does not invalidate the phase evaluator's equality checks.
