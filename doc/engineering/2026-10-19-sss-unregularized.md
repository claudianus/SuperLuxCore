# SSS default fidelity and raw comparison correction

The previous matched Blender benchmark disabled denoising/adaptive sampling but did not explicitly disable every transport regularizer. Installed-matching Cycles 5.2.2 (`d13f752e3b9c4f8c261cda552b1021f8bcc0382c`) defaults to Filter Glossy 1 and indirect sample clamp 10. Native logs showed Auto PSR sigma .03 / half-life 64. Consequently, earlier cross-engine convergence numbers are conditional historical results, not evidence for a mutually unclamped/unregularized comparison. Notice files were added to the owned historical evidence directories rather than deleting their raw data.

## Product behavior

Implicit Auto PSR no longer seeds rough-lobe blur when the scene contains a parametrized homogeneous SSS volume. This preserves the authored bulk/boundary target at finite samples. Authored positive sigma and an explicit core `path.regularization.auto=true` remain opt-ins. Explicit zero remains zero; non-SSS glass retains its existing Auto policy. The policy concerns fresh render configuration; it does not add persistent origin tracking across scene edits.

The Blender Auto tooltip explains the SSS exception. Its ordinary exporter leaves default Auto unset, so the core policy applies. A positive artist-authored sigma remains an explicit blur request.

## Measurement contract

`sss-unregularized-v2` explicitly disables PSR, Filter Glossy, both Cycles-style clamps, variance clamp, denoising, adaptive sampling and guiding; it retains Cycles reflective/refractive caustic paths. The two engines share exact authored camera, geometry, material input and light fingerprints. Mesh indices are canonicalized by exact coordinate loops, preserving winding; no coordinate tolerance or quantization is used. Each engine is compared against its own independent finite-sample reference because OpenPBR refractive bulk transport and Cycles BSSRDF are distinct models.

The writers record effective native properties / actual Cycles RNA settings, source hashes before rendering and native payload identity before and after Blender rendering. The core renderer uses `RenderConfig.GetProperties()` to inspect raw configuration: normalized `GetProperty()` currently omits some newer zero/default fields and cannot prove those settings. Writers reject source changes during rendering. The analyzer rejects legacy/missing metadata, changed authored scenes, enabled blur/clamp and disabled Cycles caustic paths unless historical analysis is explicitly requested.

Evidence: `test-scenes/validation-2026-10-19/sss-unregularized/`.

- `pilot-plan-runs.json`: stock Blender native/Cycles, 64 exact samples, same authored SHA `d913ff75bc07a85ff0d4d70f196668558909667d40d188d5291ea628cb16816c`, all checked controls zero/off.
- `serialization-check.json`: native exported-scene core rendering is bit-exact with the Blender native film at 64 samples.
- `policy.json`: 32 fresh-config cases across CPU and Metal engine configuration, including SSS opt-ins and unchanged non-SSS glass policy.
- `contract-test.log`: 12 positive/negative analysis cases; sources in the adapter's `dev-tools/sss_comparison_contract_test.py`.
- `cpu-comparison.json`: 14 CPU fixtures / 112 RGB, AOV and count buffers exactly equal to the phase-scope candidate. These fixture materials do not exercise the removed Auto PSR blur.
- `metal-comparison.json`: two Metal fixtures, exact counts and RGB max differences 1.19e-7 / 2.38e-7, within 1e-6.
- `cancel/result.json`: cancellation .0329 seconds, recovery bit-exact.
- `installed-wheel.json` and `installed-source-preservation.json`: complete wheel/cache/site delivery; 393 installed Python sources preserved except the intended PSR tooltip file.
- `installed-default/result.json`: ordinary installed exporter, implicit Auto, no SSS PSR blur; stock Blender final render passed. `installed-raw/result.json`: current installed spectral 144-sample raw buffer exactly equals its saved EXR, with no reported errors. `installed-processes.json` confirms both children were reaped. No reconstructed old/new image equality is claimed.

Current native payload SHA256: `972eaae229f3d39ab77c4f34602e167fb16495fdcbbd714db552a4d8fcdbaa70`. Immutable complete wheel SHA256: `508a95ec32e493ce42c9e41c8e97f0adb9cf2f138b6594204e6794e85be54f2e`.

## Direct Cycles source audit

Local source is `/Users/modumaru/Desktop/code/blender-5.2/intern/cycles/kernel/integrator/`. `subsurface_random_walk.h` uses a dedicated walk, throughput/albedo channel selection, opposite-interface direction guidance and combined distance/direction/channel MIS. It uses a shared maximum-albedo diffusion length as a proposal simplification. The compiled source also switches nonzero anisotropy to similarity coefficients after bounce 9; at the zero anisotropy used in this comparison that coefficient transformation is an identity. `subsurface.h::subsurface_shader_data_setup` creates a diffuse exit BSDF. Copying that exit model would change the refractive bulk boundary target; it is not an overhead-only optimization.

The new native reference profile confirms active work in the dedicated SSS loop and global Embree intersection. It also captured a late tile phase with one active render worker. This one-second diagnostic is evidence for further geometry/scheduling investigation, not a total-run percentage or an established optimization gain. No local-only geometry shortcut or transport similarity approximation was adopted.

The full Cycles speed/convergence goal remains open. Shared-workstation reference/functional timings do not establish uncontended equal-time performance.

## Corrected raw convergence screen

`quality-summary.json` records seeds 17/53/97 at 320x180, CPU4, all raw controls disabled. References use 4096 fixed samples and independent seed 181. Each engine's relative RMSE is measured on the common SSS mask against its own model reference; this is not an image-equivalence claim. Finite references still contain noise.

| Engine | Samples | Mean Blender render seconds | Mean relative SSS RMSE |
| --- | ---: | ---: | ---: |
| SuperLuxCore | 144 | 8.7014 | .455906 |
| Cycles | 144 | 3.2199 | .065554 |
| Cycles | 400 | 8.8605 | .036964 |
| Cycles | 625 | 13.9662 | .029930 |

All jobs were sequential, with owned child cleanup receipts and matching authored fingerprints. These are shared-workstation quality screens (`performance_eligible=false`), not certified speed ratios. The near-time rows show the unresolved raw convergence problem plainly. Removing overhead has not closed the gap. The next substantial proposal work should target light connections through the refractive SSS exit while preserving its boundary evaluation/PDF, with full support and model-specific mean checks; replacing the exit by Cycles' diffuse model would change the target.
