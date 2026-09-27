# OpenPBR measured-media SSS presets

## What / why

OpenPBR's subsurface lobe is an albedo-parametrized random-walk SSS: the
parser builds an implicit homogeneous interior volume from
`subsurfacecolor` (apparent diffuse albedo) and `subsurfaceradius` ×
`subsurfaceradiusscale` (per-channel transport mean free path). Correct
values for real media were previously left to the artist — the three
controls are interdependent and the numbers live in papers, not heads.

`subsurfacepreset` seeds all three from a table of *measured* materials
so e.g. skin is a dropdown pick, not a parameter hunt.

## References

- Jensen, Marschner, Levoy, Hanrahan, *"A Practical Model for Subsurface
  Light Transport"*, SIGGRAPH 2001 — Table III measured `(σa, σs')`
  (Skin1/Skin2, Apple, Chicken1, Cream, Marble, Potato, Skimmilk,
  Wholemilk, Ketchup), as tabulated in PBRT-v3 `src/core/medium.cpp`.
- Conversion to the albedo-parametrized form (`dev-tools/gen_sss_presets.py`):
  single-scatter albedo `α = σs'/σt'`, apparent albedo via the vdH/CB15
  hybrid diffusion reflectance at the medium's canonical IOR (the same
  remap the CB15 path inverts — see
  `doc/engineering/openpbr-sss-findings.md`), `radius = 1/σt'` of the
  strongest channel, `radiusscale = per-channel mfp ratios`.

## Properties

`scene.materials.X.subsurfacepreset = <name>` on `openpbr` materials.
Recognized names (canonical IOR baked into each entry):

| preset | albedo (RGB) | radius RGB | scale (m) | IOR |
|---|---|---|---|---|
| `skin_light` | 0.632 0.466 0.390 | 1.0 0.664 0.570 | 9.07e-4 | 1.4 |
| `skin_dark` | 0.468 0.289 0.191 | 1.0 0.735 0.518 | 1.30e-3 | 1.4 |
| `marble` | 0.846 0.810 0.775 | 1.0 0.835 0.729 | 4.56e-4 | 1.5 |
| `cream` | 0.977 0.905 0.742 | 0.429 0.579 1.0 | 3.16e-4 | 1.35 |
| `whole_milk` | 0.912 0.887 0.774 | 1.0 0.794 0.674 | 3.92e-4 | 1.35 |
| `skim_milk` | 0.825 0.824 0.703 | 1.0 0.574 0.366 | 1.43e-3 | 1.35 |
| `ketchup` | 0.229 0.013 0.004 | 1.0 0.232 0.163 | 4.15e-3 | 1.35 |
| `apple` | 0.855 0.849 0.564 | 0.879 0.842 1.0 | 4.96e-4 | 1.35 |
| `potato` | 0.779 0.640 0.279 | 0.982 0.945 1.0 | 1.49e-3 | 1.35 |
| `chicken` | 0.365 0.218 0.186 | 1.0 0.575 0.289 | 6.06e-3 | 1.4 |

Semantics:

- A preset supplies the *defaults* for `subsurfacecolor`,
  `subsurfaceradius` and `subsurfaceradiusscale`. Any property the scene
  also defines wins, so preset + albedo texture = measured radii under
  an artist albedo.
- `subsurfaceradius` is in **scene units** — for meters-scaled scenes
  the table values are already correct; rescale for other units.
- A preset alone also enables the implicit SSS volume (no explicit
  `subsurfaceradius` needed) — the gate only requires `subsurfaceweight`
  to be defined and `volume.interior` to be unset.
- Unknown names are a parse error, not a silent fallback.

## Test scenes / validation

- `dev-tools/e53_sss_presets.py` — parses preset and explicit materials,
  compares the generated interior volume via `Scene.ToProperties()`
  (bit-exact albedo/mfp/IOR/profile), explicit-override priority,
  unknown-preset rejection, and CPU/OpenCL/Metal render parity.
- `dev-tools/e53_sss_visual.py` — 1280×720 AgX-Punchy hero shot:
  `skin_light` vs `skin_dark` Suzanne heads under a warm backlight.
- `dev-tools/gen_sss_presets.py` — regenerates the table from the
  measured coefficients (source of truth for both engine and adapter).

## Platforms

CPU / OpenCL / Metal — presets only change parsed defaults; transport is
the existing CB15-remapped homogeneous volume on every backend.
