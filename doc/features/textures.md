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
and integer/fractional ops, plus componentwise Vector Math Floor, Ceil,
Fraction, Snap, Minimum and Maximum. LOGARITHM uses ln(x)/ln(b) composition;
Compare uses absolute difference, maximum epsilon and inclusive comparison.

**Properties.** `mathfunc.op` =
`sin|cos|tan|asin|acos|atan|atan2|exp|ln|sinh|cosh|tanh|invsqrt|floormod|snap|floor|ceil|trunc|fract|round|max|lessequal|min`;
`mathfunc.texture1` is the first operand. `mathfunc.texture2` is evaluated
for `atan2`, `floormod`, `snap`, `max`, `lessequal` and `min`. Scalar and float3
inputs are supported.

`snap(a,b) = floor(a/b)*b`, with zero output when `b == 0`.
Float3 increments are applied componentwise, including negative and zero
increments. This matches Blender Math/Vector Math Snap, not nearest rounding:
`snap(1.75,1)=1`, `snap(-1.25,1)=-2`.

```properties
scene.textures.snapped.type = mathfunc
scene.textures.snapped.op = snap
scene.textures.snapped.texture1 = 1.75 -1.25 0.5
scene.textures.snapped.texture2 = 1 1 0
```

The resulting vector is `(1,-2,0)`. No intermediate divide/round/multiply
textures are needed.

`floor`, `ceil` and `trunc` use their native float operations; `fract(a)`
is `a-floor(a)`. Blender `round(a)` is `floor(a+0.5f)`, so
`round(-1.5)=-1`, not nearest-with-ties-away-from-zero. The half-add is
float32 even for constant inputs: `round(8388609.f)=8388610.f`.
All five unary operations apply independently to float3 components.
Previously serialized operation IDs remain unchanged.

`min(a,b)` and `max(a,b)` use native floating-point extrema; `lessequal(a,b)`
returns one when `a <= b`, zero otherwise, including unordered NaN comparisons.
All three apply componentwise to float3 spectrum outputs. The adapter converts
Vector/Color→Float at scalar socket boundaries before exporting Math.
Blender Compare is `abs(a-b) <= max(epsilon, 1e-5f)`; constant subtraction
also rounds to float32 before comparison.

Math and Vector Math Minimum/Maximum each use one native operation, rather
than subtract/compare/multiply/add selection. This avoids an overflowing
intermediate `a-b` for finite opposite-sign inputs near float32's maximum.
Two linked inputs now require three explicit textures instead of six.

**Validation:** `dev-tools/e10_mathfunc_test.py` renders emission quads
through mathfunc on PATHCPU and PATHOCL (Metal via cl2msl) — 14/14 checks.
`dev-tools/math-snap-regression.py` additionally exercises positive/negative
and zero Snap increments, signed integer boundaries, fractional values,
large float32 Round boundaries, SDL texture round-trip, and real
CPU/isolated Metal rendering. The adapter's `snap_node_e2e_test.py` exports
actual Blender nodes and feeds their graphs into the same renderer gate:
310 checks passed, including scalar Clamp, typed vector/colour inputs,
inclusive Compare/minimum epsilon, float32 spacing boundaries, NaN
differences, componentwise Vector Math Floor/Ceil/Fraction, linked/folded
extrema, and HDR/subnormal-input quotients. Seventy installed-package
CPU/Metal checks also passed. Radiance tolerance is 0.05, not bitwise parity.

## hitpoint Generated coordinates — transformed base-mesh bounds

`hitpoint.channel = generated` normalizes the shading point in the base
mesh's authoring frame. A cached 3×4 map keeps translated, rotated and
nonuniformly scaled baked meshes in the same frame as their bounds.
Instances and object-motion wrappers use the shared base map after undoing
their wrapper transform. Flat axes return 0.5.

The cache is prepared before shading and invalidated by geometry or applied
transform changes. Only meshes whose material reads Generated are scanned.
Live noncommuting rotation/translation edits are checked against Blender's
reference coordinates after scene-edit cache invalidation on CPU and Metal.
Evaluation performs neither a full mesh scan nor bounding-box division.
The GPU mesh descriptor stores a 48-byte map instead of a 24-byte bbox;
texture descriptors are unchanged and no per-vertex coordinates are added.

Four transformed direct/instance Blender fixtures compare Generated with
independently interpolated Blender texspace reference values. Their error
is amplified 1000× before the ordinary 0.05 radiance gate. Complete
undeformed ORCO, custom texspace and whole-object bounds across material
submeshes are not established by this base-mesh approximation.


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
