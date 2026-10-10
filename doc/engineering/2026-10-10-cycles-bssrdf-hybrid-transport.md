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

The full CPU hybrid driver passes CPU50 followed by 19 hybrid conditions in
one final execution. Its maximum raw RGB channel mean error is 2.8503%, inside
the unchanged 3% gate. The sharp omitted-path control loses 36.5% of reference
energy. RGB/spectral color, partial/local radii, PSR, ordinary lighting, legacy
partition, independent seed and reverse-kernel preflight are covered.
Actual M5 Pro Metal55, ordinary PSR3, emission5, directional MNEE7 and light-side
MNEE3 pass with this exact native module. The last two MNEE drivers select the
intended unregularized delta operator; their wider smoke bounds do not establish
3% physical parity. The rough-adjoint22/sharp18 driver passed the preceding
Metropolis-fixed package, not this final binary; exact CI verification remains
required before claiming the full suite for the public wheel.

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

Exact new main CI wheels and rolling publication are the next acceptance step.
This unit does not complete no-edit Cycles compatibility or repair the public
2.11.27 adapter's positive-radius defect. GPU reverse walks, mixed/textured
reverse coefficients, normal/opacity/volume combinations, full estimator MIS,
startup normalization and finite-budget chromatic grain remain in the active
goal. Principled/Skin/Burley/legacy SSS and wider production scenes still require
integration and validation.
