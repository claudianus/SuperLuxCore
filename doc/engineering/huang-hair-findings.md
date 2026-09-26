# Huang hair — porting findings

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Huang hair (e36) findings

- `FresnelDielectricT` (and LuxCore's dielectric Fresnel helpers) return a
  POSITIVE transmitted cosine; Cycles' `fresnel_dielectric` returns a
  SIGNED negative one. Any ported Cycles refraction code must negate the
  cosine passed to `refract_angle`-style helpers — otherwise wt points
  above the surface and every TT/TRT visibility guard kills the path
  (furnace energy ~0.31 -> 0.97 after the fix).
- Cycles `reflect(i,n) = i - 2(i.n)n`, so their `-reflect(i,n)` already
  equals LuxCore's `ReflectDir(i,n) = 2(i.n)n - i`. Adding a '-' flips
  the reflected direction below the surface and all samples die.
- Huang'22 curve kernel -> fixed-surface-point conversion: `f*cos =
  kernel / (arc_i * cosMi)`. The R lobe then reduces exactly to the
  standard slanted-GGX BRDF `F*D*G*scale/(4*cosMi)` — verified against
  brute-force microfacet MC (the curve kernel's jac/arc factors are
  measure-change terms, they do not belong in the surface BSDF).
- `roughness` (artist param) IS the GGX alpha directly; the energy LUT
  axis is `sqrt(alpha)` (Cycles convention).
- Chiang's far-field model is only energy-conserving on `ribbon`
  tessellation (~0.90 furnace). On `solid` cylinders grazing azimuths
  lose ~40% — that is the known far-field limitation Huang'22 solves
  (Huang: 0.97 on the same geometry). Furnace tests must pick the
  tessellation that matches the model's domain.
- aspectratio<1 legitimately reads <1 in a furnace: the elliptical
  cross-section has a narrower projected silhouette, hits outside
  |h|>radius are transparent by design (same as Cycles).
- Huang eval uses deterministic Hammersley VNDF quadrature for TT/TRT
  (Evaluate must be a pure function of wi/wo); Sample uses the Cycles
  self-normalized estimator (f/pdf = eval, pdfW = 1, energy-proportional
  lobe pick).

