# MNEE directional-endpoint validation scenes

Regression assets for the single-vertex manifold next-event-estimation
(MNEE) endpoint extension. `TYPE_SHARPDISTANT` (a delta-direction
emitter, i.e. a collimated sun-like light) can never be hit by forward
BSDF sampling, so MNEE and the plain estimator stay disjoint and no MIS
is required. `TYPE_DISTANT` (finite cone solid angle) is deliberately
excluded: forward refraction can reach the cone, and running both
estimators double-counts the caustic (~19% overcount measured on
`dirsheet_cone10.scn`).

All scenes render at 1280x720. PLY paths are repo-root relative:
run `luxcoreconsole` from the repository root.

NOTE - `scene.objects.*.transformation` is parsed column-major
(`Property::Get<Matrix4x4>` fills `m[i%4][i/4]`): the translation
vector is elements 12..14 of the flat list, NOT 3/7/11. Putting the
translation at index 11 lands in the projective term `m[3][2]`, which
is silently near-identity for planar geometry and was the root cause
of a false "GPU ignores the glass panel" CPU/MetalRT discrepancy
(the panel was coplanar with the floor; the engines resolved the
z-fight differently).

## Scenes

- `dirsheet.scn` - wavy glass sheet over a matte floor, lit by a
  sharpdistant light. The refracted caustic under the sheet is only
  reachable through MNEE (off = dark under the sheet).
- `dirsheet_pt.scn` - same geometry, lit by a far point light at
  irradiance-matched intensity. Ground truth for the directional
  endpoint: the point-light MNEE path was already validated, and the
  sharpdistant result must converge to it (measured: global mean within
  0.2%, caustic region within 0.3%).
- `dirflat.scn` - flat glass panel variant. Single refractive root;
  used to isolate root-selection differences from measure errors.
- `dirsheet_cone10.scn` - finite-cone distant light (theta = 10 deg).
  Documents why cone emitters stay excluded: forward sampling reaches
  them through refraction, so MNEE-on overcounts without MIS.
- `tinycaster.scn` - small glass sphere (r~0.08) over a large matte
  floor under a sharpdistant sun. Needs a TWO-vertex chain (sphere
  entry + exit) ending at a directional endpoint; also the light-tracing
  emission-coverage benchmark (sphere covers ~1e-6 of the emission disc,
  so uniform emission never hits it - see `doc/features/gpu_lighttracing.md`).
  Exercises `e24_mnee_chain_distant_test.py` (CPU/GPU chain parity).

`sphere.ply` note: the mesh originally had vertex normals pointing out
but triangle windings pointing IN (all 9024 faces). The chain discovery
walk derives entering/exiting refraction from the geometric normal, so
the inverted winding manufactured TIR at moderate incidence and the
walk escaped the sphere (~70% discovery failures). All faces were
re-wound outward; keep windings consistent with outward normals.

## Configs

`cpu_*` use PATHCPU, `gpu_*` use PATHOCL (OpenCL and Metal kernels).
`*_on` enables `path.mnee.enable = 1`, `*_off` disables it. `*_pt`
renders the point-light reference scene, `*_flat` the flat panel,
`cone_*` the finite-cone scene, `*_nocache` disables the MNEE seed
cache (`path.mnee.seedcache.enable = 0`) - currently the recommended
setting on high-curvature refractors where flat-frame cached seeds can
steer Newton out of the correct basin.

## Validated CPU/GPU parity (256 spp, PATHCPU vs PATHOCL/MetalRT)

With the corrected transform: identical means and structure.

- dirflat  off: CPU 0.0553 / GPU 0.0553, per-pixel |diff| mean 0.0000
- dirflat  on : CPU 0.2728 / GPU 0.2727, |diff| mean 0.0000
- dirsheet off: CPU 0.3547 / GPU 0.3546, |diff| mean 0.0001
- dirsheet on : CPU 0.4410 / GPU 0.4408, |diff| mean 0.0014
  (max 1.0 on a single saturated caustic pixel = MC noise)
- dirsheet on, seedcache=1: GPU 0.4396 - cache no longer degrades
  convergence (the earlier "checkerboard" was the same coplanar-panel
  artifact, not a cache defect)
