# Experimental Cycles BSSRDF CPU hybrid transport

The opt-in CPU hybrid path classifies a sharp imported SSS camera connection
using its external path: `L S+ SSS` is `L S+ D`. The inverse entry used
internally by the adjoint walk is not an additional specular vertex. Previously
these internal camera splats were always noncaustic, so the hybrid light sampler
discarded them while the eye sampler omitted the corresponding caustic paths.

`PATHCPU` requires both existing experimental BSSRDF and adjoint opt-ins. Both
explicit hybrid enablement and canonical light-tracing promotion validate the
reverse kernel before creating workers. Device hybrid, BIDIR, vertex
connections, photon caches, mixed reverse closures and textured reverse
coefficients remain gated. The production Cycles adapter is unchanged.

## Metropolis rejection bias

The initial rough fixture failed: raw interior mean 0.6597 against an independent
unsuppressed eye mean of 0.7777 (15.2% low). Higher budgets did not repair it.
Independent Sobol/Random light tracing produced means near 0.78, while full
Metropolis produced about 0.52. Disabling light focus did not fix the defect.
Increasing the old 512 rejection limit restored the full Metropolis mean to
about 0.80.

CPU and device samplers previously forced acceptance after that limit,
including zero-contribution proposals. This changes the stationary distribution
and loses dwell weight on sparse productive paths. Both now use the luminance
acceptance ratio whenever the current contribution is positive. The legacy
`sampler.metropolis.maxconsecutivereject` property and device field remain
serializable, but never force acceptance. Rejection counters saturate rather
than wrapping to the device accepted-state marker. Stop checks and default
large-mutation probabilities remain intact.

The actual Release-library contract enumerates the complete 24-bit uniform
primary-sample grid for an independent normalization constant. It tests sparse
discontinuous support and excludes a stale reserved SampleResult slot. The old
512 limit loses 78.4% under this controlled normalization; limit1 loses 99.86%.
Both corrected limits have 0.0464% error. A separate fixed-b diagnostic has
0.375% error at the default 0.4 large-step rate. Its temporary executable is
removed on exit; this local C++ driver requires the macOS Release library.

An earlier combined startup-normalizer diagnostic still failed after the fix:
mean 2.85 times the analytically known mean. That failure is preserved, excluded
from acceptance evidence, and remains a separate normalization/warmup issue.
The controlled contract does not certify startup normalization or arbitrary
sparse MLT convergence.

## PSR partition and device mirror sampling

At explicit PSR sigma0.03, the sharp SSS hybrid was 37.1% brighter than its
unsuppressed eye reference. Light-path classification already used the source
material's delta flag. The eye path instead saw PSR's widened GLOSSY event and
kept a path that the light sampler also included. Eye receiver/terminal checks
now retain static material delta classification, with matching CPU/device
state. PSR transport and production defaults remain intact.

An ordinary matte/side-mirror control demonstrates the same partition without
experimental SSS: the suppressed eye loses 34.9% of the reference energy.
An earlier mirror-behind-cube fixture had no supported reflection path and was
rejected as a vacuous fixture. The final driver requires a meaningful omission.

The meaningful Metal control then exposed a separate mirror PSR bug. After
pushing its regularized sample, MirrorMaterial_Sample fell through into the
sharp-delta branch and pushed another stack result. Returning from the PSR
branch fixes actual device transport. The failing control had 9.0% hybrid/eye
error. The final CPU and Metal wavefront off/on errors are 0.302%, 0.873% and
0.673%, inside the unchanged 3% gate.

## Verified local package

The final whole wheel, extracted payload, cached wheel and 266 runtime addon
Python files were verified. Loaded native SHA-256:
`f1463b91e02e77ed0836077836c18d3ec2bb55f87b58729e44fcd0436925e9d2`.

The original local hybrid fixture passes CPU50 followed by 19 conditions in
one complete execution. Its maximum raw RGB channel mean error is 2.8503%, inside
the unchanged 3% gate. The sharp omitted-path control loses 36.5% of reference
energy. RGB/spectral color, partial/local radii, PSR, ordinary lighting, legacy
partition, independent seed and reverse-kernel preflight are covered.
Actual M5 Pro Metal55, ordinary PSR3, emission5, directional MNEE7 and light-side
MNEE3 pass with this exact native module. The last two MNEE drivers select the
intended unregularized delta operator; their wider smoke bounds do not establish
3% physical parity. The local rough-adjoint22/sharp18 run used the preceding
Metropolis-fixed package. The final exact CI run below separately verifies that
full suite for the public wheel.

## Actual Blender images and remaining work

The private harness adds CPUHYBRID and a closed Glass slab over sharp/rough
SSS at 1280x720. Materials, world, lights, object transforms and original Cycles
settings are fingerprinted across engines. Budgets are recorded separately:
Cycles128, CPU/Metal eye128, hybrid eye128/light512 (its partition implies more
than128 actual eye samples). No throughput comparison is made.

The initial hybrid's whole-image means hid the body error because the white
background dominates. The predefined sphere-interior circle (radius240, 75%
of the authored projected radius) finds 36.4%/38.0% sharp/rough error versus an
independent CPU eye reference. After the PSR correction the same region and 3%
gate give 0.661%/1.030%. These CPU hybrid images use the preceding PSR-fixed
native module, not the final device-mirror revision. Raw EXRs, rejected outputs,
source snapshots and exact package identities are retained.

All four PSR-fixed sharp/rough Cycles/hybrid PNGs were directly reviewed at
original resolution. Shape, orientation, warm material hue and scattering
meaning agree; stronger chromatic grain remains, particularly on rough SSS.
This is not production convergence acceptance. Independent regional means
alone are not visual acceptance. No quality defaults were reduced.

Exact main CI wheels, rolling publication and the common-core installed runtime
are verified below. Expanded hybrid-fixture acceptance is complete: CPU50/hybrid23, maximum
raw-channel mean error1.7592%, within the unchanged3% gate.
This unit does not complete no-edit Cycles compatibility or repair the public
2.11.27 adapter's positive-radius defect. GPU reverse walks, mixed/textured
reverse coefficients, normal/opacity/volume combinations, full estimator MIS,
startup normalization and finite-budget chromatic grain remain in the active
goal. Principled/Skin/Burley/legacy SSS and wider production scenes still require
integration and validation.

## Exact CI wheel and common-core deployment, 2026-10-11

Native code commit `ce7195b66ddf828353f13186b44e4086cff5417c` is published in
[wheels-latest](https://github.com/claudianus/SuperLuxCore/releases/tag/wheels-latest).
[The exact workflow](https://github.com/claudianus/SuperLuxCore/actions/runs/38062029454)
passes all four platform wheels, CUDA NVRTC, attestations and rolling publication.
All four public asset digests match the verified signed subjects, source commit
and invocation. The complete ARM wheel RECORD, extracted payload and cache were
verified before execution. ARM wheel SHA-256:
`814210cedebff3cbd8a9d68127de8b234685624aaf1461ca48ac0b18d98921e6`;
loaded native SHA-256:
`aba55215c18290773b2772f75bdb5b78dee906de5d66b9675d9822c24b815a6a`.

The full sharp driver passes CPU50, rough22 and sharp18 with this exact wheel;
maximum sharp raw-channel mean error is 0.7166%. Actual Metal55, ordinary PSR3,
emission5, directional MNEE7 and wider light-side MNEE3 also pass. CI ordinary
PSR's maximum error is 1.1414%; the actual installed copy's maximum is 1.2796%.
These existing stochastic mean gates remain unchanged.

The first low-budget CI hybrid execution fails spectral blue at 3.1359%. A
second fixture revision that increased only spectral work fails rough PSR at
3.0113%. Neither is accepted or hidden. A predeclared spectral diagnostic uses
four times the eye/light work and three independent seeds, with the original
region and 3% per-channel gate: errors 0.5965%, 0.4764% and 0.8172%. The permanent
driver now declares eye65536/light81920 for every paired condition and three
independent seeds for both spectral and rough-PSR fixtures. This test-only
change does not alter rendering defaults or the native binary. Its final full
CPU50/hybrid23 execution passes, maximum raw-channel mean error1.7592%. Original
failed outputs and fixture revisions remain separate from final acceptance.

Six 1280x720 Cycles/native comparison sets use the exact CI wheel. All 12 PNGs
were directly reviewed. CPU and Metal eye shape, orientation and warm scattering
hue agree with Cycles. Hybrid retains substantially stronger chromatic grain,
especially rough SSS; production convergence is not accepted. In the predefined
body-interior region, hybrid sharp/rough differences from independent CPU eye
are 0.5103%/1.2076%, and Metal eye differences are 0.3216%/0.1043%. Original graphs
are unchanged. The white background is excluded from this mean gate.

The common-core wheel is also deployed to the actual Blender5.2 profile. Its
whole installed payload and cached wheel match the signed CI wheel. Version
metadata remains2.11.27: this is the new rolling core, not a replacement of the
older stable2.11.27 artist ZIP. The existing 266 production-adapter Python files
and user settings are preserved; Cycles reader SHA-256 remains
`7dd35540ac9a1fcffdbd4b1aeb2684957f51164e799a30b93cc6d271fcb6a235`.
The previous public ZIP retains the exact rollback wheel. Production preview
and actual installed CPU/Metal default spectral128spp720p images (four PNGs
total) were directly reviewed: procedural diffuse colors, metal highlights,
glass, silhouettes and floor shadows remain consistent; noise remains. Fresh
processes load the exact new native fingerprint. GUI hot reload is not assumed.
The installed public adapter's positive-radius SSS defect remains open.

A longer actual-Release CPU startup diagnostic keeps the analytic mean
0.000478327. At32million iterations its cumulative mean0.000592453 is still
23.86% high, while the last4million window0.000483821 is only1.15% high and the
uniform normalizer0.000485302 is about1.46% high. Warmup remains active. This
shows that early startup contributions persist in the accumulated result even
when later estimates approach the target. It is failed/scoped diagnostic
evidence, not a completed startup fix or a production convergence result.

Evidence is preserved under
`test-scenes/validation-2026-10-11/cycles-bssrdf-hybrid-transport/`, with complete
wheels, source-frozen builders, raw outputs, original PNG/EXR images, failed
fixtures, signed publication metadata and installed-runtime proof. Temporary
executables and completed profiles are removed; full compatibility stays active.
