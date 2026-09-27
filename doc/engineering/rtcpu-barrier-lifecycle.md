# RTPATHCPU thread barrier lifecycle — deadlock analysis & fix

> Debugging note for SuperLuxCore. Incident: Blender 5.2.1 / Apple
> Silicon hang report (incident B54B0E10), main thread parked 53 s in
> `RenderSessionImpl::Pause` -> `RTPathCPURenderEngine::PauseThreads`
> -> `std::barrier::arrive_and_wait` -> `__ulock_wait`.

## The protocol

`RTPathCPURenderEngine` synchronizes pause/edit with a single
`std::barrier` of `renderThreads.size() + 1` participants
(`rtpathcpu.cpp`). Every paused loop iteration runs two `arrive_and_wait`
calls back to back: the first rendezvous with `PauseThreads()`, the
second parks the thread until `ResumeThreads()` arrives.

Two invariants keep this alive:

1. Exactly **one control thread** drives the pair, serialized.
2. All N render threads are **alive and looping** when the control
   thread arrives — `std::barrier` is phase-blind, any missing arrival
   blocks the phase forever.

## Deadlock modes found

### A. Barrier op on a stopped engine (the reported hang)

`RenderEngine::Pause()` takes **no lock** and had no `started` guard.
After `StopLockLess()` joins+resets every `JThread`, a `Pause()` still
reached `arrive_and_wait()` and waited for N thread arrivals that can
never happen — a permanent `__ulock_wait` on the UI thread.

Blender-side trigger: `engine/viewport.py` `view_draw()` snapshots
`session = engine.session`, then calls `session.Pause()` under
`session_lock` (`_locked_session_call`). The async `SessionWorker` can
stop that same session first (`_do_stop`/`_do_config`/`_do_start` →
`_stop_session` → `session.Stop()`) and publish a new one — the Python
lock serializes the two calls but does not protect the **stale object**.
The "render threads still running" in the hang stackshot belong to the
*new* session; the main thread was waiting on the *old* engine's
barrier.

Repro (old build hangs forever at `arrive_and_wait`):

```python
session.Start(); session.Stop(); session.Pause()
```

### B. Stop while threads are edit-parked

`BeginSceneEditLockLess`/`BeginFilmEdit` park threads at the barrier
**without** setting `pauseMode`. `StopLockLess` only released the
barrier when `pauseMode` was set, so `session.Stop()` mid-scene-edit
parked `join()` forever (N threads waiting at the resume phase, no
control arrival ever coming).

Repro (old build hangs in join):

```python
session.Start(); session.BeginSceneEdit(); session.Stop()
```

### C. Null deref on stale edits

`EndSceneEditLockLess`/`EndFilmEdit` dereference `samplerSharedData`,
which `StopLockLess` releases — a stale `Parse()`/`EndSceneEdit` on a
stopped engine segfaulted instead of no-op'ing.

## The fix (rtpathcpu.cpp)

- `PauseThreads()`/`ResumeThreads()` now take `engineMutex` (recursive)
  and return early when `!started`. This serializes **all** barrier
  phase participation against `StopLockLess` (which runs under the same
  mutex) and turns stale-handle calls into no-ops. Render threads never
  take `engineMutex`, so holding it across `arrive_and_wait()` cannot
  deadlock them.
- `StopLockLess()` releases the barrier when `pauseMode || threadsPauseMode`
  (edit-parked threads) — under the mutex, `threadsPauseMode == true`
  provably means the pause phase already completed and threads sit at
  the resume phase, so one arrival unblocks them.
- `EndSceneEditLockLess`/`EndFilmEdit` early-return on `!started`
  (`samplerSharedData` is gone); `EndFilmEdit` still swaps `film`/
  `filmMutex` so the engine's pointers stay valid.
- `WaitNewFrame()` skips the first-frame wait on a stopped engine.

`RenderEngine::Start`'s catch-block comment already documented this bug
class for RTPATHOCL ("a barrier-synchronized engine would otherwise
wait forever inside StopLockLess"); the OCL engine has the equivalent
`syncThreadsRunning` guard. The CPU engine simply never got one.

## Tests

`dev-tools/rtcpu_pause_deadlock_test.py` (workspace): stale Pause/Resume,
edit-parked Stop, stale film-resize Parse, normal pause sanity. On the
pre-fix build A and B deadlock (verified, >40 s hang); on the fixed
build all return in <1 ms.
