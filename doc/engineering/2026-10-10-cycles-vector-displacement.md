# Cycles Vector Displacement spaces and original vertex identity

The 2.11.22 candidate imports Cycles Object, World and Tangent Vector Displacement into native geometry without editing artist meshes, UVs, nodes, links, Scale or Midlevel. This is an explicit adapter contract; native displacement defaults retain their historical channels and UV requirement. OpenPBR, Spectral defaults and transport quality are unchanged.

## Native shape contract

`scene.shapes.NAME.map.space` accepts `native` (default), `object`, `world`, or `tangent`. Explicit spaces require a vector map. The adapter exports `(Vector - Midlevel.xxx) * Scale` as RGB/vector data; linked Scale and Midlevel remain textures. `.objecttoworld` supplies the displacement evaluation context. World data is transformed back into object-local displacement; Object data stays local. Tangent data uses raw object normal, Mikk tangent and signed normalized cross-product bitangent. The native fallback follows the original first-triangle edge convention.

Optional `.map.normalindex` and `.map.tangentindex` address colour data; `.map.signindex` addresses alpha data. `.map.vertexidlowindex` and `.map.vertexidhighindex` address two vertex AOVs containing the original Blender vertex ID in exact float-representable 16-bit words. `.map.vertexidsmoothflag` adds the corner smoothing bit to the low word. Invalid, nonfinite, fractional, missing or out-of-range channels are rejected.

Displacement is evaluated at the first triangle corner of each original vertex, then applied equally to every UV/shading copy. Recomputed unit face normals accumulate by original vertex. POINT normals use the normalized result; `.map.normaldelta` preserves CORNER/custom normals by applying the before/after normal delta. Flat corners use displaced face normals. The `normalsmooth` default and native channel defaults remain unchanged.

Hit points initialize their complete context and pause spectral evaluation for RGB coordinate data. Textures see the original world position, original geometry normal and the supplied instance context. Geometry stays local to the actual rendered object's transform.

## Mikk API and source provenance

`ComputeMikkTangents(points, triangles, normals, uv=None, smooth=None, cycles_normal_precision=False)` returns triangle-corner tangents/signs. Positions and triangles have shape `(N, 3)`; normals `(T, 3, 3)`, UVs `(T, 3, 2)`, smoothing `(T,)`. Array shapes, finite data, vertex indices and index capacity are checked; source arrays are not modified and the GIL is released during computation.

The four Apache-2.0 headers in `include/luxcore/pysuperluxcore/mikktspace/` are byte-identical to Blender `9e2066aef7ef7e20c142ad7bd3303138a4304c93`, the installed Blender 5.2.1 LTS source. Original copyright/SPDX notices and a source README are retained. The optional normal precision follows Cycles octahedral 2x16-bit normal encoding; native callers default to their original full precision. The adapter uses Cycles precision and triangle callbacks, including object-position spherical coordinates when there are no UVs. A Python temporary-mesh approximation failed curved seam checks and was replaced by this native implementation.

Source contracts were checked against local Blender `kernel/svm/displace.h`, `scene/mesh_displace.cpp`, `scene/mesh.cpp`, `blender/mesh.cpp`, `blender/geometry.cpp`, and `util/types_normal.h` at that exact SHA. Shared Cycles geometry is evaluated once in its first object's context; the adapter preserves that context and forces shared-context rebuilds after transform/geometry edits.

## Validation and limits

The first four-platform release run (`37977575837`) exposed MSVC C7626: an unnamed typedef class cannot contain the new default member initializers. `DisplacementShape::Params` is now a named struct; its member layout, defaults and call-site type are unchanged. A corrected release run must pass before publishing 2.11.22.

Run `37979782508` passed Windows compilation but failed packaging: MSYS `xargs` aborted its argument-budget check in the large build environment, dropping the dependency DLL paths before delvewheel. The repair script now collects already-absolute paths without `xargs`, exits on repair failure and checks that an output wheel exists. Linux twice crashed during the installed-wheel smoke entry point; unbuffered Python fault handling and a native import backtrace preserve the existing gate while collecting its cause. Neither failing run is a release acceptance result. Its ARM wheel did pass the 115 local CPU/API/Metal checks at 720p; this does not override either failed platform gate.

Run `37982176791` passed Windows compilation and repair but exposed the same native-import access violation as Linux. Python fault traces identify the extension import itself, before the Python NVRTC/TBB/OIDN preload functions. The binding sources had inconsistent `PYBIND11_DETAILED_ERROR_MESSAGES`: the original main binding defined it locally, while the new Mikk binding did not. In pinned pybind11 3.0.1, `arg_v` gains a `std::string type` member under this definition (`cast.h`), so default-argument constructors and destructors have incompatible layouts across translation units. The definition is now a target compile definition, applied before headers to all three binding sources; the local definition is removed. The corrected installed-wheel smoke checks remain required on Windows and Linux before release. ARM native/Python bytes from this third run exactly match the previously reviewed ARM wheel, but that equality does not establish cross-platform acceptance.

The local ARM layout probe measured `sizeof(arg_v)` as 32 bytes without the definition and 56 with it. The corrected Release build, NumPy-unavailable import, loader-reinstalled exact binary guard and all 13 Mikk property checks passed. This is local binding validation; the four-platform release gates and final bundle validation remain separate.

Native `dev-tools/displacement-space-properties-test.py` covers 16 native legacy/imported-space and invalid-data conditions. Legacy 2.11.21 positions/rejections exactly match the candidate's three legacy checks. `dev-tools/mikktspace-properties-test.py` covers 13 UV basis, mirror orientation, smoothing, quantized-normal and malformed-input conditions.

Blender adapter validation, final runtime hashes, 720p image review, CPU/Metal results and release status are recorded in `../SuperBlendLuxCore/doc/engineering/2026-10-10-cycles-vector-displacement.md` in the workspace and the corresponding add-on repository. This document does not establish a public/installed 2.11.22 runtime until its deployment evidence is complete.

Vector BUMP/BOTH, scalar World/linked Normal, adaptive/cage displacement, mixed-material original-vertex boundaries, displaced Normal Map attribute updates and broad production scenes remain acceptance work. Flat nonplanar polygons and graph-group displacement outputs also require broader checks. A successful targeted geometry test does not mean full Cycles compatibility.
