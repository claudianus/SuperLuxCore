# Experimental Cycles BSSRDF device transport

The full unchanged-Cycles-scene goal remains active and incomplete. This
extends the [CPU object-group transport](2026-10-10-cycles-bssrdf-object-groups.md)
with an actual nonlocal device random walk. The public 2.11.27 adapter and
actual user installation still use the earlier OpenPBR route for positive
standalone SSS. No production acceptance or public SSS fix is claimed.

## Device state and estimator

Each free flight uses the existing hardware trace pass and
`MK_RT_NEXT_VERTEX`. A small state record occupies `GPUTask::tmpHitPoint`
only while the walk is in flight; a host static assertion checks its size.
GPU BSDF phase 0 is ordinary/entry, phase 1 is a pending walk and phase 2
is the white diffuse exit. Initialization resets the phase for camera rays,
surface and volume BSDFs. There is no additional kernel argument or large
per-task buffer. GPU embedded struct layouts must be rebuilt together.

Entry Color/Radius/Scale/IOR/Roughness/Anisotropy are frozen. The walk uses
visible GGX refraction, Van de Hulst albedo mapping, weighted channel free
flights, Henyey-Greenstein scattering, distance/escape balance densities and
Russian roulette. Local RGB channels use a compensated local/nonlocal
selection. Metal lacks `log1p`; free-flight inversion uses a corrected
`log(1-u)` expression to retain precision for small random variates.

The compiled scene assigns dense IDs to full nonempty subsurface-group
strings. These IDs are independent of AOV Object IDs. Foreign intersections
advance the ray minimum while retaining the same distance proposal and RNG
stream. Escape through another material partition preserves entry
coefficients and evaluates a unit-white diffuse exit. Absorption splats the
original opaque sample instead of being treated as an environment miss.
Primary position, depth, normals, IDs, alpha and entry albedo remain intact.

## Numerical boundary correction

The dynamic Generated-Y zero-Scale fixture initially rendered a GPU central
mean of 0.430977 instead of local Matte's 0.45. Ray-distance rounding can
put a surface hit just off its triangle plane, turning an exact generated
zero into a tiny positive walk. Generated evaluation now projects the point
onto the actual triangle plane in both CPU and GPU paths, preserving
tangential bump offsets, like the existing Object-coordinate correction.
The same strict radius cutoff is retained. Corrected GPU means are 0.449978
and 0.450024 with wavefront queues off and on.

## Diagnostic gates

PATHOCL requires both `path.cyclesbssrdf.experimental.enable=true` and
`path.cyclesbssrdf.experimental.device.enable=true`. Eye-only transport
restrictions remain explicit. Vertex connection, LT/BIDIR, hybrid, PhotonGI
and ReSTIR GI/PT are unsupported. Entry and grouped exit Bump/Normal,
explicit material volumes and world volumes are rejected. Mixed/Add/coated
closures and partial/dynamic spectral Radius remain rejected.

These private gates do not change shipping quality defaults. Ordinary
materials retain their existing transport. The release adapter is not
switched to this model.

## Current verification

The complete private wheel's staged native SHA256 is
`bf43e75044a86f0137b27110df8d40423cec6c61035fb078547d084e5cfc7cbf`.
Metadata remains 2.11.27; this is not the released 2.11.27 module. Identity
verification covers the complete wheel RECORD, cached wheel, 266 addon
Python files, 459 profile entries and critical source files.

- Current CPU transport: 33 contract conditions. Paired tests use a single
  CPU worker and an explicit seed to remove timing-dependent assignment of
  per-thread RNG streams. The original tolerances remain unchanged.
- Actual Apple M5 Pro METAL_GPU: 17 conditions. Three preflight rejection
  contracts plus seven transport/AOV/radius contracts under each wavefront
  mode. The nonlocal mean is approximately 0.512; local Matte is 0.45.
  Tests use 8192 tasks and 1024spp for the 64px fixture, avoiding AUTO's
  32 in-flight paths per pixel at a short stopping prefix. Production task
  defaults are unchanged. These small contracts are not image acceptance.
- Ordinary emission, current 720p scene pairs and platform CI results are
  recorded separately in the delivery evidence after validation completes.

Harness: `dev-tools/cycles_bssrdf_device_test.py`, including current CPU
regressions from `cycles_bssrdf_transport_test.py`. Private Blender harnesses
accept `SUPERLUXCORE_BSSRDF_DEVICE=CPU` or `METAL` and fingerprint the original
Cycles graphs/settings before and after rendering.

## Remaining full-goal work

Adjoint/LT/BIDIR nonlocal densities and MIS, mixed/Add/Principled closures,
Skin/Burley/Legacy methods, physical spectral Radius mapping, arbitrary tiny
positive radii, exit Normal/Bump and ray context, explicit Volume combinations,
animated and production scenes, GUI workflows and other platform GPU runtime
acceptance remain. A valid Metal eye-only diagnostic is progress toward these
requirements, not completion of full compatibility or authorization to lower
the renderer's shipping quality defaults.
