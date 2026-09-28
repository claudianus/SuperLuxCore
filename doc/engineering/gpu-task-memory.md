# GPU per-task memory diet: GPUTaskMnee split

`GPUTaskDirectLight` (one slot per render task, `taskCount` entries per
OCL thread) used to inline the whole MNEE solver state:

| struct | bytes |
|--------|-------|
| `BSDF` | 416 |
| `MneeState` | 1012 |
| `GPUTaskDirectLight` (old) | 1928 |
| `GPUTaskDirectLight` (new) | 76 |
| `GPUTaskMnee` | 1848 |

`BSDF mneeBsdf + mneeBsdfFinal + MneeState` were **96%** of the struct and
are only ever read when `path.mnee.enable = 1`. At the default
`opencl.task.count = 65536` that was ~115MB of device memory per thread
allocated unconditionally (and wasted on every non-MNEE render, which is
the common case - MNEE is opt-in).

## Change

- `GPUTaskDirectLight` keeps only `illumInfo`, `seedPassThroughEvent`,
  `directLightResult`, `throughShadowTransparency` (76B).
- New `GPUTaskMnee { BSDF mneeBsdf, mneeBsdfFinal; MneeState mnee; }`
  lives in a dedicated `tasksMnee` buffer, allocated only when
  `threadTaskConfig.pathTracer.mnee.enabled`.
- `mneeSeeds` (seed cache, 16K x 32B) got the same gate - it was
  unconditionally allocated before.
- `tasksMnee` is an extra `KERNEL_ARGS` slot right after
  `tasksDirectLight`; the 15 Mnee/LMnee chain functions take an added
  `__global GPUTaskMnee *taskMnee` param; the 4 kernels that reach them
  (`MK_RT_DL`, `MK_DL_SAMPLE_BSDF`->`MK_MNEE_NEXT_VERTEX`,
  `MK_LIGHT_VERTEX`) pass `&tasksMnee[gid]` / a local.
- NULL is a legal buffer arg: the kernel slot is still bound (arg
  indices must not shift) and the MNEE state machine never dereferences
  it because `Mnee_Start` gates on `taskConfig->pathTracer.mnee.enabled`
  before touching `taskMnee`, and `lpi->mneeActive` is only ever set by
  an enabled solve.

## Verified

- e17 MNEE seed-cache suite (OpenCL + Metal devices): ALL PASS,
  including the "MNEE actually fires" caustic-activity check
  (mnee=on caustic mean 0.27 vs off 0.0) - the solver reaches the new
  buffer on both device backends.
- mnee=off log shows `GPUTaskDirectLight` 608KB @ 8192 tasks and no
  `GPUTaskMnee`/`MneeSeeds` allocation; mnee=on shows
  `GPUTaskMnee` 14.4MB + `MneeSeeds` 640KB.
- e52 adaptive-noise suite: 5/5 PASS.
- prism-conservatory PATHOCL quick bench: 1.80 Ms/s (parity).

## Note

Task-buffer structs are the single biggest unconditional memory line
item after film; any future per-task feature blob should follow the
same pattern (separate buffer + enable-gated alloc + extra KERNEL_ARGS
slot) rather than growing `GPUTask`/`GPUTaskDirectLight`.
