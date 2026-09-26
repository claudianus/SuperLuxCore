#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Vulkan backend regression (VK1).
#
# Stage A (default, seconds): run vk_intersect_test, which drives the real
# device path end to end — Context -> VulkanIntersectionDevice ->
# vkdevice CompileProgram (clspv -> SPIR-V -> SPIRV-Cross MSL ->
# vkCreateComputePipelines) -> EnqueueTraceRayBuffer on the GPU — and
# checks 8 rays against the CPU BVH. It exercises every locally-patched
# piece: the clspv producer, the SPIRV-Cross physical-storage-buffer
# emission fixes, and MoltenVK pipeline creation.
#
# Stage B (--full, opt-in): render scenes/parity/emissive-direct at
# 1280x720 on PATHOCL with the Vulkan device selected and assert the
# deterministic centre pixel (4.0 +- 0.20, same check as
# parity-regression.sh; the centred 2x2 emissive quad covers the centre
# pixel at 16:9 too).
# NOTE: first run compiles Metal pipeline states for all 21 PATHOCL
# kernels and takes tens of minutes; subsequent runs reuse Metal's
# shader cache (roughly 4x faster).
#
# Required tools (auto-detected under dev-tools/vkrt or override):
#   LUXRAYS_CLSPV      clspv binary (default: dev-tools/vkrt/clspv/build/bin/clspv)
#   LUXRAYS_MOLTENVK_DIR  dir containing libMoltenVK.dylib
#                        (default: dev-tools/vkrt/MoltenVK/Package/Release/MoltenVK/dynamic/dylib/macOS)
#
# Usage:
#   dev-tools/vulkan-regression.sh [--full] [path-to-luxcore-bin-dir]
#
# Exit code 0 = all enabled stages passed, 1 = at least one failed.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

FULL=0
BINDIR=""
for a in "$@"; do
    case "$a" in
        --full) FULL=1 ;;
        *) BINDIR="$a" ;;
    esac
done

# ---- locate tools -----------------------------------------------------------
VKRT="${VKRT_DIR:-$ROOT/../dev-tools/vkrt}"
if [ ! -d "$VKRT" ]; then VKRT="$ROOT/dev-tools/vkrt"; fi

CLSPV="${LUXRAYS_CLSPV:-$VKRT/clspv/build/bin/clspv}"
LLVM_BIN="$(dirname "$CLSPV")/../third_party/llvm/bin"
MVK_DIR="${LUXRAYS_MOLTENVK_DIR:-$VKRT/MoltenVK/Package/Release/MoltenVK/dynamic/dylib/macOS}"

for b in vk_intersect_test luxcoreconsole; do
    if [ -n "$BINDIR" ] && [ -x "$BINDIR/$b" ]; then eval "$(echo "$b" | tr a-z A-Z)_BIN=\"$BINDIR/$b\""; fi
done
VKTEST="${VK_INTERSECT_TEST_BIN:-}"
CONSOLE="${LUXCORECONSOLE_BIN:-}"
if [ -z "$VKTEST" ]; then
    for cand in "$ROOT/out/build/bin/Release/vk_intersect_test" \
                "$ROOT/out/build/bin/Debug/vk_intersect_test"; do
        if [ -x "$cand" ]; then VKTEST="$cand"; break; fi
    done
fi
if [ -z "$CONSOLE" ]; then
    for cand in "$ROOT/out/build/samples/luxcoreconsole/Release/luxcoreconsole" \
                "$ROOT/out/build/samples/luxcoreconsole/Debug/luxcoreconsole" \
                "$ROOT/out/install/Release/bin/luxcoreconsole"; do
        if [ -x "$cand" ]; then CONSOLE="$cand"; break; fi
    done
fi
if [ ! -x "$VKTEST" ]; then echo "ERROR: vk_intersect_test not found" >&2; exit 1; fi
if [ "$FULL" = 1 ] && [ ! -x "$CONSOLE" ]; then
    echo "ERROR: luxcoreconsole not found (needed for --full)" >&2; exit 1
fi
if [ ! -x "$CLSPV" ]; then echo "ERROR: clspv not found at $CLSPV" >&2; exit 1; fi
if [ ! -f "$MVK_DIR/libMoltenVK.dylib" ]; then
    echo "ERROR: libMoltenVK.dylib not found at $MVK_DIR" >&2; exit 1
fi

export LUXRAYS_CLSPV="$CLSPV"
export PATH="$(dirname "$CLSPV"):$LLVM_BIN:$PATH"
export DYLD_FALLBACK_LIBRARY_PATH="$MVK_DIR${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"

echo "vk_intersect_test: $VKTEST"
echo "clspv:             $CLSPV"
echo "libMoltenVK:       $MVK_DIR/libMoltenVK.dylib"

WORK="$(mktemp -d /tmp/vulkan-regression.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

FAIL=0

# ---- Stage A: intersection test ----------------------------------------------
echo "--- Stage A: vk_intersect_test"
if "$VKTEST" > "$WORK/intersect.log" 2>&1 && grep -q 'VK_INTERSECT: PASS' "$WORK/intersect.log"; then
    echo "[intersect] PASS ($(grep -o 'PASS ([0-9]*/[0-9]* rays)' "$WORK/intersect.log" | head -1))"
else
    cp "$WORK/intersect.log" /tmp/vulkan-regression-intersect.log 2>/dev/null
    echo "[intersect] FAIL (log: /tmp/vulkan-regression-intersect.log)"
    tail -15 "$WORK/intersect.log"
    FAIL=1
fi

# ---- Stage B: full PATHOCL render (opt-in) ------------------------------------
if [ "$FULL" = 1 ]; then
    echo "--- Stage B: PATHOCL Vulkan render (scenes/parity/emissive-direct)"

    # Vulkan is opt-in: it is only selected via the opencl.devices.select
    # string. Probe the enumeration first, then build the mask that selects
    # only the VULKAN_GPU entry. The Native thread device is not part of the
    # selectable list (see oclrenderengine.cpp).
    probe_cfg="$WORK/probe.cfg"
    sed -e "s|^batch.haltspp.*|batch.haltspp = \"1\"|" \
        "$ROOT/scenes/parity/emissive-direct.cfg" > "$probe_cfg"
    cat >> "$probe_cfg" <<EOF
renderengine.type = PATHCPU
EOF
    ( cd "$ROOT" && "$CONSOLE" "$probe_cfg" ) > "$WORK/probe.log" 2>&1
    SELECT=$(python3 - "$WORK/probe.log" <<'PYEOF'
import re, sys
types = [m.group(2) for m in re.finditer(r'Device (\d+) type: (\w+)', open(sys.argv[1]).read())]
# selectable devices exclude NATIVE_THREAD
sel = [t for t in types if t != 'NATIVE_THREAD']
try:
    idx = sel.index('VULKAN_GPU')
except ValueError:
    print(''); sys.exit(0)
print(''.join('1' if i == idx else '0' for i in range(len(sel))))
PYEOF
)
    if [ -z "$SELECT" ]; then
        echo "[render] FAIL: no VULKAN_GPU device enumerated (log: $WORK/probe.log)"
        cp "$WORK/probe.log" /tmp/vulkan-regression-probe.log 2>/dev/null
        FAIL=1
    else
        echo "device select string: $SELECT"
        cfg="$WORK/emissive-vk.cfg"
        sed -e "s|^film.outputs.1.filename.*|film.outputs.1.filename = \"$WORK/emissive-vk.hdr\"|" \
            -e "s|^batch.haltspp.*|batch.haltspp = \"64\"|" \
            -e "s|^film.width.*|film.width = \"1280\"|" \
            -e "s|^film.height.*|film.height = \"720\"|" \
            "$ROOT/scenes/parity/emissive-direct.cfg" > "$cfg"
        cat >> "$cfg" <<EOF
renderengine.type = PATHOCL
opencl.cpu.use = 0
opencl.devices.select = $SELECT
EOF
        echo "rendering (first run compiles Metal pipelines for 21 kernels — can take tens of minutes)..."
        ( cd "$ROOT" && "$CONSOLE" "$cfg" ) > "$WORK/emissive-vk.log" 2>&1
        # Report/verify which intersection path ran. A silent SW fallback
        # would still pass the pixel check but stop exercising HWRT.
        if grep -q "ray-query intersection active" "$WORK/emissive-vk.log"; then
            echo "[render] intersection mode: HWRT ray-query (BLAS+TLAS)"
        elif grep -q "using SW traversal" "$WORK/emissive-vk.log"; then
            echo "[render] WARNING: HWRT fell back to SW traversal"
            grep "using SW traversal" "$WORK/emissive-vk.log" | head -2
        else
            echo "[render] FAIL: neither HWRT active nor SW-fallback log found"
            cp "$WORK/emissive-vk.log" /tmp/vulkan-regression-render.log 2>/dev/null
            FAIL=1
        fi
        if [ ! -f "$WORK/emissive-vk.hdr" ]; then
            cp "$WORK/emissive-vk.log" /tmp/vulkan-regression-render.log 2>/dev/null
            echo "[render] FAIL: no output (log: /tmp/vulkan-regression-render.log)"
            tail -10 "$WORK/emissive-vk.log"
            FAIL=1
        else
            # reuse the same centre-pixel check as parity-regression.sh
            python3 - "$WORK/emissive-vk.hdr" <<'PYEOF' || FAIL=1
import math, struct, sys
def load_hdr(fn):
    data = open(fn, "rb").read(); pos = 0
    while True:
        e = data.find(b"\n", pos); line = data[pos:e]; pos = e + 1
        if line == b"":
            e2 = data.find(b"\n", pos)
            res = data[pos:e2].split(); pos = e2 + 1
            h, w = int(res[1]), int(res[3]); break
    px = bytearray(w * h * 4)
    for y in range(h):
        a, b, c, d = struct.unpack(">BBBB", data[pos:pos + 4]); pos += 4
        for ch in range(4):
            x = 0
            while x < w:
                cnt = data[pos]; pos += 1
                if cnt > 128:
                    v = data[pos]; pos += 1
                    for i in range(cnt - 128):
                        px[(y * w + x) * 4 + ch] = v; x += 1
                else:
                    for i in range(cnt):
                        px[(y * w + x) * 4 + ch] = data[pos]; pos += 1; x += 1
    out = []
    for i in range(w * h):
        r, g, b_, e = px[i * 4:i * 4 + 4]
        f = math.ldexp(1.0, e - 136) if e else 0.0
        out.append((r * f, g * f, b_ * f))
    return w, h, out
w, h, px = load_hdr(sys.argv[1])
c = px[(h // 2) * w + w // 2]
print("centre=(%.4f,%.4f,%.4f) expect=4.0 tol=0.20" % c)
sys.exit(0 if all(abs(v - 4.0) <= 0.20 for v in c) else 1)
PYEOF
            if [ "$FAIL" = 0 ]; then echo "[render] PASS"; else
                cp "$WORK/emissive-vk.log" /tmp/vulkan-regression-render.log 2>/dev/null
                echo "[render] FAIL: centre mismatch (log: /tmp/vulkan-regression-render.log)"
            fi
        fi
    fi
fi

# ---- Stage C: scene-edit AS rebuild (opt-in, needs pysuperluxcore) -----------
if [ "$FULL" = 1 ]; then
    echo "--- Stage C: scene-edit AS rebuild (vk_rt_update_test.py)"
    PYLUX="${PYLUX_PYTHON:-}"
    if [ -z "$PYLUX" ]; then
        for cand in /Applications/Blender.app/Contents/Resources/*/python/bin/python3.* \
                    "$(command -v python3.13 2>/dev/null)"; do
            if [ -x "$cand" ] && "$cand" -c "import sys; sys.path.insert(0,'$ROOT/out/build/src/pysuperluxcore/Release'); import pysuperluxcore" 2>/dev/null; then
                PYLUX="$cand"; break
            fi
        done
    fi
    if [ -z "$PYLUX" ]; then
        echo "[update] SKIP: no python with pysuperluxcore (set PYLUX_PYTHON)"
    elif "$PYLUX" "$ROOT/dev-tools/vk_rt_update_test.py" > "$WORK/update.log" 2>&1 \
            && [ "$(grep -c 'BLAS + TLAS built' "$WORK/update.log")" -ge 2 ] \
            && grep -q 'VK_RT_UPDATE: session completed' "$WORK/update.log"; then
        echo "[update] PASS (AS rebuilt after scene edit, $(grep -c 'BLAS + TLAS built' "$WORK/update.log") builds)"
    else
        cp "$WORK/update.log" /tmp/vulkan-regression-update.log 2>/dev/null
        echo "[update] FAIL (log: /tmp/vulkan-regression-update.log)"
        tail -15 "$WORK/update.log"
        FAIL=1
    fi
fi

if [ "$FAIL" = 0 ]; then
    echo "ALL VULKAN REGRESSION CASES PASSED"
else
    echo "VULKAN REGRESSION FAILED"
fi
exit $FAIL
