# Path-space regularization (PSR)

Blurred-lobe path regularization for secondary vertices — the
SuperLuxCore equivalent of Cycles' *Filter Glossy* / Corona's
caustic smoothing, based on:

- Kaplanyan & Dachsbacher, *Path Space Regularization for Holistic and
  Robust Light Transport*, CGF 32(2), 2013.
- Weier et al., *Optimised Path Space Regularisation*, CGF 40(4), 2021.

Rough glossy/transmission lobes at secondary bounces get their
microfacet alpha inflated (`alpha' = sqrt(alpha² + σ²)`), which lets
BSDF sampling reach caustic chains (S-D-L through rough glass) that
plain path tracing almost never hits. The estimator is biased but
self-consistent (Sample/Evaluate/Pdf share the inflated alpha), same
trade-off Cycles documents for Filter Glossy.

## Properties

| Property | Default | Meaning |
|---|---|---|
| `path.regularization.sigma` | `0` | Alpha-space blur radius applied at gated vertices. `0` disables (output bit-identical to before). |
| `path.regularization.mindepth` | `1` | Blur applies to vertices with `rayDepth >= mindepth`; `1` keeps the camera-visible bounce exact. |

Blender: *Filter Glossy Sigma / Min Depth* under the path settings in
the sampling panel.

## Coverage (v1)

- Engines: PATHCPU, PATHOCL (dense + wavefront), plus any engine built
  on `PathTracer`/`pathoclbase` (TILEPATH*, RTPATH* inherit the
  plumbing; light-path seeds are set on both eye and light tasks).
- Materials: `glossy2`, `roughglass`, `metal2` (GGX and Schlick
  branches). Delta lobes stay exact — see
  `doc/engineering/path-space-regularization.md` for the design and
  the delta-substitution follow-up.

## Regression

`dev-tools/e99_psr_regularization.py` — roughglass plate scene on
PATHCPU + PATHOCL: finite output, measurable sigma effect, σ=0 anchor.
