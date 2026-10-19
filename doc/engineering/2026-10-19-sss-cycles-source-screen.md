# Local Cycles source audit, rejected CPU proposals, and grazing guard repair

The local Blender 5.2 source at `/Users/modumaru/Desktop/code/blender-5.2`,
revision `d13f752e3b9c4f8c261cda552b1021f8bcc0382c`, was used directly.
Its `intern/cycles/kernel/integrator/subsurface_random_walk.h` shares a
maximum-albedo diffusion proposal (lines 255-263), selects channels with
throughput/albedo, combines direction/distance/channel densities, bounds
the intersection query by the sampled distance (line 411), and queries
the current object (line 416). `subsurface.h` lines 163-169 sets up a
diffuse exit BSDF. The latter differs from the refractive OpenPBR bulk
boundary target here; replacing it would change the model.

These are source observations, not a measured attribution of the entire
Cycles speed advantage to any one operation. The full convergence gap
remains unresolved.

## Channel experiments rejected

Evidence: `../test-scenes/validation-2026-10-19/sss-channel/` relative to
this component repository. A single RGB-channel future-throughput
projection worsened relative SSS RMSE from .454491 to .502252, or .491779
with an albedo-weighted selection. Rays increased from 1013.02 to
1433.83/1438.84 per pixel at 144 SPP. Both variants were removed.

A subsequent full-RGB latent-channel proposal retained every channel and
used posterior strategy likelihoods to evaluate the whole path mixture.
262144 paths per numerical case passed the direction/flight integration
checks, with maximum mean error 1.484 estimated standard errors and
incremental versus direct joint log-density error 2.84217e-14. Shadow-copy
and single-consumption guards passed. These numerical results did not
predict a practical improvement: artist-scene error was .454572 versus
.454491, while process CPU cost was 30.7251 versus 29.0512 seconds. The
prototype was removed. Frozen sources and output receipts remain;
consumed module overlays were deleted.

## Cycles-inspired bounded global query rejected

Evidence: `../test-scenes/validation-2026-10-19/sss-flight-clip/`.
The prototype peeked at an unchanged copy of the RNG, sampled the exact
existing free flight, and shortened the *global* BVH query. It included
a conservative band for the numerical boundary guard. It retained
global intersections for nested objects and excluded scenes containing
bevel impostors or hit-dependent volume overrides. The original ray
extent and authoritative scattering sampler remained intact.

1048576 proposal/RNG/guard checks passed, including 230564 guarded
boundaries covered by the query band. At 320x180, RGB, 144 SPP, seed 17,
CPU1, the raw RGB, sample-count and ray-count buffers were bit exact
between off/on. Wall times were 24.8539 and 24.9468 seconds; process CPU
times were 25.1090 and 25.2606 seconds. There was no adoption evidence.
The query change and its production environment control were removed.
This screen does not test a separately built per-object accelerator.

## Retained correction: grazing boundary guard

Both SSS scattering paths used the actual cosine in the proximity test
but `max(cosine,.001)` in the displacement. At a grazing angle this can
trigger against a distant boundary, then move a scatter *forward* to it.
For scatter .1, boundary 100, cosine 1e-8 and epsilon 1e-5, the previous
guard returned 99.92. The corrected guard returns .1. The same clamped
cosine is now used for both operations. Nongrazing arithmetic and the
non-SSS generic-volume branch remain unchanged.

`dev-tools/sss_boundary_guard_test.py` tests the actual helper against
the legacy negative control. 1048576 cases cover non-forward movement,
origin bounds, near-boundary correction and exact legacy agreement for
cosines >= .001. The constructed population reproduced 149629 legacy
forward moves. This number characterizes the probe distribution, not
the incidence of artifacts in artist scenes.

The final CPU native payload is recorded in `final-package/identity.json`
(the consumed module itself is removed after testing). The artist-scene
144-SPP RGB/sample/ray buffers match the prior baseline exactly. Full
CPU regression receipts are in `final-cpu/` and
`final-cpu-comparison.json`. These are bounded CPU correctness checks,
not a speed claim, a Metal check, or installed-Blender certification.
The selected installed Blender wheel was not replaced by a prototype.

All artist screens explicitly disabled denoising, adaptive sampling,
PSR, glossy filtering and radiance/variance clamps under the checked
`sss-unregularized-v2` contract and authored scene SHA
`d913ff75bc07a85ff0d4d70f196668558909667d40d188d5291ea628cb16816c`.
Independent 4096-SPP model-specific references still contain noise.
Shared-workstation single-thread timings are functional evidence and
must not be reported as uncontended speed ratios or Cycles parity.

Next substantial work should target the measured intersection cost
and refractive-exit light connections. A local-object traversal needs
an explicit proof that no other boundary can be missed, plus accelerator
build/memory costs in the total-time comparison. Repeating the rejected
channel variants or admitting their controls into defaults is unjustified.
