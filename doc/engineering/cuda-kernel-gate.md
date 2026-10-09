# CUDA kernel compile gate

## What broke in 2.11.21

The Windows/Linux CUDA backend could not render anything: every PathOCL
session failed with `PathOCL kernel CUDA program compilation error`.
The new Cycles-compat kernel code used OpenCL C idioms that OpenCL and the
Metal `cl2msl` pass accept but the CUDA OpenCL-emulation layer
(`include/luxrays/devices/cudadevice_*.cl`) does not:

| Idiom | Where | CUDA fix |
|---|---|---|
| `(float2)(a, b)`, `(float3)(...)`, `(float4)(...)` vector literals | `mapping_funcs.cl` DirMapping2D, `texture_funcs_evalops.cl` object/generated coordinates | `MAKE_FLOAT2/3/4()` |
| `float3 v = <float>` (implicit scalar to vector) | `materialdefs_funcs_openpbr.cl` fuzz lobe | `MAKE_FLOAT3(f, f, f)` |
| `any(v1 != v2)` on vectors | `materialdefs_funcs_openpbr.cl` terminator | per-component compare |
| `sign()` | `texture_funcs_evalops.cl` Cycles bump | added to `cudadevice_oclemul_funcs.cl` |

Also fixed while there: the CUDA `vstore4()` emulation wrote `w` into
`p[offset + 2]` and never wrote `p[offset + 3]` (used by `VSTORE4F`).

Nothing caught it because development and validation happen on macOS
(CPU/Metal) and the CI runners have no NVIDIA GPU, so the CUDA path was
never compiled before release.

## The gate

`dev-tools/cuda_kernel_check.py` compiles every hardware program exactly as
`CUDADevice` assembles it - no GPU needed, NVRTC is only a compiler library:

1. Parses each `CompileProgram()` call site under `src/` for its ordered
   `KernelSource_*` list (the PathOCL list comes from
   `PathOCLBaseOCLRenderThread::GetKernelSources()`), so new kernel files are
   picked up automatically.
2. Prepends the CUDA prelude (`cudadevice_oclemul_types`, `cudadevice_math`,
   `cudadevice_oclemul_funcs`) like `CUDADevice::GetKernelSource()`.
3. Compiles with the `cudaKernelCache::ForcedCompilePTX()` options plus the
   `GetKernelParamters()` defines. PathOCL is compiled in four variants that
   together cover PATHOCL/TILEPATHOCL/RTPATHOCL, RGB/spectral, shadow
   terminator modes 0/1/2 and wavefront queues on/off (`--full` = all 36).

The assembled PathOCL source was verified byte-identical to the source the
2.11.21 engine dumps with `LUX_DUMP_KERNEL_SRC`, and the checker reproduces
the exact 10 NVRTC errors users hit.

Where it runs:

- **Engine** `wheel-builder.yml`: job `cuda-kernel-check`; `build-wheels`
  depends on it, so no wheel (and no `wheels-latest`) is produced from a
  commit whose CUDA kernels do not compile.
- **Add-on** `build_bundle.yml` (release, latest and PR bundles): resolves the
  engine commit behind the wheels tag, compiles its kernels with the
  hash-pinned NVRTC wheel that the zips ship, and refuses to bundle otherwise.

## On a Mac

NVRTC has no macOS build. Run the static lint (under a second; catches vector
literals, `any/all` of vector comparisons and OpenCL-only builtins that the
CUDA prelude lacks):

    python3 dev-tools/cuda_kernel_check.py --lint

Install it as a pre-push hook once per clone:

    git config core.hooksPath dev-tools/git-hooks

The lint cannot see type errors such as scalar to vector assignment; the
NVRTC job in CI is the authoritative gate. For the full check locally, any
x86-64 Linux container works (slow under emulation on Apple Silicon):

    docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src python:3.13 \
      sh -c "pip install -q nvidia-cuda-nvrtc-cu12==12.9.86 && python dev-tools/cuda_kernel_check.py"

## Writing kernel code that works on every backend

- Build vectors with `MAKE_FLOAT2/3/4()` or `TO_FLOAT3(x)`, never `(floatN)(...)`.
- Never assign a scalar to a vector.
- Compare vectors per component; `any()`/`all()` only take `intN` on CUDA.
- If you need an OpenCL builtin CUDA lacks, add it to
  `cudadevice_oclemul_funcs.cl` (and check `cl2msl.py` for Metal).
