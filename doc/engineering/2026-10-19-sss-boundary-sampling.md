# CPU SSS microfacet boundary sampling

This bounded CPU improvement preserves the physical OpenPBR BSDF and volume
transport. It changes the conditional reflection/refraction proposal at an SSS
boundary, and corrects an endpoint error in the legacy TIR density on CPU/Metal.
It does not establish Cycles convergence parity or production quality.

The old sampler selected the dielectric reflection or transmission lobe using
macro-normal Fresnel before sampling the GGX visible microfacet. On an interior
interface the actual microfacet Fresnel can differ substantially, especially
near the critical angle. The new sampler retains the total mass of the two
adjacent dielectric lobes, samples their shared VNDF once, and then selects the
branch using the sampled microfacet Fresnel, lobe tint and radiance eta factor.
The PDF recomputes this conditional branch mass from the half-vector, multiplies
it by the correct reflection/refraction Jacobian, and uses it in both directions.
The physical lobe evaluations, GGX masking, multiple-scatter compensation,
absorption, phase function, spectral default, depth and roulette remain intact.
Positive proposal probability floors preserve support; they do not clamp radiance.

The proposal currently applies to CPU SSS boundaries with a shared lobe frame,
positive specular weight and no thin film. Independent lobe normals, thin film,
index-matched delta interfaces and other materials retain their existing sampler.
`SUPERLUXCORE_SSS_LEGACY_BOUNDARY=1` restores the old branch proposal for diagnostics.
The Metal sampler retains its existing proposal. Equal physical laws do not
require matching random sequences or identical noise between CPU and Metal.

The legacy shared-frame evaluation also used `EvalBtdf(eye, light)` to add a TIR
fallback density even on a light subpath. This used the wrong conditional VNDF
G1. It now calls the per-lobe PDF with the actual fixed/sampled endpoints on both
CPU and Metal. This is a PDF correction, not a change to the scattering law.
The initial 65,536-sample probe exposed a maximum reverse-density mismatch of
0.7583 and inconsistent light-subpath means; that failed receipt is retained.
After the correction, 48 configurations (roughness .05/.4/1, both sides, four
angles and eye/light transport), each with 1,048,576 IID samples per mode, had
zero sampler/evaluation and reverse-density discrepancy. Fixed-grid physical
BSDF sums were exactly equal and the maximum difference of sample means was
1.80 combined estimated standard errors. This finite statistical check supports
the analytic PDF derivation; it is not an exhaustive proof of all materials.

Evidence is in `../test-scenes/validation-2026-10-19/sss-boundary`. The matched
fixture uses stock Blender 5.2.2, CPU four threads, 320x180, fixed complete budgets,
raw RGB/EXR, no denoiser, adaptive sampling or radiance clamp. Native refractive
bulk transport and Cycles' nonlocal BSSRDF differ; use each engine's independent
4096-SPP reference for convergence. The existing reference itself contains noise.
Render operator wall includes session preparation and EXR writing and excludes
host process startup. Shared-workstation and single-fixture limits apply.

The standalone `dev-tools/sss_boundary_probe.cpp` calls the real public material
sampler and evaluator; it records moments, branch mass, forward/reverse density
errors and a deterministic physical evaluation grid. Its build recipe and
library fingerprints are preserved with the evidence. Temporary binaries,
module overlays and isolated profiles must be removed after verification;
complete delivery and rollback wheels and numerical/image evidence are retained.

## Measured delivery

RGB 144 SPP across seeds 17/53/97: legacy mean wall 8.7085s, coupled mean
8.7004s. SSS sample variance fell from 3.21632 to 2.44599 (23.95% lower),
corresponding to 12.79% lower empirical standard deviation. Bulk variance
fell 22.5%; thin-region variance fell 27.5%. SSS relative RMSE against the
native finite reference changed .5345/.5455/.4898 to .4607/.4676/.4559.
Cycles 400 SPP took 8.91/8.96/10.08s and remains visibly much cleaner. This is
an incremental improvement; the overall Cycles/Octane quality target is open.

Spectral 144 SPP sample variance fell from 3.65680 to 3.20278 (12.42% lower,
6.41% lower empirical standard deviation). The initial three-seed mean wall
was 9.927s legacy versus 10.336s coupled; the first coupled leg took 11.158s.
A predeclared follow-up on/legacy/legacy/on ABBA at the same seed/budget is
recorded in `spectral-abba-summary.json`; do not silently discard the first
slower leg or present all spectral runs as perfectly equal time.

The local Blender checkout `/Users/modumaru/Desktop/code/blender-5.2` is commit
`d13f752e3b9c4f8c261cda552b1021f8bcc0382c`, matching the installed runtime.
`subsurface_random_walk.h` uses shared maximum-albedo guiding, an initially
50% classic mixture and color-channel balance MIS. After nine bounces it
changes its mixture and applies an isotropic similarity transform. Its local
object traversal and diffuse exit (`subsurface.h`) are different transport
contracts from the native global, refractive, nested-medium walk. The native
model retains its actual phase and boundary scattering; the similarity
transform, low-throughput cutoff, low-albedo remap and diffuse exit are not
adopted here. A diagnostic port of the shared-albedo/initial-mixture/parallel-
plane probability subset took 13.624s with relative RMSE .4923, versus 8.711s
and .4607 for the current native sampler. Shared .5/optical-distance and
per-channel .5 variants also failed to improve this pilot. These are screening
results for one seed, not a full Cycles algorithm port or a general rejection
of those techniques. The failed proposal was removed from product source;
its patch and images remain evidence.

Delivery uses the measured immutable native SHA256
`a31337eda7669e146de4b02b886c1ff707d16195d936b634e734850e8202dcc7`
in a complete wheel SHA256
`b4495c79934b0eaea5d88f38a9dfbe73f4910c6865367f7bf7592026e78f7e7f`.
Owned material sources and all 181 kernel source fingerprints were read back;
the only kernel difference from the previous delivery is the TIR PDF fix.
The source/build is conditioned on the existing shared working tree and is
not a clean-release certification. A later re-link changed the raw binary
identity; that unmeasured payload did not replace the measured delivery.
CPU 13 meaningful cases and Metal four energy cases completed at exact
256 SPP. The original CPU harness silently accepted an invalid `internal`
alias; that record is excluded, the proper `interior` case rerun, and unknown
case names now fail. The first restricted Metal device query failed with
CL_INVALID_VALUE; the device-capable retry passed. Cancellation took .0231s
and recovery images matched exactly. The actual installed stock-Blender F12
path completed spectral 144 SPP in 10.082s, with engine RGB equal to its raw
EXR. All 191 wheel payload hashes and 393 installed adapter Python files
were read back; adapter Python sources were preserved. The separate isolated
and real-profile smoke images are not bit-identical and are not used as an
identity/performance comparison. No new interactive viewport acceptance is
claimed. No push or public release is implied.

A later readback found Blender's shared dependency package missing after further
comparison activity. A follow-up real-profile render completed its EXR, but its
final native-file hash read failed because the file disappeared during the
render; another Blender process was observed concurrently. The exact deleting
process was not instrumented, and this is not claimed as a diagnosed renderer
fault or a fixed general extension-lifecycle problem. That failed receipt is
retained. The complete immutable wheel is restored after other Blender jobs
exit, followed by two payload/source/settings readbacks without launching more
Blender jobs. Initial successful installed rendering and subsequent failed
persistence checks are distinct evidence. Future unrelated jobs that reconcile
the same Blender dependency directory can change installation state again.
