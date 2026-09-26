# Diffraction grating material — implementation notes

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Diffraction grating material (`scene.materials.X.type = diffraction`)

1D reflective grating (Stam'99 / GPU Gems ch.8): order m is a delta lobe
`a_m = -a_f + m·λ/d` in the (grating dir s, groove dir t, n) frame —
`d` = `spacing` in nm (real CD = 1600). `orientation` = `u|v|radialuv|
radial` (radial modes project p→`center` onto the tangent plane; no mesh
tangents needed). Orders are importance-sampled by the lamellar sinc²
envelope (`fillfactor` width, `blaze` deg shifts the facet specular) so
the BSDF weight collapses to constant `kr`. `roughness` = Gaussian jitter
of the cone dir (blurs bands like a real disc). Spectral mode diffracts at
the hero wavelength + `CollapseToHero()`; RGB fallback jitters λ∈[380,780]
through `WaveLength2RGB` (glass dispersion twin — keep in sync). Wavelength
+ roughness jitter come from a Wang hash of the sample uniforms so CPU/GPU
give bit-identical directions. `SPECULAR|REFLECT`, `Evaluate`=0 → no NEE
into lobes (finite emitters only); light tracing makes real diffraction
caustics for free.

- Files: `materials/diffraction.{h,cpp}`,
  `materialdefs_funcs_diffraction.cl` (EvalOp twin), `DiffractionParam`
  in material_types.cl, compilematerials serialization, kernels.h +
  pathoclbaseoclthreadkernels source registration, parser branch
  `"diffraction"`, BLC node `nodes/materials/diffraction.py`.
- Regression: `dev-tools/e46_diffraction_test.py` (T1 mirror-limit vs
  `mirror` via 16×16 block means — per-pixel relative metrics fail on
  light-silhouette MC noise; T2 hue spread; T3 CPU/GPU; T4 RGB finite).
- Demo: `scenes/diffraction/cd-rainbow.scn` + `gen_assets.py` (annulus
  mesh + `studio-env.pfm` equirect HDR written by hand — PFM is the
  simplest writer-free HDR format OIIO reads; `env.gamma = 1.0` keeps it
  linear). Composition that works: delta lobe + HDR env = every escaped
  ray smears an env feature into rainbows; thin bright strips become
  long rainbow slashes. `infinite` light NEEDS `file`; use
  `constantinfinite` for uniform fill.
- **Film buffer row order**: `GetOutput` is `x + y·w`, y=0 at image
  BOTTOM (`rasterToScreen`, perspective.cpp). numpy→PIL dumps need
  `reshape(h,w,3)[::-1]` or the image is upside-down. Blender consumers
  are bottom-up native (no flip needed there).

