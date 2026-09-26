# Diffraction grating material — 1D reflective grating (CD rainbow)

Status: **shipped on CPU (`PATHCPU`) and GPU (`PATHOCL`, Metal-validated)** —
`scene.materials.X.type = diffraction`, exposed in SuperBlendLuxCore as the
"Diffraction (CD)" material node. Regression suite:
`dev-tools/e43_diffraction_test.py`. Demo scene:
`scenes/diffraction/cd-rainbow.scn` (720p render `cd-rainbow-720p.png`).

## What and why

A compact disc's rainbow is **wave-optics diffraction**: the 1.6µm-pitch
spiral data track acts as a 1D reflection grating, so a broadband source is
split into per-wavelength discrete lobes (diffraction orders). This is a
different mechanism than thin-film interference (`thinfilmcoating`) — the
colors come from *path-length differences across many grooves*, not
amplitude splitting in a coating. Uses beyond CDs: holographic foils,
diffraction-grating sheets, iridescent machined/brushed metal, vinyl
records, security holograms.

## Physics

In the local frame where `s` is the grating direction in the tangent plane,
`t` the groove direction, `n` the normal, with incident direction
`(a_f, b_f, c_f)`, each propagating order `m` is a discrete delta lobe
(Stam'99, "Diffraction Shaders"; GPU Gems ch.8):

```
a_m = -a_f + m·λ/d      b_m = -b_f      c_m = +√(1 - a_m² - b_m²)
```

- `d` = groove spacing (`spacing`, nanometers; a real CD is 1600).
- Valid orders: `a_m² + b_m² ≤ 1` — a finite set (~2·d/λ orders; 1600nm
  spacing over 380–780nm gives roughly 4–9).
- `m = 0` is the ordinary mirror lobe, always present.

**Order energy envelope**: lamellar-groove model. Order `m` gets weight
`sinc²(π·fill·(a_m - a_spec)/λ·d⁻¹ …)` — i.e. `sinc²` of the grating
equation residual around the facet specular `a_spec`. `blaze` (facet tilt,
degrees in the .scn) shifts `a_spec` off `a_m = -a_f` for blazed/holographic
gratings; `fillfactor` (groove duty cycle) sets the envelope width.
Orders are **importance-sampled proportional to this envelope**, so the
returned BSDF weight collapses to the constant `kr` — minimal variance and
no per-order weight bookkeeping.

**Roughness** (`roughness`, 0–1): Gaussian jitter of the cone direction
(σ ≈ 0.25·roughness) in the `(a, b)` plane — physically the groove wander /
wavefront error of a real pressed disc; visually it softens the rainbow
bands like a real CD instead of laser-sharp arcs.

## Groove orientation (`orientation`)

| value      | groove direction `t` | use |
|------------|----------------------|-----|
| `radial`   | circles around `center` (object-space point, default origin) | CD/DVD, vinyl |
| `radialuv` | circles around (`centeru`,`centerv`) in UV space | textured discs |
| `u` / `v`  | straight along U / V | linear gratings, holographic foil sheets |

Radial modes project the point→center direction onto the tangent plane, so
no mesh tangent data is required.

## Spectral vs RGB mode

- **Spectral rendering on** (`path.spectral.enable = 1`, recommended): the
  path hero wavelength λ_h diffracts and the secondary bins collapse
  (`Spectral::CollapseToHero()`, same mechanism as dispersive glass).
  Physically exact per-wavelength order directions; the 3-bin estimate
  converges to continuous rainbows over samples.
- **RGB mode**: one wavelength λ ~ U(380,780) is jittered per sample and
  converted via the `WaveLength2RGB` approximation (piecewise-linear
  spectrum → XYZ-normalized RGB, shared with glass dispersion). Unbiased
  for white light, slightly noisier; a fallback, not the intended mode.
- `passThroughEvent` drives the λ jitter and `u1` the roughness Gaussian —
  both derive from a Wang hash of the sample uniforms so CPU and GPU
  produce bit-identical directions given identical inputs.

## Delta-BSDF semantics (read before use)

The event is `SPECULAR | REFLECT` and `Evaluate()` returns 0 — same
convention as `mirror`. Consequences:

- **No next-event estimation** into the lobes: an infinitesimal light can
  never be hit by a delta bounce. Light must reach the camera via
  BSDF-sampled paths hitting *finite-area* emitters or the environment.
  This is physically consistent (a true grating lobe is measure-zero).
- **Light tracing / BiDir / PhotonGI caustics work for free**: diffracted
  photons splat as normal specular paths, so a CD produces moving rainbow
  caustics on walls automatically.
- A scene containing *only* point/directional lights shows the material as
  essentially black — use area/environment light (the demo scene uses an
  HDR studio environment whose features the grating smears into rainbows).

## Scene properties

```
scene.materials.cd.type = diffraction
scene.materials.cd.kr = 0.95 0.95 0.95     # reflectance (texture-able)
scene.materials.cd.spacing = 1600.         # groove spacing in nm (texture-able)
scene.materials.cd.orientation = radial    # u | v | radialuv | radial
scene.materials.cd.center = 0. 0. 0.       # radial: object-space center
scene.materials.cd.centeru = 0.5           # radialuv: UV center
scene.materials.cd.centerv = 0.5
scene.materials.cd.roughness = 0.03        # lobe blur (texture-able)
scene.materials.cd.fillfactor = 0.5        # groove duty cycle -> envelope width
scene.materials.cd.blaze = 0.              # blaze angle, degrees
scene.materials.cd.orders = 8              # max searched order (clamped to 32)
```

`spacing` textured → holographic-paint effects (e.g. drive it with a noise
texture for sparkle). `fillfactor`≈0.5 is a square groove; lower values
concentrate energy in low orders.

## Files

- CPU: `include/slg/materials/diffraction.h`, `src/slg/materials/diffraction.cpp`
- GPU twin: `include/slg/materials/materialdefs_funcs_diffraction.cl`
  (`DiffractionMaterial_EvalOp`); dispatch in `material_funcs_evalops.cl`;
  OCL layout `DiffractionParam` in `material_types.cl`; serialization in
  `src/slg/engines/pathoclbase/compilematerials.cpp`; kernel registration in
  `include/slg/kernels/kernels.h` + `pathoclbaseoclthreadkernels.cpp`
- Parser branch: `src/slg/scene/parsematerials.cpp` (`"diffraction"`)
- Blender: `SuperBlendLuxCore/nodes/materials/diffraction.py`
- Tests: `dev-tools/e43_diffraction_test.py`
- Demo: `scenes/diffraction/cd-rainbow.scn`, `gen_assets.py` (annulus mesh +
  PFM studio env), `cd-rainbow-720p.png`

## Regression coverage (`dev-tools/e43_diffraction_test.py`)

1. **T1 mirror equivalence** — huge `spacing` collapses all orders onto the
   mirror direction; block-averaged image + total energy match a `mirror`
   material (per-pixel relative metrics are dominated by MC noise at the
   light-boundary silhouette, so the test compares 16×16 block means and
   global energy instead).
2. **T2 spectral CD response** — radial-grating disc under a finite
   emitter produces a hue-spread rainbow (histogram dispersion metric).
3. **T3 CPU/GPU parity** — PATHCPU vs PATHOCL RMSE within MC tolerance.
4. **T4 RGB fallback** — finite, nonzero render with spectral mode off.

## References

- J. Stam, "Diffraction Shaders", SIGGRAPH 1999.
- NVIDIA GPU Gems, ch.8 "Simulating Diffraction" (delta-lobe direction
  formula used verbatim).
- Yan et al., "Rendering Specular Microgeometry with Wave Optics" (2018);
  Steinberg & Yan, "A Generic Framework for Physical Light Transport"
  (2021) — justification that a pure grating needs only the Fourier-limit
  delta directions, no full wave-optics transport.
- Werner et al., "Scratch Iridescence" (2017) — related groove
  orientation/envelope treatment.

## Film row order gotcha (learned during this work)

`Film::GetOutput` buffers are `x + y·width` with **y=0 at the image bottom**
(OpenGL/PBRT `rasterToScreen` convention — verified in
`src/slg/cameras/perspective.cpp`). Blender viewport/final consume it
bottom-up natively, but `numpy.reshape(h,w,3)` → `PIL.Image.fromarray`
treats row 0 as top: **flip with `rgb.reshape(h,w,3)[::-1]` when dumping
PNGs from Python**. Existing numeric tests are unaffected; any Python
image dump needs the `[::-1]`.
