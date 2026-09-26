#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# CPU/GPU backend parity regression (A4).
#
# Renders scenes/parity/{emissive-direct,whiteenv} on PATHCPU and on the
# GPU intersection backend (PATHOCL; Metal HWRT on Apple Silicon) and
# asserts the deterministic centre-pixel values documented in
# scenes/parity/README.md:
#
#   emissive-direct  centre == (4,4,4)
#   whiteenv         centre == (0,0,0)
#
# The assertion is a range check, not exact equality: the centre pixel's
# reconstruction-filter footprint can graze the quad edge, so stochastic
# samplers report 3.98-4.0 (measured on both CPU and Metal). A genuine
# intersection leak produces a far larger deviation (the useMotionTime
# regression gave 1.48 / 0.625 — see scenes/parity/README.md).
# Outputs are Radiance .hdr files decoded by a pure-Python RGBE parser.
#
# Usage:
#   dev-tools/parity-regression.sh [path-to-luxcoreconsole]
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

WORK="$(mktemp -d /tmp/parity-regression.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

check_center() {
    # $1 = hdr path, $2 = expected centre value, $3 = abs tolerance
    python3 - "$1" "$2" "$3" <<'PYEOF'
import math, struct, sys

def load_hdr(fn):
    data = open(fn, "rb").read()
    pos = 0
    while True:
        e = data.find(b"\n", pos)
        line = data[pos:e]; pos = e + 1
        if line == b"":
            e2 = data.find(b"\n", pos)
            res = data[pos:e2].split(); pos = e2 + 1
            h, w = int(res[1]), int(res[3]); break
    px = bytearray(w * h * 4)
    for y in range(h):
        a, b, c, d = struct.unpack(">BBBB", data[pos:pos + 4]); pos += 4
        assert a == 2 and b == 2, "uncompressed scanline unsupported"
        assert ((c << 8) | d) == w
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
expect = float(sys.argv[2])
tol = float(sys.argv[3])
c = px[(h // 2) * w + w // 2]
print("centre=(%.4f,%.4f,%.4f) expect=%.4f tol=%.4f" % (c[0], c[1], c[2], expect, tol))
sys.exit(0 if all(abs(v - expect) <= tol for v in c) else 1)
PYEOF
}

run_backend() {
    local name="$1" backend="$2" expect="$3" tol="$4"
    local cfg="$WORK/$name-$backend.cfg"
    # 64spp: deterministic at convergence — at 8spp the Metal sampler's
    # different RNG stream gives ~0.4% centre deviation (MC noise, not a
    # leak; verified it converges to exactly 4.0 at 64spp).
    sed -e "s|^film.outputs.1.filename.*|film.outputs.1.filename = \"$WORK/$name-$backend.hdr\"|" \
        -e "s|^batch.haltspp.*|batch.haltspp = \"64\"|" \
        "$ROOT/scenes/parity/$name.cfg" > "$cfg"
    if [ "$backend" = cpu ]; then
        cat >> "$cfg" <<EOF
renderengine.type = PATHCPU
opencl.cpu.use = 1
opencl.gpu.use = 0
EOF
    else
        cat >> "$cfg" <<EOF
renderengine.type = PATHOCL
opencl.cpu.use = 0
opencl.gpu.use = 1
EOF
    fi
    ( cd "$ROOT" && "$CONSOLE" "$cfg" ) > "$WORK/$name-$backend.log" 2>&1
    if [ ! -f "$WORK/$name-$backend.hdr" ]; then
        cp -r "$WORK" "/tmp/parity-regression-failed" 2>/dev/null
        echo "[$name/$backend] FAIL: no output (logs: /tmp/parity-regression-failed)"
        return 1
    fi
    if ! check_center "$WORK/$name-$backend.hdr" "$expect" "$tol"; then
        cp -r "$WORK" "/tmp/parity-regression-failed" 2>/dev/null
        echo "[$name/$backend] FAIL: centre mismatch (logs: /tmp/parity-regression-failed)"
        return 1
    fi
    echo "[$name/$backend] PASS"
    return 0
}

FAIL=0
# emissive: 5% band around 4.0 catches the 1.48 leak class, tolerates the
# ~0.4% edge-filter deviation seen across samplers/backends.
run_backend emissive-direct cpu   4.0 0.20 || FAIL=1
run_backend emissive-direct gpu   4.0 0.20 || FAIL=1
# whiteenv: 0.02 absolute catches the 0.625 leak, tolerates edge bleed.
run_backend whiteenv       cpu   0.0 0.02 || FAIL=1
run_backend whiteenv       gpu   0.0 0.02 || FAIL=1

if [ "$FAIL" -eq 0 ]; then
    echo "ALL PARITY CASES PASSED"
else
    echo "PARITY REGRESSION FAILED"
fi
exit $FAIL
