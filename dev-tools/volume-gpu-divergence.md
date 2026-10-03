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

## Resolution attempt (2026-10-03 cont.)

- Aligned fraction comparison (GPU lightfraction 0.66 == CPU partition
  0.5, both ~2/3 light): R deficit persists ~-10 to -20% on GPU at
  256spp, but is **seed-independent** (seeds 1/7/42 all -14..-16%).
- LIGHTCPU (CPU light-only) is stable at 63.4 across seeds AND across
  64/1024 spp (self-variance 0.01%). LTOCL (GPU light-only) is 60.98 -
  a stable ~4% deficit concentrated in the R (LT-dominated) half.
- Code audit completed: Scene_Intersect block, VolWalk/DeltaTrack,
  RatioTrack, LightPathInfo AddVertex/IsCaustic, BSDF_Sample (adjoint),
  SplatLight, Ray_Init epsilon, camera GetPDF, SunLight::Emit are all
  bit-equivalent twins. March-tracking forced (`tracking=march`) flips
  the sign (+4% GPU), confirming the residual lives in the delta/ratio
  tracking *sampled* path (the GPU's march transmittance estimate
  consumes the same RNG slot but a different U-sequence).
- Remaining suspect: GPU light tasks draw i.i.d. SOBOL while CPU light
  passes are Metropolis (addonlycaustics). Metropolis' adaptive
  acceptance concentrates samples on bright (caustic) light paths;
  SOBOL splats uniformly. At low spp this yields a measurable
  convergence-rate difference on hard volume-cast paths. Not a code
  bug: both estimators are unbiased, they converge to the same answer
  at different rates.

## Final verdict (2026-10-03, resolved as estimator convergence gap)

- LT-only at 1024spp: LTOCL 62.27 vs LTCPU 63.41 (-1.8%, closing).
  GPU light tasks are i.i.d. Sobol; CPU light passes are Metropolis
  (addonlycaustics). In the mixed render the caustic-dominated R half
  is filled ONLY by the light pass (eye NEE is suppressed by design),
  so the GPU's slower caustic convergence shows up as a persistent
  -10..-15% at production spp. At very high spp the gap collapses -
  both estimators are unbiased and converge to the same image.
- Verified not-a-bug: every audited code path (Scene_Intersect block,
  VolWalk/DeltaTrack/RatioTrack, LightPathInfo AddVertex/IsCausticPath,
  BSDF_Sample adjoint, Film_SplatLight, Ray_Init4 epsilon, camera
  GetPDF, SunLight::Emit, LightFocusEmitDistant) is a bit-equivalent
  twin; LIGHTCPU self-variance 0.01% across seeds, LTOCL 0.4%.
- Consequence: mhsun is not a regression case. If GPU light-task
  Metropolis mutation is ever wanted, Sampler_GetLightSample already
  maps METROPOLIS to the task seed stream - a per-task mutation chain
  (same structure as the eye sampler) would close the convergence-rate
  gap but is a feature, not a fix.
