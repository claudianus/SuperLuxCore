#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# GPU light-tracing depth parity regression.
#
# Renders scenes/cornell/lmnee-open.scn (a diffuse Cornell box with no
# specular occluders, so every camera connection is a plain light->lens
# splat) at increasing path.maxdepth and asserts the PATHOCL
# lighttracing.only mean radiance tracks CPU LIGHTCPU.
#
# Regression this guards: hybridBackForwardEnable is force-enabled by
# path.lighttracing.enable and truncates non-specular light paths at
# diffuse+glossy depth > 1. That is correct in hybrid mode (eye paths own
# the diffuse term) but wrong in lighttracing.only mode (eyeTaskCount==0),
# where the light path is the sole estimator and must run full depth.
# Before the fix the depth>=3 indirect term was dropped entirely:
#
#   maxdepth   GPU/CPU mean ratio
#     1          ~1.00
#     2          ~1.00
#     3          ~0.97   <- deficit starts
#     4          ~0.886  <- deep diffuse connects lost
#
# After the fix (gate the cut on eyeTaskCount > 0) every depth matches
# within Monte-Carlo noise (~1.000).
#
# Usage:
#   dev-tools/lighttracing-depth-parity.sh [path-to-luxcoreconsole]
#
# Exit code 0 = all depths passed, 1 = at least one failed.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CONSOLE="${1:-}"
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

# A python with PIL for PNG decode (PIL is not always on PATH python3).
PY=""
for p in /usr/bin/python3 python3 /opt/homebrew/bin/python3; do
    if "$p" -c "import PIL" 2>/dev/null; then PY="$p"; break; fi
done
if [ -z "$PY" ]; then
    echo "ERROR: no python3 with PIL found (needed to decode PNG output)" >&2
    exit 1
fi

WORK="$(mktemp -d /tmp/lt-depth-parity.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Mean pixel value of a PNG via PIL.
mean_png() {
    "$PY" - "$1" <<'PYEOF'
import sys
from PIL import Image
import numpy as np
im = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.float64)
print("%.6f" % im.mean())
PYEOF
}

# $1 = depth, $2 = "cpu"|"gpu"  -> echoes mean pixel value
render_mean() {
    local depth="$1" backend="$2"
    local out="$WORK/d${depth}-${backend}.png"
    local cfg="$WORK/d${depth}-${backend}.cfg"
    cat > "$cfg" <<EOF
film.width = 512
film.height = 288
batch.halttime = 0
batch.haltspp = 256
scene.file = scenes/cornell/lmnee-open.scn
sampler.type = SOBOL
filter.type = NONE
screen.refresh.interval = 0
path.maxdepth = $depth
image.filename = "$out"
film.imagepipeline.0.type = TONEMAP_LINEAR
film.imagepipeline.1.type = GAMMA_CORRECTION
film.imagepipeline.1.value = 2.2
EOF
    if [ "$backend" = cpu ]; then
        cat >> "$cfg" <<EOF
renderengine.type = LIGHTCPU
opencl.cpu.use = 1
opencl.gpu.use = 0
EOF
    else
        cat >> "$cfg" <<EOF
renderengine.type = PATHOCL
opencl.cpu.use = 0
opencl.gpu.use = 1
opencl.native.threads.count = 0
path.lighttracing.enable = 1
path.lighttracing.taskfraction = 1.0
path.lighttracing.only = 1
EOF
    fi
    # luxcoreconsole writes the film to image.png regardless of the
    # configured filename, so capture it from the CWD.
    local before="$ROOT/image.png"
    rm -f "$before"
    ( cd "$ROOT" && "$CONSOLE" "$cfg" ) > "$WORK/d${depth}-${backend}.log" 2>&1
    if [ ! -f "$before" ]; then
        cp -r "$WORK" "/tmp/lt-depth-parity-failed" 2>/dev/null
        echo "[depth=$depth/$backend] FAIL: no output (logs: /tmp/lt-depth-parity-failed)" >&2
        echo "-1"
        return
    fi
    mv "$before" "$out"
    mean_png "$out"
}

FAIL=0
# depth 2 sanity (pre-fix already matched) + depth 4 (the regressed case).
# Tolerance +-8% absorbs MC noise across samplers; the bug was a clean
# ~11% drop, well outside the band.
for depth in 2 4; do
    cpu=$(render_mean "$depth" cpu)
    gpu=$(render_mean "$depth" gpu)
    res=$("$PY" - "$cpu" "$gpu" <<'PYEOF'
import sys
cpu, gpu = float(sys.argv[1]), float(sys.argv[2])
if cpu <= 0 or gpu < 0:
    print("FAIL nan")
else:
    r = gpu / cpu
    print("ratio=%.4f %s" % (r, "PASS" if 0.92 <= r <= 1.08 else "FAIL"))
PYEOF
)
    echo "[depth=$depth] cpu_mean=$cpu gpu_mean=$gpu $res"
    case "$res" in *FAIL*) FAIL=1;; esac
done

if [ "$FAIL" -eq 0 ]; then
    echo "ALL LIGHT-TRACING DEPTH PARITY CASES PASSED"
else
    echo "LIGHT-TRACING DEPTH PARITY FAILED"
fi
exit $FAIL
