# CPU SSS indexed sampling: local Cycles audit and bounded cost reduction

Local Cycles was read directly at `/Users/modumaru/Desktop/code/blender-5.2`,
revision `d13f752e3b9c4f8c261cda552b1021f8bcc0382c`.
`intern/cycles/kernel/integrator/subsurface_random_walk.h` combines an
internal walk, object-local intersection, boundary guidance and RGB-channel
MIS. Its similarity coefficients are enabled after bounce 9; their
transformation is an identity at zero anisotropy. `subsurface.h` sets up a
diffuse exit BSDF. The dedicated-loop and sampling structure are applicable
references. Replacing the refractive OpenPBR boundary by that diffuse exit
or changing the physical anisotropic phase is outside this optimization.
These source differences do not measure how much each accounts for the
Cycles performance/convergence gap. Earlier direct-source experiments and
rejections are recorded in `2026-10-19-sss-cycles-source-screen.md`.

## Retained change

`PathTracer::RenderSSSInterior` generated five light-sampling dimensions even
when the existing exact opaque-domain test skipped direct-light sampling.
For SOBOL and TILEPATHSAMPLER, these are indexed queries with no draw-order
state; they now occur only when needed. Random and lazy samplers retain the
previous five draws. No scene, physical coefficients, boundary, PDF,
roulette rule, depth limit, sample index, radiance clamp or denoiser changed.
`SUPERLUXCORE_SSS_RETAIN_BLOCKED_NEE_SAMPLES=1` restores the old schedule
for an in-process-code A/B control (each render runs in its own process).

## Evidence and limits

Evidence is `../test-scenes/validation-2026-10-19/sss-indexed-samples/` from
the component root. `final-cost-plan-runs.json` records eight sequential
ABBABAAB runs, CPU4, 320x180, RGB144, seed17, using the same v2 artist scene.
Every run has identical raw RGB, sample-count and ray-count buffers.
All fidelity controls are checked explicitly: no PSR, glossy filtering,
clamps, adaptive sampling or denoising.

| Native scope | Before mean | After mean |
| --- | ---: | ---: |
| Config/preparation/tracing/readback wall | 7.206762 s | 6.616195 s |
| Process CPU | 24.488896 s | 22.747290 s |

Observed reductions are 8.195% wall and
7.112% process CPU. This is a shared-workstation
native screen, **not** a certified uncontended speedup or installed-Blender
measurement. `performance_eligible=false` remains explicit. An initial
sequence overlapped an owned rebuild and is excluded in
`initial-cost-excluded.json`; only the completed final-module sequence
above is summarized. Builds and functional regressions did not overlap
that final sequence.

`final-cpu-comparison.json` verifies all 112 RGB/AOV/count buffers for
14 CPU cases against the prior LPE-fixed module, at 16x16, fixed256SPP.
Coverage includes spectral, textured, anisotropic, nested, internal/area/
point lights and shadow transparency. A preliminary 12x12/64-SPP run
failed the existing furnace statistical gate; its failure was retained,
then the established 16x16/256-SPP fixture was run without loosening it.

Final frozen native SHA256 is `eee20289c68af956a5f6eecfe0bbfb2ea116f07bd1ca3dfa4d7540d8f4baffb6`;
raw build SHA256 is `4a1beaad232a5c881e0829d9d0a421a7316c12cb9f3abcc60cd1362217d25bd9`.
This CPU-only edit changes no device kernel. No new Metal or installed
wheel validation is claimed, and no selected Blender wheel was replaced.
At fixed SPP the raw noise is identical. This does not establish an
equal-time raw-noise reduction against Cycles, and the large convergence
gap remains open.

## Rejected neighboring experiments

`../test-scenes/validation-2026-10-19/sss-escape-cone/` records an exact
mixture proposal with 25% tangent escape caps. Its numerical integration
passed, but artist SSS relative RMSE rose from .454491 to 2.013240 despite
fewer traced rays. It was removed. The gate used volumeDepth>=3, which
also counts SSS boundaries; it did not isolate exactly the fourth scatter.

`../test-scenes/validation-2026-10-19/sss-bidir-measure/` records a separate
CPU phase/BDPT audit. The isolated actual BSDF probe reproduces the old
light-side projected-cosine error: nonabsorbing isotropic eye/light sample
means are 1/.5, versus 1/1 in the prototype (110592 checks per version).
The combined volume-measure/RR/phase prototype still fails a unit-environment
conservation furnace: three 1048576-SPP seeds give mean .995589892,
seed SD .000057318. It is **not promoted**. All three production files
were restored byte exactly; prototype sources, negative-control results,
fingerprints and child-reaping receipts remain. Consumed native overlays
were deleted. Do not describe this audit as a production BDPT fix or an
SSS convergence improvement.
