# CPU SSS interior loop: physical contract and evidence

The default `path.sss.fastloop.enable = 1` specializes consecutive eye-path
SSS vertices without replacing the refractive bulk transport model. The
phase function, physical absorption/scattering, spectral packet, global
intersections, nested-medium routing, existing depth limits, roulette
compensation, emitter MIS, LPE state and radiance/AOV accumulation remain.
No diffusion closure, deep-walk similarity change, denoising, clamping,
local-object-only traversal or new path truncation is used.

## Execution

`PathTracer::RenderSSSInterior` processes non-emitting homogeneous SSS until
a real boundary, other medium or unsupported feature returns control to the
generic eye loop. Cache/guiding/ReSTIR/portal/vertex-connection integrations
retain their generic path. Boundary BSDF shading is deferred when only a
volume collision is needed; material overrides and bevel geometry retain
full resolution. All intersections still traverse the whole scene, so an
embedded object/light is never omitted.

A stack-owned coefficient scope reuses exact results only for recognized
spatially constant inputs (constants and bounded constant Scale graphs).
The key includes volume identity and actual wavelength/hero/alive-mask/
upsampling context. Spectral pauses and changed contexts miss the scope;
it ends with the current walk. Procedural inputs are evaluated normally.
This is neither a radiance cache nor an interpolated material approximation.

For an opaque OpenPBR shadow exit, ordinary blocked visibility can be
resolved without preparing the full shading frame. MNEE keeps its full
shadow BSDF, because the solver consumes that boundary even when ordinary
visibility is blocked.

## Zero-contribution connections

A scene prepass can certify a closed opaque SSS domain containing no NEE
source. It requires one connected, consistently oriented closed triangle
shell per object, positive signed volume and finite nondegenerate geometry.
Exact position welding joins duplicated UV/normal seams; it never closes a
crack. Motion meshes, bevels, hit-dependent volume overrides, transparency,
unsupported finite light types and lights whose bounds overlap the domain
fail closed. World-volume references also fail closed. Triangle-light
bounds and point positions must be outside the domain bounds; infinite
lights are exterior. A query also requires a valid entry axis and a vertex
inside the certified bounds. Internal lights keep full NEE.

Only provably blocked straight connections are omitted. MNEE keeps its
connection strategies. The prepass does not approximate attenuation or
store illumination. Certificates are rebuilt on scene preprocessing/edits;
mesh topology scratch is released immediately. Limits are 50,000 triangles
and150,000 vertices per mesh and500,000 topology triangles per scene;
exceeding a limit selects ordinary processing, without changing quality.

## Sampling

The physical phase and free-flight weights retain the actual nine-component
marginal/conditional proposal density. The classic share remains10 percent;
transport channel probabilities use throughput times single-scatter albedo.
The entry/opposite tangent mixture is balanced by optical position rather
than a fixed99-percent preference. Known thin spans may use this proposal;
unknown spans, nonzero anisotropy and emitting media retain classic sampling.
The CPU-only change does not alter Metal's physical coefficients or proposal.

A shared maximum-albedo guide and refreshing the tangent after every
internal reflection were screened and rejected: their extra walk cost or
thin-part error did not justify adoption. Diagnostic legacy controls remain
for reproducible execution-only comparisons.

## Study boundary

Stock Blender5.2.2LTS `d13f752e3b9c`, Apple M5Pro, four CPU threads,320x180.
Same authored geometry, optical inputs, lights and BOX-equivalent filtering;
fixed complete budgets, raw EXR, denoising/clamping/adaptive off. RGB compares
against Cycles; spectral is retained and measured separately. Cycles uses a
nonlocal BSSRDF, native OpenPBR uses refractive bulk transport. Each renderer
uses its own finite4096-SPP reference from the preceding study. The native
legacy reference requested181 but used actual sequence131 before the seed
repair, and its earlier source fingerprint is disclosed. These are finite,
source-conditioned references rather than exact physical truth.

Evidence is in workspace `test-scenes/validation-2026-10-19/sss-loop`, including
plans, raw images, source snapshots, failures, the numerical comparison,
installed-wheel identity and final machine-readable report. This is a local
developer delivery incorporating pre-existing core WIP, not a clean release
or a claim that the Cycles/Octane convergence target is achieved.

## Final measured result

Execution-only ABBA uses the same final binary and the legacy guide at100SPP:
generic9.945s mean, dedicated6.247s mean (37.2 percent less render-operator
wall). This includes exact coefficient reuse and certified zero-NEE elision,
not just loop syntax. With current sampling, previous81SPP and new144SPP
ABBA average8.708s and8.696s at seed17. These are one-fixture, shared-host
observations, not a general production speed ratio. Each leg was admitted
without another Blender process, and owned builds/tests did not overlap.

RGB seeds17/53/97 reduce SSS finite-reference RMSE by16.2/17.9/22.3 percent.
Empirical interseed SSS variance falls4.9203→3.2162 (34.6 percent), standard
deviation19.2 percent. Three-seed render means are8.277s versus8.708s, so
that quality aggregate has5.2 percent more mean wall time; the matched ABBA
seed17 comparison controls time separately. Cycles400SPP averages8.997s
across these seeds; at seed17 its SSS relative RMSE is3.46 percent versus
native53.4 percent. Native and Cycles references/models remain distinct.

Spectral matched-budget screening uses previous81SPP versus new144SPP:
mean11.163s versus9.935s; empirical SSS variance6.5360→3.6382 (44.3 percent),
standard deviation25.4 percent lower. These three-seed noise estimates are
small-sample diagnostics, not confidence intervals or converged spectral
reference errors.169SPP overshot the time budget at two seeds and remains
recorded as a pilot, not the matched quality set.

Final delivered CPU13cases finish256 samples per pixel. Of104 raw/count/AOV
buffers,102 are bit-identical between generic and dedicated execution.
Nested-medium RGB/irradiance differ only at float32 rounding, max3.58e-7
under an explicit5e-7 absolute bound; this is not labeled bit-exact. MNEE
with a real point light and embedded light has24/24 exact buffers. Actual
PATHCPU hybrid=true also enters the loop and preserves bounded furnace
energy, but progressive counts differ: it is functional evidence only. An
earlier8x8 tiled hybrid attempt was rejected: that engine ignores hybrid
and the furnace threshold was calibrated at16x16. Four Metal cases complete
256SPP; no new GPU speed claim. Native stop/recovery completes in44ms with
bit-identical fresh recovery films.

The installed stock-Blender spectral F12 test uses144SPP at every pixel,
no native/source override, default fast loop, and bit-identical engine RGB
and saved EXR. All393 installed adapter sources are unchanged; complete
package/dylibs/cache/LOCAL source and file-only RECORD are verified after
Blender exit. The final native SHA256 starts272cb6681247; complete wheel
SHA256 startsd619c7bafbe5 in workspace dist/sss-loop-cpu-20261019. The raw
ACES2 images still show substantial grain/fireflies and a large Cycles gap.
