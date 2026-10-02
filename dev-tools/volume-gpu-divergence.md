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
