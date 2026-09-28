# SSP tail — implementation notes

## Invariants

- `SspTail` is a **thread-local** record (`PathTracerThreadState::sspTail`,
  or a local in `RTRenderFunc`). It is never shared between threads on CPU
  - no synchronization needed. Reset happens in `RenderEyeSample` only;
  consecutive light samples of the same thread reuse the last eye tail.
- Recorded order is **eye order** (vtx[0] nearest camera). The solver
  wants light->lens order, so `LMNEETailConnectToEye` walks the record
  backwards: `chain[i] = vtx[specN-1-i]`.
- Anchors are positions+normals only - never BSDFs. Consumption
  reprojects (`MneeReproject`, gap = 0.5 * max(1e-5, 1e-4*dist)) and
  re-validates objectID + `MneeChainVertexInit`, so stale or cross-object
  records degrade to a wasted reproject, never to a wrong path.
- The tail is only consulted **after** `LMNEEConnectToEye` fails - a
  solved single-vertex path must not also run the tail (double count).
- `LMneeChainSolveAndEval` (pathtracer_mnee.cpp) is the shared
  solve/validate/geometric-term/splat-prep tail of
  `LMNEEMultiConnectToEye` and `LMNEETailConnectToEye`. Splat and
  LightFocusCredit stay in the members (they are the only part needing
  member access).

## Debug

- `LUX_SSP_DBG=1` prints one `SSP_GATE` line per blocked connect reaching
  the tail gate (specN, overflow, blocker objectID, recorded IDs).
- `LUX_LMNEE_REJ=1` already prints `LMNEE_ACC tail` / `LMNEE_REJ tail-*`
  stages (reproj, stale, init, then the shared ms-* rejects).

## GPU port notes (Phase 3)

- Buffer: `sspTailsBuff`, per-eye-task, allocated when
  `path.ssp.enable && path.mnee.enable` (same gating as tasksMneeBuff).
- Recorder: `MK_GENERATE_NEXT_VERTEX_RAY` after `EyePathInfo_AddVertex`;
  reset in `GenerateEyePath`. Pass-through marker is `cosSampledDir < 0`.
- Consumer: `MK_LIGHT_VERTEX` blocked branch, before `LMneeChain_Start`;
  add `MNEE_PHASE_TAIL_LOAD` to reproject one anchor per launch, then
  reuse `MneeChain_WriteJacStart`/`LMneeChain_SolveEnd` unchanged.
- Pairing: tails are pixel-unrelated - `pairSlot = lightIndex %
  eyeTaskCount`. Seqlock `version` field (see GPU mirror in
  pathinfo_types.cl) protects the shared array.
