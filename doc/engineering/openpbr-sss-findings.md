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

## CB'15 hybrid SSS remap (e48)

- The CB15 diffuse-reflectance formula `Rd(α,η)` is correct in itself but
  WRONG as an absolute albedo target at matched boundaries: at η=1 it
  overestimates Monte-Carlo transport reflectance (~0.87 vs measured 0.83
  for α=0.8). vdH/d'Eon fits exist precisely because they fit measured
  reflectance at matched boundaries.
- Correct design = **hybrid**: baseline `Rd` from vdH (exact at η=1),
  CB15 used only as a ratio `K(α,η) = Rd_cb15(α,η)/Rd_cb15(α,1)` to model
  internal-Fresnel trapping loss at η>1. Inverted numerically so the
  albedo parameter is exactly what the artist picks.
- Residual error is a diffusion-ratio limit: at low albedo + η=1.4 the
  hybrid underestimates trapping correction (~19% at α=0.3 vs vdH's ~43%).
- Gotcha: there are TWO homogeneous-volume GPU paths —
  `materialdefs_funcs_homogeneousvol.cl` (volume material) AND the shared
  material-volume path. Mirroring the remap in only one silently desyncs
  GPU (observed: openpbr sss GPU rendered vdH, CPU cb15, ~35% off).
- OpenPBR `subsurface_radius` is the CB15 diffusion-radius convention, so
  the implicit interior volume maps 1:1 to `sssprofile=cb15`.

## Measured-media presets (e53)

- `subsurfacepreset` is resolved at parse time into the *defaults* of
  `subsurfacecolor`/`subsurfaceradius`/`subsurfaceradiusscale` — the
  material itself is unchanged, so serialization, GPU upload and the
  implicit-volume path needed zero new plumbing. Explicit properties
  always win over preset defaults.
- Table generation (`dev-tools/gen_sss_presets.py`): α = σs'/σt',
  apparent albedo = invertible vdH/CB15-hybrid Rd(α,η) at the medium's
  canonical IOR (1.35 milk-like / 1.4 skin / 1.5 marble), radius = mfp
  of the strongest channel so `radiusscale` max component is exactly 1.
- `wantSSSVol` previously required an explicit `subsurfaceradius` —
  with a preset the property may be absent, so the gate now also fires
  on `sssPreset` (still ANDed with `subsurfaceweight` being defined).
- **Found bug (unrelated to SSS)**: `OpenPBRMaterial::ToProperties()`
  and `AddReferencedTextures()` dereferenced `SellmeierB`/`SellmeierC`
  unconditionally → `Scene.ToProperties()` segfaulted on EVERY openpbr
  material without a sellmeier ior set. Disney material already guards
  the same optional pair; openpbr now matches that pattern.

## Skin recipe sanity numbers (meters, human head ~20 cm)

- `subsurfacepreset = skin_light`/`skin_dark`, `subsurfaceweight = 1`
  (skin's diffuse look IS the SSS — no separate diffuse lobe needed),
  specular IOR 1.4, roughness ~0.4.
- Translucency through ears/nostrils needs REAL thickness — the
  implicit volume is a true random walk, not a screen-space diffusion
  blur.
- `gen_sss_presets.py` keeps engine `parsematerials.cpp` and the
  adapter's `SSS_PRESETS` dict in sync — regenerate, don't hand-edit.

