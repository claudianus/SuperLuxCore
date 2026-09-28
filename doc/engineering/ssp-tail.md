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

## GPU port notes (Phase 3, landed)

- Buffer: `sspTailsBuff`, `eyeTaskCount * sizeof(SspTail)` (~300B/task),
  allocated only when `pathTracer.mnee.sspEnable` (i.e.
  `path.ssp.enable && path.mnee.enable`). CL mirror lives in
  `pathoclbase_datatypes.cl` (component floats - host-compiled file).
- Recorder: `SspTail_RecordVertex` in `MK_GENERATE_NEXT_VERTEX_RAY`
  right after `EyePathInfo_AddVertex`, gated on a non-null buffer.
  **Reset trick**: the `depth == 1` call resets the record, which covers
  every engine's path-init path (PATHOCL / TILEPATHOCL / RTPATHOCL)
  without touching `GenerateEyePath`.
- Consumer: `MK_LIGHT_VERTEX` blocked branch tries
  `LMneeChain_StartTail` between `LMnee_Start` and `LMneeChain_Start`.
  Pairing is `sspTails[lightIndex]` (light task lt reads eye task lt's
  record; guarded `lightIndex < eyeTaskCount`).
- **No dedicated TAIL_LOAD phase**: `LMneeChain_StartTail` sets
  `mnee->useTail` and aims the first MS_DISCOVER ray at the farthest
  anchor. Inside MS_DISCOVER each aimed hit is objectID-validated
  (`vtx[chainMaxV-chainN]`); a mismatch just clears `useTail` and the walk
  degrades to plain `MneeChain_WalkDir` discovery - the traced vertices
  stay physically valid, so the fallback is free. `chainMaxV = specN`
  (the StartTail-time snapshot) makes the regular vertex bound hand the
  loaded chain to the Newton solve unchanged.
- **Snapshot indexing gotcha**: vtx[] indices must come from
  `chainMaxV` (the specN snapshot), never a live `sspTail->specN` read.
  The paired eye task rewrites the record between launches - a new path
  resets specN to 0 - so a fresh read can shrink below `chainN` and
  underflow `specN - chainN` into an out-of-bounds vtx[] access (seen in
  review; fixed before merge). Content staleness remains guarded by the
  per-vertex objectID match, which only ever costs steering quality.
- No seqlock needed: producer (MK_GENERATE_NEXT_VERTEX_RAY) and
  consumer (MK_LIGHT_VERTEX) are separate serialized kernel launches;
  a record is only ever written/queried across launch boundaries.
- Kernel args: `sspTails` is appended to MK_GENERATE_NEXT_VERTEX_RAY
  (write) and MK_LIGHT_VERTEX (read, after the PGIC_DEPOSIT tail) -
  NOT added to shared KERNEL_ARGS (Apple translator arg limit).
