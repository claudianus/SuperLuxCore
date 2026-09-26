#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Dense-vs-wavefront queue regression test (B2/E3, M1+M2).
#
# For each test case this script:
#   1. renders once in dense mode and once with
#      LUXRAYS_WAVEFRONT_QUEUES=1 (wavefront queues are opt-in)
#   2. asserts queue integrity from the LUXRAYS_WAVEFRONT_DEBUG=1
#      counters: oob / dup / badState / badLambda must all be 0
#   3. compares the RGB outputs with a pure-Python PNG decoder and
#      requires the mean-radiance relative difference to stay within a
#      Monte-Carlo tolerance (the two modes consume RNG in different
#      orders, so pixel-exact equality is NOT expected)
#
# Usage:
#   dev-tools/wavefront-regression.sh [path-to-luxcoreconsole]
#
# Exit code 0 = all cases passed, 1 = at least one failed.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CONSOLE="${1:-}"
if [ -z "$CONSOLE" ]; then
    for cand in \
        "$ROOT/out/build/samples/luxcoreconsole/Debug/luxcoreconsole" \
        "$ROOT/out/build/samples/luxcoreconsole/Release/luxcoreconsole" \
        "$ROOT/out/install/Release/bin/luxcoreconsole"; do
        if [ -x "$cand" ]; then CONSOLE="$cand"; break; fi
    done
fi
if [ ! -x "$CONSOLE" ]; then
    echo "ERROR: luxcoreconsole binary not found (pass path as \$1)" >&2
    exit 1
fi
echo "Using luxcoreconsole: $CONSOLE"

WORK="$(mktemp -d /tmp/wf-regression.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Mean-radiance comparison tolerance (relative). At haltspp >= 64 the
# dominant difference between dense and wavefront is Monte-Carlo noise;
# a genuine scheduling bug produces either a crash, a black image, or a
# far larger deviation than this.
TOL="0.20"

compare_png() {
    python3 - "$1" "$2" "$TOL" <<'PYEOF'
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
sa = sum(pa[::cha]) if cha > 1 else sum(pa)
sb = sum(pb[::chb]) if chb > 1 else sum(pb)
na = len(pa) // cha
ma, mb = sa / na / 255.0, sb / na / 255.0
tol = float(sys.argv[3])
rel = abs(ma - mb) / max(ma, mb, 1e-9)
print(f"mean dense={ma:.6f} wavefront={mb:.6f} reldiff={rel:.4f} tol={tol}")
sys.exit(0 if rel <= tol else 1)
PYEOF
}

run_case() {
    local name="$1" scn="$2" spectral="$3" spp="$4"
    local cfg="$WORK/$name.cfg"
    cat > "$cfg" <<EOF
renderengine.type = PATHOCL
sampler.type = SOBOL
film.width = 512
film.height = 512
scene.file = $scn
batch.halttime = 0
batch.haltspp = $spp
opencl.cpu.use = 0
opencl.gpu.use = 1
opencl.gpu.workgroup.size = 64
path.pathdepth.total = 5
path.spectral.enable = $spectral
film.imagepipelines.0.0.type = TONEMAP_LINEAR
film.imagepipelines.0.0.scale = 1
film.imagepipelines.0.1.type = GAMMA_CORRECTION
film.imagepipelines.0.1.value = 2.2
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.index = 0
film.outputs.0.filename = $WORK/$name.png
EOF

    # Run from $ROOT so scene-relative asset paths (PLYs etc.) resolve;
    # outputs go to $WORK via the absolute filename above.
    local ok=1
    ( cd "$ROOT" && "$CONSOLE" "$cfg" ) > "$WORK/$name.dense.log" 2>&1 || ok=0
    mv "$WORK/$name.png" "$WORK/$name.dense.png" 2>/dev/null || ok=0

    ( cd "$ROOT" && LUXRAYS_WAVEFRONT_QUEUES=1 LUXRAYS_WAVEFRONT_DEBUG=1 \
        "$CONSOLE" "$cfg" ) > "$WORK/$name.wf.log" 2>&1 || ok=0
    mv "$WORK/$name.png" "$WORK/$name.wf.png" 2>/dev/null || ok=0

    if [ "$ok" -eq 0 ]; then
        cp -r "$WORK" "/tmp/wf-regression-failed-$name" 2>/dev/null
        echo "[$name] FAIL: render did not complete (logs: /tmp/wf-regression-failed-$name)"
        return 1
    fi

    # Queue integrity: every debug counter must stay zero.
    local bad
    bad=$(grep -oE "oob=[0-9]+|dup=[0-9]+|badState=[0-9]+|badLambda=[0-9]+" \
          "$WORK/$name.wf.log" | grep -v "=0" | head -1)
    if [ -n "$bad" ]; then
        echo "[$name] FAIL: queue integrity violation: $bad"
        return 1
    fi

    if compare_png "$WORK/$name.dense.png" "$WORK/$name.wf.png" \
        | tee "$WORK/$name.cmp"; then
        echo "[$name] PASS"
        return 0
    else
        echo "[$name] FAIL: output difference beyond MC tolerance"
        return 1
    fi
}

FAIL=0
run_case cornell        scenes/cornell/cornell.scn          0 64 || FAIL=1
run_case cornell_spectral scenes/cornell/cornell-spectral.scn 1 128 || FAIL=1

if [ "$FAIL" -eq 0 ]; then
    echo "ALL WAVEFRONT REGRESSION TESTS PASSED"
else
    cp -r "$WORK" /tmp/wf-regression-failed 2>/dev/null
    echo "WAVEFRONT REGRESSION FAILURES (logs: /tmp/wf-regression-failed)"
fi
exit $FAIL
