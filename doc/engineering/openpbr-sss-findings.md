# OpenPBR / SSS debugging findings

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## OpenPBR / SSS debugging findings (e35)

- `GgxSampleVNDF(wo, alphaX, alphaY, u0, u1)` — callers must keep the
  alphas before the random draws. OpenPBR originally passed
  `(wor, u0, u1, alphaT, alphaB)`; the sampler then used the uniforms as
  roughness (~0.5 effective alpha) while eval computed pdfs with the
  intended alpha → ~57% energy loss on every glossy/BTDF vertex.
- `FresnelDielectricModulated` must test the *physical* TIR condition
  (`etaTI < 1 && 1-cosI^2 > etaTI^2`) before the specular_weight
  modulation shortcut — with `specularweight=0` the modulated eta is 1
  and the early return would otherwise report F=0 for directions the
  sampler's TIR fallback reflects at pdf>0 (energy loss).
- OpenPBR interior IOR convention: `hitPoint->interiorIorTexIndex`
  (GPU) / `GetInteriorVolume()->GetVolumeIOR()` (CPU) is ray-relative.
  When the material has no interior volume the fallback must be
  `specular_ior`, not 1 — otherwise eta=1 collapses the transmission
  half-vector and BTDF eval returns 0.
- eta = n(wi side)/n(wo side) with `wo.z>0` = entering. For internal
  rays (`wo.z<0`) nWo comes from the interior volume, nWi is the
  exterior — swapping them inverts refraction (~80% loss). On TIR the
  sampler must reflect off the microfacet (the direction is scored by
  the specular lobe's eval/pdf), never kill the path.
- Eval functions that take TIR-reflected directions must canonicalize
  `wor`/`wir` into the +z hemisphere before computing `wh = w_o + w_i`
  — raw internal directions give wh.z<0 and early-out on backfacing.
- Walter BTDF f*cosI has NO `|wi.z|` factor (see roughglass.cpp); an
  extra `|wi.z|` attenuates every transmitted path by ~0.6.
- Albedo-parametrized SSS volumes already reproduce `subsurface_color`
  as diffuse reflectance — the OpenPBR interface tint must be white
  (`sssAlbedoMedium`), else the color is applied twice. CPU reads it
  via `GetInteriorVolume()` + `IsSSSParametrized()`; GPU mirrors it via
  `mats[material->interiorVolumeIndex].volume.homogenous.sssAlbedoTexIndex`.
- `hitPoint.passThroughEvent` feeds THREE consumers (surface lobe pick,
  stochastic transparency, volume pick). `Scene::Intersect` must draw a
  FRESH random for the volume free-flight sample — reusing passThrough
  correlates the escape decision with the boundary lobe pick (~8%
  energy loss on a matched glass shell, +48% on interior NEE before the
  MIS fix). GPU mirrors this via `Rnd_InitFloat(passThrough, &volSeed)`.
- `scene.objects.X.transformation` takes 16 values in COLUMN-major
  order (Property::Get<Matrix4x4> reads v0,v4,v8,v12 as row 0). A
  row-major translation string silently lands in the projective row.
- Ninja multi-config: `ninja -C out/build pysuperluxcore` builds Debug only;
  the Release module needs `-f build-Release.ninja pysuperluxcore`.

