# New / extended textures — blackbody (texturable) + whitenoise

## Blackbody texture — texturable temperature

Status: implemented (CPU + GPU + spectral). `blackbody.temperature` accepts a
texture, not only a constant.

**What/why.** Emission driven by a spatial field — fire / volume temperature,
procedural heat — needs the Planckian colour evaluated per shading point.
Previously `.temperature` was a compile-time constant, so a density grid's
temperature channel could not drive the colour. Now it can.

**How.** The RGB path uses a 160-entry temperature→RGB lookup table baked from
`TemperatureToWhitePoint` (the full Planck→XYZ integral is too costly per
shading point); the same normalized table is embedded in the kernel source.
The spectral path still evaluates the true Planckian SPD per wavelength
(`Spectral_BlackbodyEval` / `BlackbodySPD`), so spectral renders keep exact
physics rather than an RGB approximation.

- Constant `.temperature = <K>` still works and is byte-identical (exact
  integral, not the LUT).
- `blackbody.normalize = 0|1` — 1 gives normalized chromaticity, 0 the raw
  physical scale.
- **References:** Planck's law (blackbody SPD); CIE colour-matching functions.
  `scenes/cornell/bb-test.scn` demonstrates a density grid driving fire
  emission temperature.
- **Validation:** deterministic surface render is **bit-identical** CPU↔GPU
  (max|diff| = 0); volume fire render identical on CPU / OpenCL / Metal /
  spectral.

## Whitenoise texture — deterministic 3D-seed noise

Status: implemented (CPU + GPU). Cycles White Noise equivalent.

**What/why.** A hash of a 3D position seed to a deterministic [0,1) value /
colour — uncorrelated per-point noise used for randomization (sprinkle
variation, anti-repetition). Cycles parity.

**How.** `whitenoise.texture` = the vector seed (default `position`);
`whitenoise.seed` = an offset. The seed is hashed by bit-reinterpretation
(`as_uint` on GPU / union on CPU) + an integer avalanche mix so CPU and GPU
produce **identical** values even for negative / large world coordinates — a
plain arithmetic float→uint cast is UB for negatives and diverged between the
two (found and fixed during validation).

- `whitenoise.scn` float + colour outputs.
- **Validation:** CPU↔GPU identical distribution on a Cornell-box render.

## mathfunc texture — generic unary/binary math

Status: implemented (CPU + GPU). Backs Cycles `ShaderNodeMath` trig/exp/log
ops (SINE, COSINE, TANGENT, ARCSINE, ARCCOSINE, ARCTANGENT, ARCTAN2,
EXPONENT, LOGARITHM via ln(x)/ln(b) composition).

**Properties.** `mathfunc.op` = `sin|cos|tan|asin|acos|atan|atan2|exp|ln`;
`mathfunc.texture1` (operand), `mathfunc.texture2` (atan2 only; unary ops
ignore it). Scalar and float3 inputs both supported.

**Validation:** `dev-tools/e10_mathfunc_test.py` renders emission quads
through mathfunc on PATHCPU and PATHOCL (Metal via cl2msl) — 14/14 checks.

## gabornoise texture — sparse Gabor convolution

Status: implemented (CPU + GPU). Backs Cycles `ShaderNodeTexGabor`
(Value / Phase / Intensity outputs).

**Properties.** `gabornoise.vector` (eval coordinates, e.g. `position` or
`uv`), `gabornoise.scale` (coordinate multiplier, default 1),
`gabornoise.frequency` (kernel band rate, default 2),
`gabornoise.isotropy` (1 = all kernels at `orientation`, 0 = random
per-impulse orientation, blends in between),
`gabornoise.orientation` (base kernel angle, radians),
`gabornoise.output` = `value|phase|intensity`.

**Algorithm.** Independent implementation of Lagae et al. 2009 "Procedural
noise using sparse Gabor convolution" (2D case): a 3×3 cell neighbourhood
sums 8 impulse kernels per cell; each kernel is a Hann-windowed Gaussian
multiplied by a phasor (cos/sin of the orientation-projected offset).
The phasor sum is normalised by the analytically quadratured standard
deviation (Tavernier et al. 2019), then `value` maps to roughly [0, 1],
`phase` returns the phasor angle in [0, 1), `intensity` the normalised
phasor magnitude (Tricard et al. 2019). The impulse schedule is drawn from
the shared Tausworthe RNG seeded by the white-noise cell hash, so CPU and
GPU evaluate bit-for-bit identical noise.

**Limitations.** 2D only (Blender `gabor_type=3D` is not yet mapped); no
per-cell impulse-density texture input.

**Validation:** `dev-tools/e11_gabor_test.py` — PATHCPU vs TILEPATHOCL
(Metal via cl2msl) agree to ~1e-3 on mean/std/range for all three outputs.

**Known issue (pre-existing, unrelated to this texture):** PATHOCL
nondeterministically corrupts texture evals with multiple child inputs
(even `add` of two constants reads ~1/4 of the correct value on some
runs). TILEPATHOCL and all CPU engines are unaffected; tracked in
`roadmap.md` / needs a dedicated fix.

## rayinfo texture — Cycles Light Path node support

Status: implemented (CPU + GPU). Backs Cycles `ShaderNodeLightPath` in the
Blender adapter.

**What/why.** Cycles' Light Path node classifies the *ray that produced the
current shading point* (camera ray, shadow ray, diffuse/glossy bounce, ray
depth, ray length). LuxCore textures evaluate context-free from `HitPoint`,
so a small engine extension carries the needed ray context into `HitPoint`:
the intersecting ray's type flags, the BSDF event that generated it, the
`PathDepthInfo` counters, and the ray segment length. Both the CPU
`Scene::Intersect()` and the OpenCL `Scene_Intersect()` paths populate these
fields, so the texture evaluates identically on every engine.

**Properties.** `rayinfo.channel` = `iscameraray|isshadowray|isdiffuseray|
isglossyray|issingularray|isreflectionray|istransmissionray|
isvolumescatterray|raylength|raydepth|diffusedepth|glossydepth|
speculardepth|transmissiondepth|transparentdepth`.
Default channel: `isshadowray`.

- `iscameraray` / `isshadowray` read the ray-type flags recorded on the ray
  (`CAMERA_RAY` / `SHADOW_RAY` scene ray-type bits).
- `isdiffuseray` / `isglossyray` / `issingularray` / `isreflectionray` /
  `istransmissionray` classify the BSDF event that generated the incoming
  ray (`DIFFUSE`, `GLOSSY`, `SPECULAR`, `REFLECT`, `TRANSMIT` bits of
  `PathInfo::lastBSDFEvent`). They return 0 on camera and shadow rays, which
  have no generating bounce.
- `isvolumescatterray` returns 1 at volume-scatter shading points.
- `raylength` is the length of the ray segment that produced the hit.
- `raydepth` / `diffusedepth` / `glossydepth` / `speculardepth` /
  `transmissiondepth` return the `PathDepthInfo` counters at the hit point
  (total bounce depth and the per-event-class depths).
- `transparentdepth` counts `TRANSMIT` pass-through events
  (`GetPassThroughTransparency`) accumulated while tracing the current ray.

**Hit points without ray context** (light sampling, photon mapping, utility
intersections, texture evals outside the path) have all context fields
zeroed, so every channel safely returns 0.

**Validation.** `SuperBlendLuxCore/dev-tools/e23_cycles_compat_e2e_test.py`
renders a LightPath-driven mix (camera-ray red vs indirect green) through
the Blender adapter. A standalone scene test renders the same
`rayinfo`-driven mix on PATHCPU and PATHOCL with identical statistics
(camera branch 0.162 / indirect branch 0.027 mean contribution), and
`transparentdepth` was verified through a fully-transparent wall.

## Platforms

All textures: CPU, OpenCL GPU, Metal GPU (cl2msl-compatible kernel code).
