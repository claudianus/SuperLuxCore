# Light BVH strategy

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Light BVH strategy (`lightstrategy.type = LIGHT_BVH`)

E&K'18 ("Importance Sampling of Many Lights with Adaptive Tree
Splitting", ACM TOG 37(4) — the technique behind Cycles' light tree).
`LightStrategyLightBVH` (`src/slg/lights/strategies/lightbvh.cpp`)
derives from `LightStrategyLogPower`: TASK_ILLUMINATE builds a
binned-SAH binary tree over direct-sampling-enabled lights; TASK_EMIT/
TASK_INFINITE_ONLY keep the flat log-power table, which also serves as
the device fallback.

- Node = `slg::ocl::LightBVHNode` (56B, `lightbvh_types.cl`): implicit
  layout, root at 0, left subtree in `[i+1, rightChildIndex)`. Per node:
  bbox, emission bounding cone (axis + thetaO), `energyFlat` (infinite/
  directional leaves — bypasses spatial bounds) + `energyLocal`.
- Importance bound at receiver: `I = E_flat + E_local/max(d^2,dmin^2)
  * cosSurf * cosOrient` — CPU `NodeImportance()` and GPU
  `LightBVH_NodeImportance` (`lightbvh_funcs.cl`) are bit-parity twins;
  keep them in sync.
- `SampleLightPdf` replays the root-to-leaf path via the
  `lightToLeaf[lightSceneIndex]` table — `lightSceneIndex` ==
  `lights[]` order == `lightDefs[]` order, so the same index works on
  both backends.
- GPU dispatch is buffer-presence driven (`if (lightBVHNodes)` before
  the DLSC check in `lightstrategy_funcs.cl`) — the call sites pass
  `NULL` when `lastOnlyInfiniteLights`/`onlyInfLights` is set, same as
  `dlscAllEntries`.
- Device buffers: `lightBVHNodesBuff` + `lightBVHLightToLeafBuff`
  (CompiledScene `lightBVHNodes`/`lightBVHLightToLeaf`
  SpillableArrays, uploaded in `InitLights`, bound in
  `SetAdvancePathsKernelArgs` right after the dlsc args — `KERNEL_ARGS`
  and `LIGHTS_PARAM_DECL` order must match).
- Regression: `dev-tools/e26_lightbvh_test.py` (unbiasedness vs
  LOG_POWER ref, same-spp RMSE bound, CPU/GPU parity, finiteness).
- clspv probe recipe in git history: cat the .cl files in
  `GetKernelSources()` order + `-D LUXRAYS_OPENCL_KERNEL
  -D LUXRAYS_OPENCL_DEVICE -D SLG_OPENCL_KERNEL -D RENDER_ENGINE_PATHOCL`.
  `Init` kernel reports a pre-existing POD-arg layout error on clspv —
  unrelated to light params (it takes none).

