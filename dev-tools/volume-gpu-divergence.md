# GPU volume-shell divergence (unresolved, 2026-10-03)

Observed on `dense-volume` (pre-fix, `glass` shell) and `portal-interior`
at PATHOCL/METAL_GPU: heterogeneous+glass-shell volume renders nearly
black on GPU while CPU shows proper in-scattering. Not a convergence
issue: 256spp still dark on GPU (mean ratio ~0.5-0.7, sparse fireflies).

Confirmed working on GPU:
- heterogeneous + null shell + camera inside (dv-hetnull: lit 70% == CPU 69%)
- homogeneous + null shell (dv-homnull)
- minimal homogeneous + glass shell + matched IOR (minvol: GPU 119 == CPU 124)
- minimal glass shell + default 1.5 ior + camera inside (mv15: GPU 119)

Failing on GPU (all at scene/gauntlet scale):
- heterogeneous + glass shell (dv-noext, dv-homog3)
- glass shell + no explicit exterior (dv-noprio)
- glass shell + pinned camera.volume (dv-cv2, autovolume off)
- archglass shell WORKS on GPU (dv-arch 63.9 ~ CPU 54)

Hypothesis: on GPU, when a BSDF-shading surface (glass) marks a volume
boundary, `PathVolumeInfo_Update`/`SetHitPointVolumes` resolve
`currentVolumeIndex` differently than CPU - a subtle ordering/priority
case. `archglass` uses the same hit plumbing but a different event path.
Next probe: printf-instrument `Scene_Intersect` `rayVolumeIndex` for the
first hit of a camera ray inside a glass-shelled box; compare to CPU.

## Additional data (2026-10-03, trace continued)

- `archglass` and `roughglass` (uroughness=0.001) shells both render
  fog correctly on GPU; only `glass` (delta specular transmit + reflect
  choice) diverges.
- `null` shell with explicit `vol_air` exterior volume also works on GPU
  (dv-nullext: lit% 68.4 ~ CPU 52.4, comparable beam).
- Strong absorption (sigma_a=50) is equally dark on GPU and CPU,
  confirming Volume_Scatter runs and transmittance is applied. The
  divergence is specific to *scattering contribution* under a `glass`
  delta-transmit shell - likely a stochastic difference in
  `PathVolumeInfo_Update` timing (the glass transmit branch orders
  `PathDepthInfo_IncDepths`/`PathVolumeInfo_Update` differently from
  CPU's PathInfo::AddVertex) or `passThroughEvent`-derived seed state
  consumed differently after a delta boundary.
- `BSDF_GetMaterialInteriorVolume` resolves via evalOp; `glass`
  material's `evalGetInteriorVolumeOpLength` may be >1 even when
  static (has both interior AND exterior volume evals in one op
  program). Worth checking whether the op result consumes a stale
  `passThroughEvent` on the eval stack on GPU.

## Channel isolation (mhsun.scn minimal repro)

160x120 heterogeneous fbm volume, glass shell, camera on the face
plane (half the frame inside the box), sun light only:

- lighttracing + MNEE disabled: GPU == CPU (51.1% lit both) - eye
  path transport through the delta boundary is correct.
- lighttracing enabled (0.5 task fraction, MNEE off): GPU 74.3% lit,
  CPU 99.4% - the divergence is in the GPU light-tracing connect
  channel only.
- Volume self-emission through the same glass shell: GPU == CPU -
  `Volume_Scatter` + `PathVolumeInfo_Update` at the boundary work
  correctly for eye paths.

Isolated root cause hypothesis: in
`pathoclbase_kernels_micro.cl` Stage A (line ~3391) the
light-to-camera visibility ray is traced with
`Scene_Intersect(LIGHT_RAY | CAMERA_RAY | SHADOW_RAY)`. For a glass
shell `BSDF_GetPassThroughShadowTransparency` is BLACK (parsematerials
default 0), so the connect ray terminates at the glass face on the
*first* hit. On the CPU side `TraceLightPath`'s connect does the same,
so symmetric-blocking is consistent - but the *splat* conditional on
line 3415 (`visRayHit->meshIndex == NULL_INDEX`) then drops the
contribution differently. CPU `TraceLightPath` applies splat on the
*same* condition but carries `pathThroughput` through the boundary
via a different accounting path. The GPU's `pendingSplat.radiance*`
was already committed before the boundary was consumed by the
`PathVolumeInfo_Update` inside the marching loop, so a shadowed
transmission term is applied one march too late.
