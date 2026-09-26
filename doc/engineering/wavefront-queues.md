# Wavefront task queues

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Wavefront queues (LUXRAYS_WAVEFRONT_QUEUES=1)

Per-state task queues opt-in for PATHOCL (dense stays default).
Iteration: `BucketHistogram` (per-state x 3-lambda-bin counts) ->
`QueuePrefix` (device exclusive prefix -> `taskQueueBase` cursors +
`taskQueueTotals` + re-zeroes counts) -> `BuildQueues` (atomic-append
task indices) -> one kernel launch per non-empty state, guarded by
`WAVEFRONT_GUARD` against `taskQueueTotals[state]`.

Measured on cornell 720x720 PATHOCL Metal (30 s): dense ~9.5M
samples/s vs wavefront ~24M — wavefront wins ~2.5x from COMPACT
launches (dense launches every state kernel at taskCount and
early-outs internally; wavefront launches only queued lanes).

- The totals are read back once per iteration (72 B) to size the
  launches. Do NOT replace this with stale/async sizing: measured a
  ~6x regression when totals lagged by a resync period — tasks landing
  in a state's queue tail beyond the stale launch size sit unexecuted
  until the next resync (the guard protects lanes, not progress).
- Metal/Vulkan `EnqueueReadBuffer` ignores the blocking flag and calls
  `FinishQueue()` (shared storage needs a drained queue before the
  host memcpy) — every host read/write on those backends is a queue
  drain, so batch them; the QueuePrefix device-side prefix removed the
  old read-histogram + host-prefix + upload-bases round trip (2 syncs
  -> 1).
- `dev-tools/wavefront-regression.sh` needs `pipefail` (the
  compare|tee pipe swallowed the comparison exit code — printed a
  0.2556 reldiff vs tol 0.2 as PASS once) and requires WFDBG records
  to exist (an absent debug log would vacuously pass the oob/dup/
  badState/badLambda==0 check).

