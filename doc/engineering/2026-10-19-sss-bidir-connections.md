# Conditional CPU bidirectional SSS connections

This is an explicit diagnostic option, not a default SSS improvement or a
Cycles convergence result. PATHCPU, the physical material model and the
installed Blender package are unchanged. Evidence is under
`../test-scenes/validation-2026-10-19/sss-bidir-connections/`.

## Local Cycles reference and scope

The local Blender source is `/Users/modumaru/Desktop/code/blender-5.2`,
commit `d13f752e3b9c4f8c261cda552b1021f8bcc0382c`. Its
`intern/cycles/kernel/integrator/subsurface_random_walk.h` uses a dedicated
walk, local object intersections, throughput/albedo channel selection and
forward/backward Dwivedi direction-distance mixtures. The implementation
uses the maximum channel albedo for the guiding fit; its comments explicitly
describe the cost and outlier tradeoff of channel-dependent phase sampling.
`subsurface_shader_data_setup` in `subsurface.h` installs a diffuse exit
closure. That closure is a different boundary model from the retained
refractive bulk boundary here. Copying the exit behavior would not preserve
the current model. These observations come from the local source, not an
assumption about renderer branding or public speed figures.

The existing PATHCPU dedicated loop already implements the analogous
direction/color/flight mixture while retaining global geometry and the
authored boundary. This study instead bounds the cost of BIDIRCPU's many
eye-light vertex connections; it does not establish that BIDIRCPU should
replace PATHCPU for the artist scene.

## Conditional selection

`SUPERLUXCORE_SSS_BIDIR_CONNECTIONS=K` enables a budget of 1..64 connections
at a non-delta SSS eye vertex when the light subpath contains more than K
vertices. With no option, K=0 and the original complete connection loop runs.
Light generation, the physical connection kernel and its MIS weights remain
unchanged. Selection does not alter the phase or distance sampler.

For a fixed pair of subpaths let the original contribution sum be
`sum_i C_i(u)`, where u drives stochastic visibility. A selected connection
is evaluated as `C_i(v)/(K*p_i)`. The selection CDF uses 90% normalized
nonnegative light-throughput/inverse-distance-squared importance and 10%
uniform support. The distance floor bounds the proposal only. Invalid
importance entries fall back to their uniform mass. We rescale importance
by its maximum to avoid overflowing the normalization sum.

K strata use `(u+j)/K`, followed by a remap within the selected CDF interval
for visibility. The aggregate of these strata integrates the same uniform
unit interval, so the reciprocal selection mass preserves the original sum.
The full RGB contribution is retained. Repeated selections are allowed.
Only a copy of the eye throughput is scaled; stored path vertices are not
modified. Per-thread vectors reuse their capacity instead of allocating
per connection. Storage is bounded by the existing light subpath.

Additional diagnostics: `..._CONNECTION_UNIFORM` selects uniform importance;
`..._CONNECTION_ALL` tests all non-delta vertices in conservative non-SSS
oracles; `..._CONNECTION_PROFILE` reports potential/actual calls and selected
eye vertices. These flags are explicit and are not enabled by Auto.

## Verification and limits

`python3.13 dev-tools/bidir_connection_test.py --out <receipt>` compiles the
actual selector header in a temporary directory and removes the binary.
40 cases and 41,943,040 deterministic quadrature draws test positive support,
remapped nonconstant visibility, extreme importance imbalance, zero weights,
and equal integrals for K=1/K=4. Maximum relative integral error is
6.63355e-5. Omitting the reciprocal mass fails all 16 applicable negative
controls. This checks the conditional estimator, not a full BDPT proof.

The actual CPU conservative diffuse-transmission/scattering furnace was run
with K=0/1/4, three seeds, requested 16,384 SPP, adaptive sampling and all
reconstruction/clamping off. All nine means fall in [0.99932059, 1.00033465]
against expected 1. The predeclared per-run tolerance was .012; the observed
maximum deviation was .00067941. Actual Sobol sample counts are recorded and
are not exactly the requested count. The diagnostic ALL flag activates the
selector in this known-solution non-SSS fixture.

The refractive SSS furnace uses the production SSS eligibility without ALL.
Three requested-8192-SPP seeds average .94845572 for all connections and
.94840479 for K=1; their paired mean difference is -.00005093. This is a
finite Monte Carlo comparison, not an analytic SSS energy oracle.

The final Release module passes all 14 existing CPU SSS fixtures at exact
256 SPP, 16x16, one thread, including spectral, textured, anisotropic,
interior-light, nested and transparent-shadow cases. All 112 saved RGB,
sample-count and AOV buffers are bit-identical to the pre-selection module.
The final isolated native SHA256 is
`5948f1b3252f04ff06859a6c55b537a09eaa4a54109ecca9da2a60f71701e7a6`;
the raw build SHA256 is
`5c7cb939d61231d992e5ff38ec730baff4345b8887143bf75c79712dc8a137c2`.
The build is conditioned on the recorded shared workspace and dependencies,
not a claim of a clean release artifact.

In the 320x180 artist pilot at requested 4 SPP, the complete loop made
13,107,218 connection calls; K=1 made 2,678,071 (79.6% fewer). This is an
algorithmic work count, not a certified speedup. Most raw variance remained
in the underlying eye/light paths. At this very low count, Sobol termination
produces 3..5 actual samples per pixel, enough to change peak normalization
substantially. The pilot therefore does not establish convergence ROI.

A separate trial reused the existing chromatic Dwivedi proposal in both
bidirectional walks with classical MIS heuristic densities. It reduced
connection work but increased raw outliers: the CPU1 4-SPP whole-frame peak
rose from 1741.55 to 77790.15; SSS-region relative RMS against the finite
PATH reference rose from 3.7963 to 4.1859. It was rejected and removed from
production source. Its private source, raw arrays and native identity are
retained for diagnosis. Packet roulette and light-channel experiments were
also removed; they are not silently bundled into this selector change.

All final render checks use one CPU thread while another user-owned Blender
is open. They are functional-only, `performance_eligible=false`. No process
belonging to that Blender was stopped. No equal-time Cycles, uncontended
ABBA, installed-addon, Metal or viewport improvement is claimed. BIDIRCPU's
private screen also has a different finite-depth contract from the canonical
PATH/Cycles comparison, so those errors are not directly interchangeable.
Consumed native overlays were removed after measurement. The final candidate
was also removed at user-requested closure; one immutable baseline remains.
The raw build, identities, source and render evidence are retained. See
`2026-10-19-sss-tile-balance.md` for the closing checks and incomplete scope.
