# Texture-driven image coordinates

`imagemap.vector` is an optional texture returning raw coordinates. Its X/Y values are sampled directly, overriding the legacy `mapping.*` block. Omitting it retains the existing image mapping and analytic derivative path. This allows a Blender adapter to provide any acyclic Vector graph, including chained and linked Mapping nodes, without changing the image or scene.

CPU coordinate evaluation pauses spectral conversion only while obtaining the data vector. GPU compilation emits balanced raw-data pause operations around the coordinate subtree and stores its texture index in `ImageMapTexParam`. Image colour evaluation then retains its normal spectral leaf contract. References, replacement edits, image-map dependencies and SDL serialization retain the optional input. Native CPU and GPU image filtering/wrap implementations remain in use.

For a texture-driven vector, image Bump uses the generic surface finite-difference path, so the coordinate graph is reevaluated at each surface offset. The image without that input retains its existing analytic UV derivative path. Sampling coordinates does not mutate the hit point.

Blender 5.2.1 / actual native and package version 2.11.17 passed macOS ARM CPU and actual Metal 1280×720 tests: 13 RGB coordinate conditions, four standard spectral conditions, and four image-height Bump/Normal compositor conditions per backend. RGB max MAE was CPU 0.00010423 and Metal 0.00010503; spectral max MAE was CPU 0.008558 and Metal 0.008534. Bump Normal max MAE was CPU 0.00077502 and Metal 0.00078087. All values were finite, without rendering errors. Five comparison sheets were inspected for orientation, gradients, tiling, linked transforms, alpha and normal boundaries. E37 passed 22/22. Evidence: workspace `test-scenes/validation-2026-10-09/cycles-scene-goal-phase11/`.

These tests cover the adapter's flat image-coordinate consumer. They do not establish all image projections, filtering modes, UDIM/time/image sources, surface derivatives or production scenes. No integrator or spectral quality defaults are lowered by this change.
