# SSS learned-direction screen and guiding model-selection units

The remaining SSS convergence gap was tested with a learned incident-continuation field while retaining the dedicated CPU interior loop and its existing Dwivedi direction/distance proposals. The physical coefficients, global geometry, refractive boundary, roulette, depths and raw film were retained. This experimental SSS integration was removed after its three-seed screen did not justify adoption. The general guiding model-selection unit correction described below remains in source.

## Experimental SSS integration

Evidence: `test-scenes/validation-2026-10-19/sss-learned/`. The removed implementation is archived in `final-prototype/`; earlier conditional-flight sources and their hashes are also retained. Consumed module overlays and temporary probe binaries were removed after use.

- A 16-sample pilot (seed 211) collected actual completed-path continuation targets, using a fixed 64-entry path-local reservoir. Pilot recording was serialized; it did not query changing read trees. The pilot image was bit-exact with the prior same-thread renderer when proposals were disabled.
- A separate render loaded an immutable table, with no online recording. Query/sample counters and table hashes confirmed actual use and a stable table. The pilot produced 149 warm leaves in a 20,876-byte table. Tree growth was capped at 1,024 leaves; observed process peak RSS was about 225 MB.
- The learned direction mixture used the full-support base direction proposal and retained its exact conditional distance distribution: `q(w,t) = [(1-s) q_base(w) + s q_learned(w)] q_base(t|w)`. A different initial variant folded learned directions into physical-extinction components; it also failed the cost screen.
- Learned proposals were limited to proven closed opaque SSS direct-occlusion domains, without MNEE, so direct-light MIS at a volume vertex could not use a stale competing density. Other cases retained the original proposal. Generic guiding remained disabled.
- Training corrected the nonuniform direction measure in the fitted moment: `Lhat/sqrt(q_direction)` for the second-moment target, `Lhat/q_direction` for the mean target. A 2,097,152-sample constant-field test recovered angular integral .999613, weighted mean z -.000126 and second z moment .333745. The uncorrected sampling measure had mean z .319202. These fits shape a proposal; they do not replace traced lighting.
- Histogram units were scaled to avoid the existing guide-training target cap. The traced radiance was not scaled or clamped. The corrected pilot's largest pre-scale target was 8,746.24; scale .0001 kept all observed records below the guide-training cap of 25.

The direction/conditional-flight mixture separately passed 2,097,152-sample transport moment tests. With the feature disabled, all 112 buffers across 14 CPU fixtures were bit-exact with the prior library. This is bounded numerical/default-path evidence, not broad certification of the experimental renderer.

## Three-seed decision

`summary.json` uses the same 320x180 exported scene, CPU1, 144 fixed samples, seeds 17/53/97 and the native independent 4096-sample reference. Blur, clamps, denoising and adaptive sampling were disabled. Reference noise remains. Other workstation activity makes these quality screens ineligible for an uncontended speed claim.

| Screen | Mean relative SSS RMSE | Interseed variance | Mean core CPU seconds |
| --- | ---: | ---: | ---: |
| Existing sampler | .455994 | 2.445995 | 23.2770 |
| Learned mixture, share .25 | .442981 | 2.288405 | 24.8727 |

The pilot separately cost 3.0485 CPU seconds (2.7447 wall seconds). The roughly 6.4% interseed variance reduction did not establish a gain once query overhead and pilot cost were included. No SSS learned-guiding default, artist control or installed wheel was promoted. Do not present the prototype's small fixed-sample error reduction as equal-time acceleration.

## Retained fix: model selection must not depend on radiance units

`PathGuidingCache::FitLeaf` normalized its target for EM but multiplied the model-selection log likelihood by raw histogram magnitude. Its BIC penalty used the signal-record count. Thus changing units of identical unclipped training data changed the balance of likelihood and penalty. In the SSS screen, rescaling the histogram made every learned cell select a uniform model, even though its normalized directional information was unchanged.

The likelihood now uses the signal-record count consistently with the penalty. Target normalization and the physical renderer are unchanged. `LUX_PG_BIC_LEGACY_MASS=1` is a diagnostic negative control.

`dev-tools/path_guiding_scale_test.py` compiles a disposable probe using current Release libraries. It fits identical known anisotropic input at scales 1, .01 and .0001, requires a nonuniform result, and checks the resulting PDFs agree. The old policy is required to fail. The test also checks surface-cosine and isotropic-volume importance-sampling means:

- Maximum relative PDF difference: 2.56e-6; all three fitted peaks: 1.83782.
- Surface mean .999857, estimated standard error .000389.
- Volume mean .997018, estimated standard error .002633.

This establishes the numerical model-selection invariant for unclipped data and these proposal checks. It does not establish a production SSS speedup or an installed-Blender/Metal guiding quality certification. The prior installed SSS wheel is not replaced by this source-only fitter correction.

The final restored source again passed all 112 default-path CPU buffers exactly. The 22 recorded render children were reaped. The installed dependency package was found missing again during the audit; the unchanged selected prior complete wheel was restored and all 192 files verified. Its native identity remains `972eaae229f3d39ab77c4f34602e167fb16495fdcbbd714db552a4d8fcdbaa70` (`final-state.json`, `dependency-restore.json`). The repeated removal cause is not established by this work.

Reference for variance-aware guiding targets: [Rath et al., Variance-Aware Path Guiding](https://graphics.cg.uni-saarland.de/papers/rath-2020-siggraph-guiding-paper.pdf). The density-measure correction above was derived and tested for this pilot; no published speedup is transferred to this renderer or hardware.

## Next high-impact hypothesis: path-coherent channel selection

The current RGB walk repeatedly mixes channels with different extinction/radius. Test selecting one active channel once at the first eligible SSS volume vertex, with positive probability based on current throughput (and optionally local albedo), then keeping its throughput divided by that probability while clearing the other future-throughput channels. For RGB transport linear in throughput, the conditional expected future contribution is unchanged. Already accumulated radiance must remain intact; corresponding future irradiance throughput must use the same projection. Once throughput has one active component, do not select again. Physical coefficients and boundary evaluation stay unchanged.

This is not implemented by this change. First test RGB-only, using an independent reserved random dimension and excluding learned guiding. Validate finite/zero-channel cases, white-furnace and lit-scene means, raw convergence with independent references, and pilot-free matched-time CPU evidence. Spectral support needs a separate audit of hero wavelength/IOR choice, alive masks, accumulated contributions and final packet-to-RGB conversion; an RGB result cannot certify the default spectral path or complete the original goal.
