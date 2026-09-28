# Metal command-buffer batching (deferred commit)

`MetalDevice::EnqueueKernel()` used to create, retain, track and commit a
full `MTLCommandBuffer` + `MTLComputeCommandEncoder` for **every** kernel
dispatch. Kernels-heavy loops (dense PATHOCL dispatches ~15 kernels per
iteration, image pipelines, merge passes) paid the alloc/commit/track cycle
per dispatch and lost all per-command-buffer amortization.

## Design

`MetalDevice` keeps one shared *pending* command buffer
(`pendingCB`, `pendingBuffers`, `pendingEncoderCount`, all under
`inFlightMutex`):

- `EnqueueKernel()` encodes its compute encoder into `pendingCB`, appends
  the dispatch's buffers to `pendingBuffers`, and returns. The batch is
  committed when the encoder count hits **64** or any sync point below runs.
- `CommitPendingLocked()` moves `pendingCB` into `inFlightWork` (same
  ownership protocol as before: the CB is retained at creation, released by
  `FinishQueue` after `waitUntilCompleted`) and commits it.
- `FlushQueue()` commits without waiting (matches OpenCL flush semantics
  and starts the GPU earlier).
- `FinishQueue()` commits the batch first, then waits on all
  `inFlightWork` — pending encoders are covered by the wait.
- `EnqueueWriteBuffer()`'s conflict scan checks `pendingBuffers` in
  addition to `inFlightWork.buffers`: a host `memcpy` into a buffer that
  uncommitted encoders still reference would race with their reads.
- `AllocBuffer()` same-size `src` copy path got the same pendingBuffers
  scan; the realloc/free paths already call `FinishQueue()`.
- `CommitAndTrackInFlight()` (used by the native HWRT path in
  `metalrtaccel.mm`) commits the pending batch **before** tracking the
  externally encoded command buffer, preserving submission order.

## Ordering argument

An OpenCL queue is strictly in-order. The batched Metal path is equivalent:

- Encoders within one command buffer serialize in encode order via Metal's
  automatic per-command-buffer hazard tracking on declared resource
  usages (`setBuffer`/`useResource` calls already declare them).
- Every path where the host observes or mutates buffer contents commits
  pending work first (read/write-conflict/finish/alloc/free/dtor).
- No caller can observe a committed-but-untracked or tracked-but-
  uncommitted CB: commit and `inFlightWork` push stay atomic under
  `inFlightMutex`.

## Measured

`prism-conservatory` 480x270 quick bench, PATHOCL, 21 s walltime:

| build | spp | Msamples/s |
|-------|-----|------------|
| per-dispatch CB | 283 | 1.85 |
| batched (2 runs) | 282 / 294 | 1.84 / 1.91 |

Neutral on this GPU-bound workload (kernels are ms-scale; dispatch cost was
already amortized). The win is CPU-side allocation/commit overhead on
dispatch-bound loops and the groundwork for further batching (e.g.
auto-fencing by buffer hazard sets instead of fixed 64-encoder cap).
e52 adaptive-noise regression: 5/5 PASS on the batched build.
