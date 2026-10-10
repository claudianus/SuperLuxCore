# Cycles BSSRDF material partition boundaries, 2026-10-10

The full unchanged-Cycles-scene goal remains active and incomplete. This
extends the [experimental CPU transport](2026-10-10-cycles-bssrdf-transport.md);
the public 2.11.27 adapter and actual user installation still use the earlier
OpenPBR route for positive standalone SSS. No production acceptance is claimed.

## Boundary identity

Blender exports each object's material partitions as separate native scene
objects. A walk bounded only by its entry mesh ignores the other partitions.
The native closed-box diagnostic loses about 30% of its central radiance when
the entry partition is left ungrouped. Frozen entry coefficients must survive
an escape through a different, even black, material partition.

`scene.objects.<name>.subsurfacegroup` gives those partitions an explicit
scattering identity. Empty groups keep mesh-local boundaries. The CPU walk
accepts its own entry mesh or a boundary with the same nonempty group. It
continues to skip unrelated overlapping objects. AOV Object IDs are independent
and may deliberately be identical across unrelated objects and copies.

`Scene::SetObjectSubsurfaceGroup` is available in C++ and Python. It validates
the object, clears the public property cache and marks a geometry edit. Native
`DuplicateObject`, including motion overloads, starts each copy ungrouped.
The addon assigns matching material partitions of each copy a fresh group.
Hair/particle parts appended after mesh conversion retain their own boundaries.
The complete SDK/native wheel must be kept together; no GPU scene struct or
per-BSDF payload field is added by this CPU change.

## Current verification

The complete private wheel's staged native SHA256 is
`26ea4c3aa544a14e99732b52390e1fd593890f544bb45a943e475c2b65032b00`.
Version metadata remains 2.11.27; this is not the released 2.11.27 native.
The package guard covers 266 addon Python files, 459 profile payload entries,
wheel RECORD, cached wheel and critical source files. The actual user profile
is unchanged.

- Native transport: 33 contract conditions, including partition radiance,
  entry AOVs, an unrelated object sharing the AOV ID, setter/serialization/cache,
  missing-object rejection and duplicate isolation. The ungrouped box mean
  ratio is 0.704672; grouped versus whole-object maximum channel mean error is
  0.0000651. These stochastic 64px diagnostics are not image acceptance.
- Blender exporter: three guarded native-API contracts verify material/hair
  separation and distinct groups for static and motion copies with shared IDs.
  Completion requires the result JSON and completion marker; use Blender's
  `--python-exit-code 1`, since its default exit code can hide script failure.
- Ordinary emission: E113's five checks pass on current-build CPU, LIGHTCPU
  and actual Apple M5 Pro METAL_GPU. This does not verify BSSRDF on Metal.
- Seven unchanged-graph 1280x720, 128spp CPU pairs were rendered and their
  comparison images directly reviewed. Material partitions and collection
  instances preserve shading and boundary continuity with mean radiance ratios
  1.004653 and 1.004505. The original rough/aniso, color, partial RGB Radius,
  textured entry and emission coordinate control also ran on this native.
  Small spectral/noise differences remain; no pixel equality or broad
  compatibility percentage is inferred.

Evidence is preserved in workspace
`test-scenes/validation-2026-10-10/cycles-bssrdf-object-groups`.
Native harness: `dev-tools/cycles_bssrdf_transport_test.py`. Addon harnesses:
`cycles-bssrdf-experimental-scene-test.py` and
`cycles-bssrdf-group-export-test.py` under its `dev-tools` directory.

## Remaining full-goal work

Metal needs equivalent nonlocal walk and object-group payloads. Adjoint/LT/BIDIR
densities and MIS, mixed/Add/Principled closures, Skin/Burley/Legacy methods,
physical spectral Radius mapping, tiny positive radii, exit bump/normal ray
context and explicit Volume combinations remain. Motion group serialization is
verified; animated BSSRDF production rendering, GUI and other platform hardware
checks are still required. Private CPU eye-only restrictions are not shipping
quality defaults, and the release adapter has not been switched to this model.
