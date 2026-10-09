# MikkTSpace header provenance

These four unmodified Apache-2.0 headers are from Blender commit
`9e2066aef7ef7e20c142ad7bd3303138a4304c93`, `intern/mikktspace/`.
Their original copyright and SPDX notices are retained.

Source: https://projects.blender.org/blender/blender/src/commit/9e2066aef7ef7e20c142ad7bd3303138a4304c93/intern/mikktspace

The standalone Python binding computes Mikk corner data without changing a
Blender mesh. Its optional Cycles normal precision follows the octahedral
encoding in the same commit's `intern/cycles/util/types_normal.h`; native
callers default to their original full-precision normals.
