# Experimental native Cycles BSSRDF transport, 2026-10-10

The full unchanged-Cycles-scene goal remains active and incomplete. Public
2.11.27 still exports positive standalone SSS through OpenPBR and its implicit
bulk volume; its documented near-black failure is not fixed in that release.
This implementation is an opt-in CPU diagnostic on main, not a shipping
compatibility claim. Native OpenPBR, general volumes and quality defaults are
unchanged.

## Transport implementation

`cyclesbssrdf` is a separate native material with Color (`kd`), Radius, Scale,
IOR, Roughness, Anisotropy and the normal/bump input. It freezes the inputs at
the entry. A visible-GGX refraction sample uses standalone Cycles' direct
roughness alpha and clamped IOR. The object-local random walk uses Van de Hulst
coefficients, compensated low albedo, HG phase sampling, a balance mixture of
channel distance PDFs and compensated roulette. It does not retint the result
at every bulk-volume segment. The exit uses a unit-white diffuse closure.

The eye path records the entry's albedo, opacity, depth, position, normal,
material/object IDs, UV, motion and own emission before the scattering vertex
moves. Absorption terminates the path rather than becoming an environment miss.
The exactly black channel preserves Cycles' safe-divide zero endpoint; the
earlier private prototype's small black-color leak was caught by a white
environment fixture and repaired before the final tests.

All-local Radius*Scale uses the original colored Matte closure. RGB partial
local channels use a probability-compensated local/nonlocal closure selection,
with nonlocal parameters evaluated once at entry. A generated-coordinate Scale
that is zero on the visible face exercises the dynamic RGB local limit.

The target contract comes from Blender 5.2.1 LTS build
`9e2066aef7ef7e20c142ad7bd3303138a4304c93`. The local 5.2 source checkout is
`d13f752e3b9c4f8c261cda552b1021f8bcc0382c`; the five BSSRDF files inspected in
the source-contract evidence are identical at those revisions. Source and
provenance snapshots are under the previous deployment27 validation folder.
This preserves the imported closure's meaning without asserting pixel equality.

## Explicit experimental boundary

RenderConfig requires `path.cyclesbssrdf.experimental.enable=true`, `PATHCPU`,
and explicitly disabled hybrid/light tracing, PhotonGI and ReSTIR GI/PT. Other
engines, nested mix/coating subtrees and nonlocal GPU compilation reject the
request. Spectral partial channels and dynamic spectral Radius/Scale reject
before render workers start, with material context. They do not turn into a
local/white/Clay fallback. Existing material enum integers are preserved by
appending the experimental type; the BSDF override adds no per-BSDF data field.

The following remain required for the full goal: nonlocal Metal transport;
adjoint/light/BIDIR transport and forward/reverse nonlocal densities; mixed and
additive closures; Principled/Skin/Burley/Legacy methods; physical spectral
radius mapping (including unbounded units and partial channels); very small
positive radii versus geometric precision; grouping Blender material partitions
into one scattering object; and exit bump/normal evaluation with complete ray
context. Explicit-volume combinations and broader animated/production scenes
are unverified. OSL and baking retain their previously deferred scope.

## Current validation

The final private package keeps version metadata 2.11.27, so its native hash is
the identity, not a release version. Staged native SHA256 is
`cb99bcb66d07c0c49e01e442b0eec518846e2602c6cd66937b117b4ba3cc21f6`.
The installer updates the entire package, matching dylibs, translator,
dist-info and cached wheel. Identity checks cover 266 runtime addon Python
files, 459 profile payload files and the critical native/test source files.
The actual user installation remains the verified public27 payload.

The development repacker regenerates wheel RECORD hashes and lengths from the
final archived bytes and drops obsolete RECORD signatures. Replacing the native
binary while retaining the public wheel's old inventory was inconsistent.
The final payload/inventory check covers every wheel entry; the native bytes
are unchanged by this packaging repair.

The native transport test passed 26 contract conditions: explicit unsupported
transport rejection, nested two-sided mix rejection, unused-material behavior,
property roundtrip, spectral preflight, black absorption, entry AOV/albedo,
foreign interior geometry, zero Radius, dynamic RGB zero Scale and partial RGB
channel weights. These small native fixtures are not visual acceptance.
E113's five ordinary-emission checks passed on the current build, including
PATHCPU, LIGHTCPU and actual Apple M5 Pro `METAL_GPU` PATHOCL.

Private Blender pairs use unchanged original Cycles graphs at 1280x720, 128
samples, no denoising, CPU, spectral except the partial-Radius RGB case. The
rough-anisotropic, colored, partial-Radius and textured-entry diagnostics have
native/Cycles mean ratios around 1.012, 1.025, 1.001 and 1.025. These are scoped
measurements, not percentages of project compatibility. A separate generated
checker emission pair isolates coordinates from scattering. Review includes
saved EXR pixels and independently generated comparison images; neither a
mean ratio nor a preview alone is acceptance. The source hashes, exact metrics,
images and review status are stored with the validation evidence.

Evidence: workspace
`test-scenes/validation-2026-10-10/cycles-bssrdf-cpu-transport`.
Native harness: `dev-tools/cycles_bssrdf_transport_test.py`.
Blender harnesses in the sibling addon checkout:
`dev-tools/cycles-bssrdf-experimental-test.py` and
`dev-tools/cycles-bssrdf-experimental-scene-test.py`.
