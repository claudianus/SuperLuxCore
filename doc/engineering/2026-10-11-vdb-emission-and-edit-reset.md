# Finite VDB emission and CPU scene edits

Zero-extinction heterogeneous volumes previously lost every emission sample:
the delta tracker skipped cells whose extinction majorant was zero. Finite
emitting VDB regions now integrate their source with a uniform sample over
each such interval. This estimator also integrates sources between residual
events on paths where further scattering is disabled, retaining the analytic
minorant attenuation at the sampled position. CPU and device implementations
use the same estimator. Infinite zero-extinction emitting media have no finite
radiance integral; this change only integrates finite intervals.

Fixed-step heterogeneous marching now uses the segment emission calculated by
the homogeneous segment routine. Adding raw emission once per step made
radiance depend on the number of steps and omitted the segment length.
Marching retains its existing approximation for extinction within each step.

`PathCPURenderEngine::EndSceneEditLockLess` now calls the immediate no-tile
parent, resetting the eye sampler as well as restarting workers. Sobol shared
pixel passes use `assign` during reset: `resize` did not clear pass values when
the film dimensions were unchanged. This was visible as excess noise after
moving/scaling a VDB even though its mean brightness and geometry were right.

Validation is deliberately narrower than complete Cycles compatibility:

- `dev-tools/heterogeneous-emission-regression.py`: 256-square, 256-SPP raw
  RGB renders against the independent integral `Le * L` or
  `Le * (1 - exp(-sigma_a * L)) / sigma_a`. Six CPU and six Metal cases cover
  delta/march, lengths 0.5/2, zero extinction and absorption 0.4. All pass a
  2% gate; the largest measured error is 0.2016% for marching. The old installed
  wheel fails the first case with zero radiance and 100% relative error.
- Local source VDB scene edits, 1280 by 720 at 64 SPP: initial, transformed,
  named-density material edit, and disconnected Volume output all pass against
  fresh exports and independent fresh-repeat noise. Object counts are 1/1/1/0.
  The transformed live/fresh pixel MAE drops from 0.01213 to 0.003103, matching
  the independent fresh-repeat MAE 0.003107. This is a correctness/noise result,
  not a performance claim or a GUI redraw certification.

Evidence lives in `../test-scenes/validation-2026-10-11/vdb-attributes/`:
`native-emission-{before,cpu,metal}/metrics.json`, native build logs, and
`live-cpu-native-r3/metrics.json`. The emission tests used build-native SHA-256
`710843c1f1ddd1c603d3b8d4d0c65b8b84743d90df4358569bfb82832b7ca1bc`;
the complete local wheel including edit resets has native SHA-256
`ae2822ecfc023715215de5bd03719403e0ac433c3aecfa65ff786885cf9c97ff`.
The VDB reader/mapping and null bounds carrier are adapter changes. Colored
volume transport, broad blackbody units, edge reconstruction, Generated
coordinates, other platforms, and full production scene coverage remain
separate gates. No 99% or complete scene-compatibility claim follows from this
feature-level regression.
