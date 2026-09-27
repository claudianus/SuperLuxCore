# Dispersion: Cauchy-B and Sellmeier 3-term (S4)

## What

Dielectric materials (glass, roughglass, disney, openpbr) gain
physically-accurate dispersion. Two models:

- **Cauchy-B** (legacy): `n(λ) = A + B/λ²`, property `cauchyb`.
- **Sellmeier 3-term**: `n²(λ) = 1 + Σᵢ Bᵢλ²/(λ²−Cᵢ)` (λ in µm, C in µm²),
  enabled by a **named glass preset** or **explicit coefficients**:
  - `scene.materials.<m>.sellmeier = "N-BK7"|"N-SF6"|"N-SF10"|"F2"|"fusedsilica"|"diamond"|"water"`
  - `scene.materials.<m>.sellmeierb = B1 B2 B3` + `scene.materials.<m>.sellmeierc = C1 C2 C3`

Sellmeier coefficients define `n(λ)` absolutely — when present they
**replace** the Cauchy model for that material (`interiorior` is then only
the volume/tracking IOR; keep it consistent with the coefficient mean).

## Semantics

A shared `slg::Dispersion` descriptor (`EvaluateDispersion`) resolves once
per hit: Sellmeier wins when both coefficient textures exist, else Cauchy-B
when > 0. GPU sentinel: `sellB.x < 0` ⇒ Sellmeier inactive (mirrored on
CPU — a negative B1 falls back to Cauchy for parity).

- **Direction-defining IOR** (`DispersiveIOR`): hero-wavelength `n` under
  spectral transport; **560nm reference** under non-spectral transport —
  so Sellmeier still refracts at its real index instead of the base
  `interiorior`.
- **Per-bin Fresnel** (`DispersiveFresnelR`): spectral mode evaluates
  dielectric R at each live bin's `n(λᵢ)`; non-spectral evaluates once at
  the 560nm effective index (so grazing Fresnel stays correct).
- **Spectral bin termination**: a dispersive transmission collapses the
  path to its hero bin (uniform-pick ×N weighting), preserving unbiasedness.
- **Non-spectral legacy path** (glass): wavelength sampled from u0 ∈
  [380,780]nm → per-path IOR + `WaveLength2RGB` tint, unchanged semantics.

## Where

| Material | Properties | Notes |
|---|---|---|
| `glass` | `cauchyb` \| `sellmeier`/`sellmeierb`/`sellmeierc` | reflection+transmission, film-aware |
| `roughglass` | same | microfacet dielectric BTDF |
| `disney` | same | integrated transmission lobe |
| `openpbr` | `dispersion` (Cauchy-B) \| `sellmeier`/`sellmeierb`/`sellmeierc` | thin-film-aware stack |

MNEE (manifold next-event) solves dispersive glass chains with either model
(`Dispersion` drives `etaVertex` at the hero wavelength).

## Implementation notes / gotchas

- `Bᵢλ²/(λ²−Cᵢ)` denominators can be **negative** (IR resonance terms at
  visible λ): the guard clamps magnitude only, never folds to +ε —
  `d = l²−C; Bᵢl² / (|d|>1e-9 ? d : sign-preserving 1e-9)`. A positive-only
  clamp produced `n² ≈ 4e11` → total internal reflection → black render.
- OpenCL `copysign` doesn't survive cl2msl translation — use a sign
  ternary instead.
- Kernel compile errors are silent without a debug handler; dump
  `/tmp/luxcore_metal_src.msl` and compile standalone (`metal -c`) or via
  a `makeLibrary` harness to see them.
- `.cl` edits require a rebuild — kernel sources are embedded at build
  time; runtime dumps show the *translated* program, not your edit.

## Validation

`dev-tools/e49_sellmeier_dispersion.py`:
- Sellmeier glass sphere under white env → chromatic fringing
  (saturation ≈ 0.25-0.29 vs 0 for plain glass), mean ≈ 1.
- Named preset ≡ explicit B/C (image diff ~4%, Monte-Carlo noise level).
- Cauchy parity unchanged; glass/roughglass/disney/openpbr CPU↔GPU
  parity (mean rel. error ≤ ~10%).

## Platforms

CPU / OpenCL / Metal / Vulkan — single shared evaluation code path on both
host and device.
