# Light linking

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Light linking (scene.{objects,lights}.X.linkgroups)

Receiver-based direct-illumination linking (Cycles semantics):
`scene.lights.X.linkgroups = "a,b"` puts the light in groups a,b
(`LightSource::linkMask`, 0 = global); `scene.objects.X.linkgroups` +
`.linkmode = include|exclude` sets the object's accept mask
(`SceneObject::linkAcceptMask`). A light contributes to a vertex iff
`light->IsLinkedTo(bsdf.GetLinkAcceptMask())` (mask intersect, or
light global). Names map to bits via `Scene::ParseLinkGroupMask` —
insertion order = bit order, and `Scene::ToProperties` re-emits names
in the same order so masks round-trip through .bcf.

- Filtered: NEE (`DirectLightSampling`), BSDF-sampled direct emitter
  hits (`DirectHitFiniteLight`/`DirectHitInfiniteLight` — must stay
  consistent with NEE or MIS weights leak light), and the FIRST
  surface vertex of light tracing (`RenderLightSample`,
  `BiDirCPURenderThread::TraceLightPath`, GPU light kernel).
- NOT filtered: indirect bounces (depth>=1) — a linked light still
  propagates through GI; objects receive it indirectly.
- `EyePathInfo::linkAcceptMask` carries the receiver mask to direct-hit
  tests; updated in `EyePathInfo::AddVertex` (CPU+GPU twins).
- Emissive mesh triangle lights inherit the owning object's link
  groups (`sceneobjectdefs.cpp`); env/infinite lights take
  `scene.lights.X.linkgroups` too.
- 64-bit masks use `u_longlong` host-side (NOT `u_int64_t` — POSIX
  only) and `ulong`/`~0ull` kernel-side (`~0ul` is 32-bit on Windows).
- Regression: `dev-tools/e41_lightlink_test.py` (8/8: include/exclude,
  global, mesh emitter, env, multi-group, CPU/GPU parity, roundtrip).

