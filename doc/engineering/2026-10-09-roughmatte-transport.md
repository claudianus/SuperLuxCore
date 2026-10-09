# Rough matte transport and MIS directions

The no-edit Cycles Diffuse Roughness adapter exposed a CPU energy defect in
the existing EON material. A white sphere with roughness one under a uniform
white infinite light rendered with central mean 0.91954; a repeat with a larger
sample limit gave 0.91956. Direct-Sun fixtures passed and did not reveal this
defect. The sampling model and quality policy remain unchanged.

`eon::Pdf(wo, wi, roughness)` takes the fixed direction first and the sampled
direction second. CPU RoughMatte Evaluate/Pdf supplied these in reverse order,
so the PDF used by MIS disagreed with Sample. Both methods now select the
fixed direction by transport (`eye` for radiance, `light` for importance) and
lift both directions into the positive hemisphere together. This also aligns
negative-Z/backface evaluation with Sample. Importance Sample now multiplies
by the light-side fixed cosine, matching Evaluate and the native Matte
transport contract; radiance uses the light-side sampled cosine.

Private Release 2.11.19 was rebuilt and fully installed into an isolated
Blender 5.2.1 profile, including its cached wheel and runtime dependencies.
The existing Cycles node graph remains unmodified. Standard spectral
1280×720 white-furnace tests disabled noise halt, denoising and clamping and
used native sample limit 128 and Cycles sample limit 32. Both roughness zero
and one passed the 1% central-mean energy gate, finite-image and error gates.

| Native engine / face | Mean at roughness zero | Mean at roughness one |
| --- | ---: | ---: |
| PATHCPU front | 0.99976933 | 0.99964637 |
| Metal front | 0.99969435 | 0.99954909 |
| PATHCPU reversed faces | 0.99977630 | 0.99961805 |
| Metal reversed faces | 0.99970239 | 0.99954230 |
| BIDIRCPU front | 0.99816167 | 0.99809569 |

Four RGB front-face conditions also passed: roughness-one means were
0.99959254 CPU and 0.99956954 Metal. Both RGB and spectral comparison sheets
were directly inspected. PATH front/back images recover
the uniform white appearance. BIDIRCPU has a dark silhouette ring for both
Matte and RoughMatte; the central energy test does not certify that boundary
behavior or general bidirectional parity. Preserve that as a separate open
issue. Full colour/incidence sweeps and composite-closure production scenes
also remain open. These results are bounded physical validation, not full
Cycles compatibility or bitwise agreement.

Evidence: workspace
`test-scenes/validation-2026-10-09/cycles-scene-goal-phase15/furnace-after-pdf-fix`;
the preceding failure is retained alongside it under `furnace-before-pdf-fix`.
Harness: add-on `dev-tools/cycles-diffuse-white-furnace-test.py`, with optional
`SUPERLUXCORE_AUDIT_BIDIR=1` and `SUPERLUXCORE_AUDIT_BACKFACE=1`.
This repair must be included in fresh four-platform wheels before fixed
2.11.19 release publication; earlier 2.11.19 CI wheels are superseded.
