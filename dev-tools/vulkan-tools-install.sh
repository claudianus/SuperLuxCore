#!/usr/bin/env bash
# Installs the LuxCore Vulkan toolchain into ~/.luxcore/vktools so any
# host process can enumerate Vulkan devices and compile kernels without
# environment setup (GUI-launched Blender has no developer PATH or
# DYLD_FALLBACK_LIBRARY_PATH).
#
# Layout mirrors the clspv build tree because vkdevice resolves the
# bundled LLVM tools relative to the clspv binary:
#   <clspv>/../third_party/llvm/bin/{opt,llvm-dis}
#
#   vktools/bin/clspv, clspv-reflection
#   vktools/third_party/llvm/bin/{opt,llvm-dis}
#   vktools/lib/libMoltenVK.dylib          (macOS)
#
# Override sources via env: LUXRAYS_CLSPV, LUXRAYS_MOLTENVK, LUX_VKTOOLS.
set -euo pipefail

WORKSPACE="$(cd "$(dirname "$0")/../.." && pwd)"
VKRT="${LUX_VKRT:-$WORKSPACE/dev-tools/vkrt}"
CLSPV="${LUXRAYS_CLSPV:-$VKRT/clspv/build/bin/clspv}"
MVK="${LUXRAYS_MOLTENVK:-$VKRT/MoltenVK/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib}"
DEST="${LUX_VKTOOLS:-$HOME/.luxcore/vktools}"

for f in "$CLSPV"; do
	[ -x "$f" ] || { echo "missing: $f (set LUXRAYS_CLSPV)" >&2; exit 1; }
done
CLSPV_BIN="$(dirname "$CLSPV")"
CLSPV_ROOT="$(cd "$CLSPV_BIN/.." && pwd)"
LLVM_BIN="$CLSPV_ROOT/third_party/llvm/bin"

mkdir -p "$DEST/bin" "$DEST/third_party/llvm/bin" "$DEST/lib"

install -m 0755 "$CLSPV_BIN/clspv" "$DEST/bin/clspv"
if [ -x "$CLSPV_BIN/clspv-reflection" ]; then
	install -m 0755 "$CLSPV_BIN/clspv-reflection" "$DEST/bin/clspv-reflection"
fi
for t in opt llvm-dis; do
	[ -x "$LLVM_BIN/$t" ] || { echo "missing: $LLVM_BIN/$t" >&2; exit 1; }
	install -m 0755 "$LLVM_BIN/$t" "$DEST/third_party/llvm/bin/$t"
done

if [ "$(uname -s)" = "Darwin" ] && [ -f "$MVK" ]; then
	install -m 0755 "$MVK" "$DEST/lib/libMoltenVK.dylib"
	# MoltenVK's install_name may reference @rpath; make it standalone
	# so a plain dlopen by absolute path always resolves.
	install_name_tool -id "$DEST/lib/libMoltenVK.dylib" \
		"$DEST/lib/libMoltenVK.dylib" 2>/dev/null || true
	codesign --force --sign - "$DEST/lib/libMoltenVK.dylib" 2>/dev/null || true
else
	echo "note: no MoltenVK dylib at $MVK (non-macOS or unset LUXRAYS_MOLTENVK)"
fi

echo "installed Vulkan toolchain to $DEST"
ls -R "$DEST"
