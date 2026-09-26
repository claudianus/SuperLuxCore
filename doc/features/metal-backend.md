# Metal backend — Apple-native GPU path tracing + HWRT

Status: implemented. A full Metal compute backend for LuxCore on Apple
silicon, including native hardware ray tracing via `MTLAccelerationStructure`.

This is the largest fork feature. The detailed design (measured evidence,
architecture, cl2msl translation rules) lives in
`doc/metal_backend_design.md`; this page summarizes the delivered state.

## What and why

Apple GPUs have no CUDA and only deprecated OpenCL. A Metal backend is the
only way to run LuxCore at full speed on modern Apple hardware — and Metal's
`MTLAccelerationStructure` provides dedicated ray-tracing silicon that
outperforms software BVH traversal as scene size grows.

## Components

| Layer | Commits | What |
|---|---|---|
| Device | `register DEVICE_TYPE_METAL_GPU`, `Metal device layer in luxrays`, `backend exposure` | `luxrays` enumerates the Apple GPU as `METAL_GPU`; device/context/kernel marshalling. |
| Compiler | `cl2msl` rewriter, `M2c/M3`, `full 139-file kernel set` | The OpenCL `.cl` kernel sources are translated to Metal Shading Language and compiled into a Metal library. |
| Pipeline | `full engine render E2E`, `GPU-resident intersection complete` | Camera→intersect→BSDF→light→film all execute on GPU. |
| HWRT | `native MTLAccelerationStructure intersection path (E0)`, `instance + motion blur via MBVH`, `instance-only AS refit`, `hardware image pipeline` | Scene geometry uses hardware ray tracing; instances + motion-blur descriptors; AS refit for animated instances. |
| Fixes | `thread-safety + scalar marshalling`, `order host buffer writes`, `AGX compiler crash workaround`, `compiled-scene buffer teardown` | Correctness + stability. |

## References

- Apple. **Metal Shading Language Specification** / **Metal Performance
  Shaders — Ray Tracing (`MTLAccelerationStructure`)**. Apple developer docs.
- `doc/metal_backend_design.md` — in-tree design doc with measured M5 Pro
  evidence (ray rates, HWRT vs software scaling).

## Validation

- Full engine renders (luxball, Cornell, TILEPATHOCL) on Apple M5 Pro.
- HWRT intersection + motion blur + instance refit verified.
- CPU↔GPU parity checked per feature (e.g. blackbody/whitenoise bit-exact).
- `dev-tools/e20_metal_imagemap_test.py` — HALF/FLOAT image-map
  infinite-light parity vs OpenCL + PATHCPU (regresses the
  `vload_half`/`vloadn`/`vstoren` shims ignoring their element offset,
  which made HALF image maps read as a constant first texel on Metal).

## Platforms

**Apple silicon only** (Metal). CPU and OpenCL paths remain the portable
fallbacks on other OS/hardware.
