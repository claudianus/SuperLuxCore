#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# CPU-vs-GPU renderer parity on complex scenes.
#
# For each test case this script renders once on PATHCPU and once on
# PATHOCL (Metal HWRT on Apple Silicon) and compares the mean output
# radiance within a Monte-Carlo tolerance. The two engines consume
# different RNG streams, so pixel-exact equality is NOT expected; the
# tolerance is per-case and sized to catch black screens, channel
# mixups and systematic energy loss while tolerating firefly variance.
#
# Cases cover the paths where a CPU/GPU divergence is most damaging:
# multi-bounce caustics, volumes, mirror chains, spectral transport,
# and curve/hair geometry.
#
# Usage:
#   dev-tools/cpu-gpu-parity.sh [path-to-luxcoreconsole] [case ...]
#
# Exit code 0 = all cases passed, 1 = at least one failed.

set -u
set -o pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CONSOLE=""
if [ $# -gt 0 ] && [ -x "$1" ]; then
    CONSOLE="$1"; shift
fi
if [ -z "$CONSOLE" ]; then
    for cand in \
        "$ROOT/out/build/samples/luxcoreconsole/Release/luxcoreconsole" \
        "$ROOT/out/build/samples/luxcoreconsole/Debug/luxcoreconsole" \
        "$ROOT/out/install/Release/bin/luxcoreconsole"; do
        if [ -x "$cand" ]; then CONSOLE="$cand"; break; fi
    done
fi
if [ ! -x "$CONSOLE" ]; then
    echo "ERROR: luxcoreconsole binary not found (pass path as \$1)" >&2
    exit 1
fi
echo "Using luxcoreconsole: $CONSOLE"

WORK="$(mktemp -d /tmp/cpu-gpu-parity.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Mean-radiance comparison (same pure-Python PNG decoder as
# wavefront-regression.sh; compares the first channel mean like the
# wavefront script so near-monochrome scenes get a stable statistic).
compare_png() {
    python3 - "$1" "$2" "$3" <<'PYEOF'
import struct, sys, zlib

def png_pixels(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w, h, bitdepth, ctype = 8, b"", 0, 0, 8, 2
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype4 = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype4 == b"IHDR":
            w, h, bitdepth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif ctype4 == b"IDAT":
            idat += chunk
        pos += 12 + length
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    stride = w * ch
    px = bytearray()
    prev = bytearray(stride)
    off = 0
    for _ in range(h):
        f = raw[off]; off += 1
        line = bytearray(raw[off:off + stride]); off += stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc
                                      else b if pb <= pc else c)) & 255
        px += line
        prev = line
    return px, ch

pa, cha = png_pixels(sys.argv[1])
pb, chb = png_pixels(sys.argv[2])
assert len(pa) == len(pb), "size mismatch"
# All-channel mean (RGB parity signal, not just luminance)
sa = sum(pa); sb = sum(pb)
na = len(pa)
ma, mb = sa / na / 255.0, sb / na / 255.0
tol = float(sys.argv[3])
rel = abs(ma - mb) / max(ma, mb, 1e-9)
print(f"mean cpu={ma:.6f} gpu={mb:.6f} reldiff={rel:.4f} tol={tol}")
sys.exit(0 if rel <= tol else 1)
PYEOF
}

# run_case <name> <scene> <spectral> <spp> <tol> [extra cfg lines]
run_case() {
    local name="$1" scn="$2" spectral="$3" spp="$4" tol="$5" extra="${6:-}"
    local cfg="$WORK/$name.cfg"
    cat > "$cfg" <<EOF
sampler.type = SOBOL
film.width = 256
film.height = 256
scene.file = $scn
batch.halttime = 0
batch.haltspp = $spp
path.pathdepth.total = 8
path.spectral.enable = $spectral
film.imagepipelines.0.0.type = TONEMAP_LINEAR
film.imagepipelines.0.0.scale = 1
film.imagepipelines.0.1.type = GAMMA_CORRECTION
film.imagepipelines.0.1.value = 2.2
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.index = 0
film.outputs.0.filename = $WORK/$name.png
$extra
EOF

    local ok=1
    cat >> "$cfg" <<EOF
renderengine.type = PATHCPU
opencl.cpu.use = 1
opencl.gpu.use = 0
EOF
    ( cd "$ROOT" && "$CONSOLE" "$cfg" ) > "$WORK/$name.cpu.log" 2>&1 || ok=0
    mv "$WORK/$name.png" "$WORK/$name.cpu.png" 2>/dev/null || ok=0

    # Strip the engine lines, then write the GPU variant (must keep the
    # .cfg extension - luxcoreconsole rejects unknown suffixes)
    local gcfg="$WORK/$name.gpu.cfg"
    grep -v "renderengine.type\|opencl\." "$cfg" > "$gcfg"
    cat >> "$gcfg" <<EOF
renderengine.type = PATHOCL
opencl.cpu.use = 0
opencl.gpu.use = 1
opencl.gpu.workgroup.size = 64
EOF
    ( cd "$ROOT" && "$CONSOLE" "$gcfg" ) > "$WORK/$name.gpu.log" 2>&1 || ok=0
    mv "$WORK/$name.png" "$WORK/$name.gpu.png" 2>/dev/null || ok=0

    if [ "$ok" -eq 0 ]; then
        cp -r "$WORK" "/tmp/cpu-gpu-parity-failed-$name" 2>/dev/null
        echo "[$name] FAIL: render did not complete (logs: /tmp/cpu-gpu-parity-failed-$name)"
        return 1
    fi

    if compare_png "$WORK/$name.cpu.png" "$WORK/$name.gpu.png" "$tol" \
        | tee "$WORK/$name.cmp"; then
        echo "[$name] PASS"
        return 0
    else
        cp "$WORK/$name.cpu.png" "$WORK/$name.gpu.png" \
            "/tmp/cpu-gpu-parity-failed-$name.png" 2>/dev/null
        echo "[$name] FAIL: output difference beyond MC tolerance"
        return 1
    fi
}

FAIL=0
CASES="$*"
if [ -z "${CASES// /}" ]; then
    CASES="cornell caustic_many mirror_maze vol_caustic spectral strands"
fi
for c in $CASES; do
    case "$c" in
        cornell)
            run_case cornell scenes/cornell/cornell.scn 0 64 0.10 || FAIL=1 ;;
        caustic_many)
            # Multi-bounce specular caustics: firefly-heavy, needs a
            # wider band at moderate spp.
            run_case caustic_many scenes/caustics/caustic-stress-many.scn 0 64 0.15 || FAIL=1 ;;
        mirror_maze)
            run_case mirror_maze scenes/caustics/mirror-maze.scn 0 64 0.15 || FAIL=1 ;;
        vol_caustic)
            run_case vol_caustic scenes/caustics/vol-caustic-deep.scn 0 64 0.20 || FAIL=1 ;;
        spectral)
            # cornell-spectral.scn (laser through a dispersive prism) is
            # firefly-dominated: its mean is decided by a handful of rare
            # caustic hits, so it can not gate CPU/GPU parity reliably
            # (measured ~0.2-0.3 reldiff at 256-1024spp on BOTH spectral
            # and non-spectral runs - tail luck, not energy loss; the
            # p95/p99 structure matched exactly). The area-light variant
            # exercises the same spectral+dispersion code paths with a
            # well-behaved estimator.
            run_case spectral scenes/cornell/cornell-spectral-area.scn 1 128 0.15 || FAIL=1 ;;
        strands)
            run_case strands scenes/strands/hair.scn 0 16 0.20 || FAIL=1 ;;
        kitchen)
            run_case kitchen scenes/kitchen/kitchen.scn 0 32 0.15 || FAIL=1 ;;
        classroom)
            run_case classroom scenes/classroom/classroom.scn 0 32 0.15 || FAIL=1 ;;
        *)
            echo "[$c] unknown case"; FAIL=1 ;;
    esac
done

if [ "$FAIL" -eq 0 ]; then
    echo "ALL CPU/GPU PARITY TESTS PASSED"
else
    cp -r "$WORK" /tmp/cpu-gpu-parity-failed 2>/dev/null
    echo "CPU/GPU PARITY FAILURES (logs: /tmp/cpu-gpu-parity-failed)"
fi
exit $FAIL
