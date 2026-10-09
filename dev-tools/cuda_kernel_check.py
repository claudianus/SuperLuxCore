#!/usr/bin/env python3
"""CUDA kernel compile gate - no NVIDIA GPU required.

Day-to-day development happens on macOS (CPU/Metal), where the CUDA backend is
never exercised: Metal's cl2msl pass and OpenCL both accept idioms that the
CUDA emulation layer rejects (`(float2)(a, b)` vector literals, scalar->vector
assignment, `any(v1 != v2)`, missing builtins such as `sign()`...). v2.11.21
shipped a Windows/Linux build whose PathOCL kernel did not compile at all.

This script rebuilds every hardware program exactly as the engine assembles it
for a CUDA device - the KernelSource_* list is parsed from the C++ call sites,
prefixed with the CUDA OpenCL-emulation headers, plus the device's -D options -
and compiles it with NVRTC. NVRTC is a pure compiler library, so this runs on
GPU-less CI runners.

    pip install nvidia-cuda-nvrtc-cu12==12.9.86
    python dev-tools/cuda_kernel_check.py            # full gate (Linux/Windows)
    python dev-tools/cuda_kernel_check.py --lint     # static lint, also on macOS

NVRTC has no macOS build: on a Mac run --lint locally (fast, catches the known
pitfalls) and rely on the `cuda-kernel-check` CI job, which blocks wheels.
"""

import argparse
import concurrent.futures
import ctypes
import glob
import os
import re
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CUDA_PRELUDE = [
    "cudadevice_oclemul_types",
    "cudadevice_math",
    "cudadevice_oclemul_funcs",
]

# Mirrors cudaKernelCache::ForcedCompilePTX() (src/luxrays/utils/cuda.cpp),
# minus --pch/--split-compile which only affect build speed.
NVRTC_BASE_OPTS = [
    "--device-as-default-execution-space",
    "-Xcudafe", "--display_error_number",
    "-Xcudafe", "--diag_suppress=550",
    "-Xcudafe", "--diag_suppress=1055",
    "-Xcudafe", "--diag_suppress=68",
]

# Mirrors CUDADevice::AddKernelOpts() plus the --use_fast_math that
# PathOCLBaseEngine/Film set as additional compile options for CUDA devices.
CUDA_DEVICE_OPTS = ["-D LUXRAYS_CUDA_DEVICE", "-D LUXRAYS_OS_LINUX", "--use_fast_math"]

# Spectral::KernelDefines(): only the names matter for compilation.
SPECTRAL_DEFINES = ["-D SLG_SPECTRAL"] + [
    f"-D {n}=1.0f" for n in (
        "SLG_SPECTRAL_PROJ_KR", "SLG_SPECTRAL_PROJ_KG", "SLG_SPECTRAL_PROJ_KB",
        *(f"SLG_SPECTRAL_YREFL_{c}" for c in
          ("WHITE", "CYAN", "MAGENTA", "YELLOW", "RED", "GREEN", "BLUE")))]

EPSILON_OPTS = ["-D PARAM_RAY_EPSILON_MIN=1e-05f", "-D PARAM_RAY_EPSILON_MAX=0.1f"]


def pathocl_variants(full):
    """GetKernelParamters() variants. Every ifdef'ed feature that is selected
    at runtime must be covered by at least one variant."""
    def opts(engine, spectral, terminator, wavefront):
        o = ["-D LUXRAYS_OPENCL_KERNEL", "-D SLG_OPENCL_KERNEL",
             f"-D RENDER_ENGINE_{engine}", *EPSILON_OPTS,
             f"-D SLG_SHADOW_TERMINATOR_MODE={terminator}"]
        if spectral:
            o += SPECTRAL_DEFINES
        if wavefront:
            o.append("-D PATHOCL_WAVEFRONT_QUEUES")
        return o

    if full:
        out = []
        for engine in ("PATHOCL", "TILEPATHOCL", "RTPATHOCL"):
            for spectral in (False, True):
                for terminator in (0, 1, 2):
                    for wavefront in (False, True):
                        out.append((f"{engine}/spectral={int(spectral)}/term={terminator}/wf={int(wavefront)}",
                                    opts(engine, spectral, terminator, wavefront)))
        return out
    # Pairwise-ish cover: each value of each dimension appears at least once.
    return [
        ("PATHOCL/rgb/term=0", opts("PATHOCL", False, 0, False)),
        ("PATHOCL/spectral/term=1/wf", opts("PATHOCL", True, 1, True)),
        ("TILEPATHOCL/spectral/term=2", opts("TILEPATHOCL", True, 2, False)),
        ("RTPATHOCL/rgb/term=2/wf", opts("RTPATHOCL", False, 2, True)),
    ]


# Programs whose source starts with run-time generated #defines.
EXTRA_SOURCE_PREFIX = {
    "BVHKernel": (
        "#define BVH_VERTS_PAGE_COUNT 1\n#define BVH_NODES_PAGE_COUNT 1\n"
        "#define BVH_NODES_PAGE_SIZE 1024\n#define BVH_VERTS_PAGE0 1\n#define BVH_NODES_PAGE0 1\n"),
    "MBVHKernel": (
        "#define MBVH_VERTS_PAGE_COUNT 1\n#define MBVH_NODES_PAGE_COUNT 1\n"
        "#define MBVH_NODES_PAGE_SIZE 1024\n#define MBVH_VERTS_PAGE_SIZE 1024\n"
        "#define MBVH_VERTS_PAGE0 1\n#define MBVH_NODES_PAGE0 1\n"
        "#define MBVH_HAS_TRANSFORMATIONS 1\n#define MBVH_HAS_MOTIONSYSTEMS 1\n"
        "#define MBVH_HAS_VERTEXMOTION 1\n"),
}

# Need the OptiX SDK headers, which are not available on CI.
SKIP_PROGRAMS = {"OptixEmptyAccel"}

# ---------------------------------------------------------------------------
# Source discovery
# ---------------------------------------------------------------------------

KERNEL_TOKEN_RE = re.compile(r"\b(?:luxrays|slg)::ocl::KernelSource_(\w+)")
CALL_RE = re.compile(r"(?:->|\.)\s*CompileProgram\s*\(")
LITERAL_OPT_RE = re.compile(r'push_back\(\s*"(-D [^"]+)"\s*\)')
STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def index_cl_files(repo):
    index = {}
    for p in sorted((repo / "include").rglob("*.cl")):
        if p.stem in index:
            raise SystemExit(f"duplicate kernel file name {p.stem}: {index[p.stem]} / {p}")
        index[p.stem] = p
    return index


def _call_span(text, start):
    depth, i = 0, text.index("(", start)
    while True:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1


def discover_programs(repo):
    """[(programName, [kernel names], [literal -D opts], cppFile)] for every
    CompileProgram() call site in src/."""
    programs = []
    for cpp in sorted((repo / "src").rglob("*.cpp")):
        text = cpp.read_text(encoding="utf-8", errors="replace")
        prev_end = 0
        for m in CALL_RE.finditer(text):
            end = _call_span(text, m.start())
            window = text[prev_end:end]
            prev_end = end
            names = KERNEL_TOKEN_RE.findall(window)
            strings = STRING_RE.findall(text[m.end():end])
            if not names or not strings:
                continue
            programs.append((strings[-1], names, LITERAL_OPT_RE.findall(window),
                             cpp.relative_to(repo).as_posix()))
    return programs


def assemble(cl_index, names, prefix=""):
    missing = [n for n in names if n not in cl_index]
    if missing:
        raise SystemExit(f"kernel files not found for KernelSource_{missing}")
    return "".join(cl_index[n].read_text(encoding="utf-8") for n in CUDA_PRELUDE) + \
        prefix + "".join(cl_index[n].read_text(encoding="utf-8") for n in names)


# ---------------------------------------------------------------------------
# NVRTC
# ---------------------------------------------------------------------------

def find_nvrtc(explicit):
    cands = [explicit] if explicit else []
    cands += [os.environ.get("LUX_NVRTC_LIB")]
    try:
        import nvidia.cuda_nvrtc as pkg  # pip install nvidia-cuda-nvrtc-cu12
        base = Path(list(pkg.__path__)[0])
        cands += glob.glob(str(base / "lib" / "libnvrtc.so*"))
        cands += glob.glob(str(base / "bin" / "nvrtc64_*.dll"))
    except ImportError:
        pass
    for root in (os.environ.get("CUDA_PATH"), "/usr/local/cuda"):
        if root:
            cands += glob.glob(os.path.join(root, "lib64", "libnvrtc.so*"))
            cands += glob.glob(os.path.join(root, "bin", "nvrtc64_*.dll"))
    for c in cands:
        if c and os.path.isfile(c) and ".alt." not in os.path.basename(c):
            return c
    return None


class NVRTC:
    def __init__(self, path):
        libdir = Path(path).parent
        if sys.platform == "win32":
            # nvrtc64 loads nvrtc-builtins64 through the regular DLL search path
            os.add_dll_directory(str(libdir))
            os.environ["PATH"] = str(libdir) + os.pathsep + os.environ.get("PATH", "")
        else:
            # Same for libnvrtc-builtins: make it resolvable before dlopen
            for b in sorted(libdir.glob("libnvrtc-builtins.so*")):
                if ".alt." not in b.name:
                    ctypes.CDLL(str(b), mode=ctypes.RTLD_GLOBAL)
                    break
        self.lib = ctypes.CDLL(path)
        self.lib.nvrtcGetErrorString.restype = ctypes.c_char_p
        major, minor = ctypes.c_int(), ctypes.c_int()
        self.lib.nvrtcVersion(ctypes.byref(major), ctypes.byref(minor))
        self.version = (major.value, minor.value)

    def compile(self, source, name, opts):
        lib = self.lib
        prog = ctypes.c_void_p()
        rc = lib.nvrtcCreateProgram(ctypes.byref(prog), source.encode(), name.encode(), 0, None, None)
        if rc:
            raise RuntimeError(lib.nvrtcGetErrorString(rc).decode())
        try:
            enc = [o.encode() for o in opts]
            arr = (ctypes.c_char_p * len(enc))(*enc)
            rc = lib.nvrtcCompileProgram(prog, len(enc), arr)
            size = ctypes.c_size_t()
            lib.nvrtcGetProgramLogSize(prog, ctypes.byref(size))
            buf = ctypes.create_string_buffer(size.value)
            lib.nvrtcGetProgramLog(prog, buf)
            return rc == 0, buf.value.decode(errors="replace")
        finally:
            lib.nvrtcDestroyProgram(ctypes.byref(prog))


def errors_only(log):
    keep = [l for l in log.splitlines() if re.search(r"\berror\b", l)]
    return "\n".join(keep[:60]) or log[-4000:]


# ---------------------------------------------------------------------------
# Static lint (works everywhere, including macOS)
# ---------------------------------------------------------------------------

LINT_RULES = [
    (re.compile(r"\(\s*(?:u?int|float|u?char|u?short)[234]\s*\)\s*\("),
     "OpenCL vector literal `(floatN)(...)` does not compile on CUDA: use MAKE_FLOATn()/TO_FLOATn()"),
    (re.compile(r"\b(?:any|all)\s*\(\s*[^()]*?(?:!=|==|<=|>=|<|>)[^()]*\)"),
     "any()/all() of a vector comparison does not compile on CUDA: compare components explicitly"),
]


# OpenCL C builtins with no CUDA equivalent. Using one is fine only once
# cudadevice_*.cl (or the kernels themselves) define it.
OPENCL_ONLY_BUILTINS = (
    "sign select step smoothstep degrees radians fract mad native_divide native_recip native_sin "
    "native_cos native_tan native_rsqrt half_sqrt half_exp half_divide maxmag minmag rootn pown powr "
    "bitselect mad24 mul24 rotate isless isgreater islessequal isgreaterequal isnotequal vstore_half "
    "shuffle").split()


def strip_comments(text):
    """Blank out comments, keeping line numbers."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def lint(cl_index, repo=REPO):
    """Flags OpenCL-only idioms in kernels shared with CUDA. Device-specific
    files (Metal/Vulkan/OpenCL-only preludes) are skipped. Not exhaustive
    (e.g. scalar->vector assignment needs a type checker): NVRTC is the gate."""
    prelude = strip_comments("".join(cl_index[n].read_text(encoding="utf-8") for n in CUDA_PRELUDE))
    shared = {name: strip_comments(path.read_text(encoding="utf-8"))
              for name, path in sorted(cl_index.items())
              if not name.startswith(("cudadevice_", "ocldevice_", "metal", "vk", "vulkan"))}
    everything = prelude + "".join(shared.values())

    def is_defined(fn):
        return (re.search(rf"#define\s+{fn}\b", everything) or
                re.search(rf"\b\w+[\s*]+{fn}\s*\([^;{{]*\)\s*\{{", everything))

    rules = list(LINT_RULES)
    missing = [fn for fn in OPENCL_ONLY_BUILTINS if not is_defined(fn)]
    if missing:
        rules.append((re.compile(r"(?<![\w.>])(?:%s)\s*\(" % "|".join(missing)),
                      "OpenCL builtin not available on CUDA: add it to cudadevice_oclemul_funcs.cl"))

    problems = []
    for name, code in shared.items():
        path = cl_index[name]
        raw = path.read_text(encoding="utf-8").splitlines()
        for lineno, line in enumerate(code.splitlines(), 1):
            for rule, msg in rules:
                if rule.search(line):
                    problems.append(f"{path.relative_to(repo).as_posix()}:{lineno}: {msg}\n    {raw[lineno - 1].strip()}")
    return problems


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", type=Path, default=REPO)
    ap.add_argument("--nvrtc", help="path to libnvrtc.so / nvrtc64_*.dll")
    ap.add_argument("--arch", action="append",
                    help="NVRTC target(s), default compute_75 (oldest arch the wheels support)")
    ap.add_argument("--full", action="store_true", help="every PathOCL define combination (36 compiles)")
    ap.add_argument("--lint", action="store_true", help="static lint only (no NVRTC)")
    ap.add_argument("--only", help="regex on program name")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    ap.add_argument("--dump", type=Path, help="write assembled sources here")
    args = ap.parse_args()

    repo = args.repo.resolve()
    cl_index = index_cl_files(repo)

    problems = lint(cl_index, repo)
    for p in problems:
        print(f"LINT {p}")
    if args.lint:
        print(f"lint: {len(problems)} problem(s) in {len(cl_index)} kernel files")
        return 1 if problems else 0

    nvrtc_path = find_nvrtc(args.nvrtc)
    if not nvrtc_path:
        print("NVRTC not found. `pip install nvidia-cuda-nvrtc-cu12==12.9.86`, pass --nvrtc, "
              "or use --lint on macOS.", file=sys.stderr)
        return 2
    nvrtc = NVRTC(nvrtc_path)
    print(f"NVRTC {nvrtc.version[0]}.{nvrtc.version[1]} from {nvrtc_path}")

    jobs = []
    seen = set()
    for name, names, literal_opts, cpp in discover_programs(repo):
        if name in SKIP_PROGRAMS or (args.only and not re.search(args.only, name)):
            continue
        source = assemble(cl_index, names, EXTRA_SOURCE_PREFIX.get(name, ""))
        if name == "PathOCL kernel":
            variants = pathocl_variants(args.full)
        else:
            opts = literal_opts or ["-D LUXRAYS_OPENCL_KERNEL", "-D SLG_OPENCL_KERNEL"]
            if any("ray_funcs" == n or n.startswith("bvh") for n in names):
                opts = opts + [o for o in EPSILON_OPTS if o not in opts]
            variants = [("default", opts)]
        key = (name, source)
        if key in seen:
            continue
        seen.add(key)
        for label, opts in variants:
            jobs.append((name, label, cpp, source, opts))

    if not any(j[0] == "PathOCL kernel" for j in jobs) and not args.only:
        print("FAIL: PathOCL kernel program not discovered - update cuda_kernel_check.py", file=sys.stderr)
        return 1

    archs = args.arch or ["compute_75"]
    failures = 0
    t0 = time.time()

    def run(job, arch):
        name, label, cpp, source, opts = job
        full_opts = NVRTC_BASE_OPTS + [f"--gpu-architecture={arch}"] + opts + CUDA_DEVICE_OPTS
        if args.dump:
            args.dump.mkdir(parents=True, exist_ok=True)
            stem = re.sub(r"[^\w]+", "_", f"{name}_{label}_{arch}")
            (args.dump / f"{stem}.cu").write_text(source, encoding="utf-8")
            (args.dump / f"{stem}.opts").write_text("\n".join(full_opts), encoding="utf-8")
        t = time.time()
        ok, log = nvrtc.compile(source, name, full_opts)
        return job, arch, ok, log, time.time() - t

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(run, j, a) for j in jobs for a in archs]
        for f in concurrent.futures.as_completed(futs):
            (name, label, cpp, _, _), arch, ok, log, dt = f.result()
            status = "ok  " if ok else "FAIL"
            print(f"[{status}] {name} [{label}] {arch} ({cpp}) {dt:.1f}s", flush=True)
            if not ok:
                failures += 1
                print(errors_only(log), flush=True)

    print(f"\n{len(futs) - failures}/{len(futs)} CUDA program compiles passed in {time.time() - t0:.0f}s")
    if problems:
        print(f"{len(problems)} lint problem(s)")
    return 1 if failures or problems else 0


if __name__ == "__main__":
    sys.exit(main())
