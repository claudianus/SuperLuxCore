# Copyright (C) Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Preprocesses a .metal kernel source into a self-contained MSL source that can
# be compiled at runtime with -[MTLDevice newLibraryWithSource:]. This avoids the
# build-time 'xcrun metal'/'metallib' toolchain (Xcode-only) so the Metal device
# module can be built with the Command Line Tools alone, matching how LuxCore
# compiles its own Metal kernels (runtime source compilation).
#
# Steps:
#   1. clang -E with __METAL_VERSION__ defined selects the Metal device code path
#      (OIDN_COMPILE_METAL_DEVICE). A stub 'metal_stdlib' on the include path lets
#      the preprocessor resolve <metal_stdlib>; the real include is re-added below.
#   2. linemarkers / pragmas are stripped.
#   3. '#include <metal_stdlib>' is re-prepended so the runtime Metal compiler
#      resolves the standard library.

import os, sys, subprocess, tempfile, re

if len(sys.argv) < 4:
    sys.exit("Usage: metal_source.py <out.metal> <in.metal> <include_dir> [include_dir...]")

out, src, incs = sys.argv[1], sys.argv[2], sys.argv[3:]
sdk = subprocess.check_output(["xcrun", "--show-sdk-path"]).decode().strip()

with tempfile.TemporaryDirectory() as td:
    stub = os.path.join(td, "metal_stdlib")
    open(stub, "w").close()
    cmd = ["clang", "-E", "-x", "c++", "-std=c++17", "-D__METAL_VERSION__=310",
           "-isysroot", sdk] + sum([["-I", i] for i in incs], []) + ["-I", td,
           src, "-o", out + ".tmp"]
    subprocess.check_call(cmd)

with open(out + ".tmp") as f:
    lines = [l for l in f if not re.match(r"^# \d+", l) and not l.startswith("#pragma")]
os.remove(out + ".tmp")
with open(out, "w") as f:
    f.write("#include <metal_stdlib>\n")
    f.writelines(lines)
