# CPU bidirectional volume measure, connection state and roulette repair

This repairs concrete transport errors. It does not establish an SSS speedup,
production readiness for a different integrator, or equal-time convergence
against Cycles. The default PATHCPU artist image is unchanged.

## Defects and changes

1. `BSDF::Sample` applied a surface adjoint cosine to volume phases on CPU.
   The normal of a phase vertex is a sampling frame, with no projected area.
   The actual gray, nonabsorbing isotropic light sample averaged .5 instead
   of 1. Volume samples now return unit measure cosine and skip surface
   adjoints, matching `BSDF::Evaluate`. The device public sampler mirrors
   the contract, including exactly tangent directions where its old surface
   correction floor could suppress a valid phase.
2. BIDIRCPU used phase-frame cosines in area-density conversions and its
   MIS recurrence. Volume vertices now use unit cosine; surface geometry
   remains unchanged. `SUPERLUXCORE_BIDIR_LEGACY_VOLUME_MEASURE` is a partial
   geometry-measure negative control; it does not revert the BSDF repair.
3. Environment light emission is represented per projected disk. The light
   path already initialized its emission cosine to 1, but NEE divided by
   the cosine of a virtual enclosing sphere. Both use `BiDirEmissionCosine`
   now. In a one-volume-vertex path, the three strategy weights previously
   failed to sum to one.
4. Visibility rays were offset onto the outgoing side of a surface while
   retaining the incoming volume stack. NEE leaving an SSS body could then
   attenuate the exterior ray as though it were still inside. Camera and
   vertex connections had the same problem. Each connection now updates
   the origin's volume stack on geometric transmission. Vertex connections
   no longer choose the initial medium from the destination normal.
5. Some connection PDFs included roulette factors derived from evaluated
   BSDF values, while recurrence PDFs omitted them. These are not the
   sampler's continuation weights and depend on which strategy generated
   the path. All strategies now use the same roulette-free heuristic
   densities. Actual roulette termination and reciprocal survival factors
   in throughput are retained.

The last change follows the MIS requirement that weights partition unity
and assign zero weight where a strategy has zero support; heuristic
weights need not be the actual sampling densities. See the
[PBRT MIS derivation](https://www.pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency).
Applying this to the recurrence is an implementation inference, checked
by the independent oracle and render controls below. It is not a claim
that every untested path family has been certified.

## Numerical and device evidence

Evidence root from this component: `../test-scenes/validation-2026-10-19/sss-bidir-measure/`.

- `volume_phase_test.py --before <old bsdf.cpp> --out <receipt>` links the
  actual BSDF implementation and a separately compiled old object.
  110592 checks per version cover gray/colored coefficients, HG/Schlick,
  g=-.6/0/.6, three frames and both transport directions. Mean eye/light
  weights are 1/1 after and 1/.5 before; expected-weight error is zero.
- `bidir_measure_test.py --out <receipt>` checks 1048576 same-path strategy
  partitions using the actual environment measure helper. Maximum sum
  error is 1.49012e-7. The old sphere-cosine expression fails 907331 cases
  at the fixed 1e-6 test threshold; its maximum deficit is .452316.
- `volume_phase_device_test.py --out <receipt>` calls the actual public
  `BSDF_Sample` plus the material-op interpreter on real Metal hardware.
  2304 cases/20736 fields agree with CPU; maximum absolute difference is
  1.01328e-6. Exact and near-tangent inputs are included. The restricted
  initial device query failed with CL_INVALID_VALUE; the device-capable
  run and tangent follow-up passed. Scratch binaries/objects and device
  buffers are released.

## Known-solution render oracles

`dev-tools/bidir_furnace.py --module <isolated complete import directory>
--out <new directory> --mode matte|volume|translucent --rr <depth> --spp 65536`
explicitly disables adaptive sampling, PSR, glossy filtering, clamping,
photon caches, hybrid partition and MNEE. The flat fixture is retained for
replay. Requested SPP is not per-pixel exact under SOBOL; actual minimum
and maximum counts and native/source hashes are recorded. The fixed
finite-budget oracle tolerance is .002; this is not an asymptotic proof.

| Fixture | Expected | Measured mean |
| --- | ---: | ---: |
| Flat matte, albedo .8, roulette at 1 | .8 | .79998814 |
| Same, roulette effectively disabled | .8 | .79999673 |
| Nonabsorbing homogeneous sphere, unit environment | 1 | 1.00004688 |
| Conservative diffuse transmission boundary and scattering interior | 1 | 1.00042813 |
| Old roulette MIS, same matte oracle | .8 | .98021384 (expected failure) |

Earlier nonabsorbing-medium diagnostic sweeps varied density, FOV, film
size, path depth, clipping and epsilon before changing the environment
cosine. The pre-fix three-seed sigma2 mean was .99558989. After the cosine
repair, three 262144-SPP seeds averaged 1.00014275 for sigma2 and .99970836
for sigma4. These earlier private diagnostics used their recorded sampler
defaults; the permanent known-solution oracles above explicitly turn
adaptive sampling off.

## Refractive SSS diagnosis and remaining scope

The initial dense SSS furnace remained too dark after phase and environment
repairs. Disabling roulette and removing smooth normals did not remove the
gap; an index-matched boundary largely did. Correcting the outgoing medium
changed the requested2048SPP, no-roulette BIDIR mean .793849 to .824219,
near PATHCPU .826035. This is a bounded 4x4 diagnostic, not a new SSS target
or a matched artist-performance claim.

A further 3-seed SSS diagnostic at radius .085 and requested8192SPP isolated
roulette. Before the consistent heuristic, paired RR3-minus-no-RR means
were .01902335/.01911753/.01898432. After they were
.00014773/-.00043314/-.00128257 (mean -.00052266). These finite private
comparisons used identical recorded sampler settings; they do not certify
all refractive scenes, spectral BDPT, VCM, or an installed Blender build.

The final native SHA256 is
`1216a70cc393b8dd7e38c1ea745335497d3bcb118a42bc19efd9179f3709fd9d`;
raw build SHA256 is
`8cdbb3ee1fea283e6203dd7c2902a4d95e73bf098a2fded692c9d559ab2cbfe5`.
`final-summary.json` records source hashes. The payload was built in the
existing shared working tree; unrelated WIP is preserved and excluded
from this commit. These receipts are conditioned on the recorded sources
and libraries, not a clean-release certification. The 14-case256SPP PATHCPU
regression retains all 112 raw/AOV/count buffers exactly. The v2 artist
RGB144 seed17 raw RGB, sample-count and ray-count buffers are bit exact to
the preceding indexed-sampling version. No experimental integrator was
made Auto, no selected Blender wheel was replaced, and no new equal-time
Cycles result or speed ratio follows from these functional checks.

The corrected bidirectional path still pays substantial light/eye-vertex
connection cost. Its accuracy prerequisites are stronger now; whether it
can improve raw error per second remains open and must be measured before
adoption as an SSS acceleration path.
