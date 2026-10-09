// SPDX-FileCopyrightText: 2026 SuperLuxCore contributors
// SPDX-FileCopyrightText: 2011-2026 Blender Foundation (octahedral normal encoding)
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
// Blender's build supplies uint; Windows does not provide the POSIX alias.
namespace mikk { using uint = unsigned int; }
// The pinned Blender headers use these branch hints from Blender's build.
// Keep the compatibility definitions local so the vendored files stay intact.
#ifndef LIKELY
#define LIKELY(x) (x)
#define SLC_MIKK_DEFINED_LIKELY
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) (x)
#define SLC_MIKK_DEFINED_UNLIKELY
#endif
#include "luxcore/pysuperluxcore/mikktspace/mikktspace.hh"
#ifdef SLC_MIKK_DEFINED_LIKELY
#undef LIKELY
#undef SLC_MIKK_DEFINED_LIKELY
#endif
#ifdef SLC_MIKK_DEFINED_UNLIKELY
#undef UNLIKELY
#undef SLC_MIKK_DEFINED_UNLIKELY
#endif

namespace py = pybind11;
namespace {
using F3 = mikk::float3;
F3 Unit(const F3 &v) { return v.length_squared() > 0.f ? v.normalize() : F3::zero(); }
F3 PackedNormalRoundTrip(const F3 &v) {
    // Blender types_normal.h: octahedral 2x16-bit normal representation.
    constexpr float hmu = 65535.f / 2.f;
    const float inv = 1.f / (fabsf(v.x) + fabsf(v.y) + fabsf(v.z) + 1e-6f);
    float x = v.x * inv, y = v.y * inv;
    const float wx = (1.f - fabsf(y)) * copysignf(1.f, x);
    const float wy = (1.f - fabsf(x)) * copysignf(1.f, y);
    if (v.z < 0.f) { x = wx; y = wy; }
    const unsigned int ix = static_cast<unsigned int>(std::clamp(x * hmu + (hmu + .5f), 0.f, 65535.f));
    const unsigned int iy = static_cast<unsigned int>(std::clamp(y * hmu + (hmu + .5f), 0.f, 65535.f));
    x = ix * (2.f / 65535.f) - 1.f;
    y = iy * (2.f / 65535.f) - 1.f;
    const float z = 1.f - fabsf(x) - fabsf(y);
    const float t = std::max(-z, 0.f);
    x += copysignf(t, -x); y += copysignf(t, -y);
    return Unit(F3(x, y, z));
}
struct Mesh {
    const float *points, *normals, *uv;
    const unsigned int *triangles;
    const bool *smooth;
    float *tangents, *signs;
    int count;
    bool packed;
    int GetNumFaces() const { return count; }
    int GetNumVerticesOfFace(int) const { return 3; }
    bool has_uv() const { return uv != nullptr; }
    F3 GetPosition(int f, int v) const {
        const float *p = points + 3 * triangles[f * 3 + v];
        return F3(p[0], p[1], p[2]);
    }
    F3 GetNormal(int f, int v) const {
        if (smooth && !smooth[f]) {
            const F3 a = GetPosition(f, 1) - GetPosition(f, 0);
            const F3 b = GetPosition(f, 2) - GetPosition(f, 0);
            const F3 n(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
            return n.length_squared() > 0.f ? n.normalize() : F3(1.f, 0.f, 0.f);
        }
        const float *n = normals + (f * 3 + v) * 3;
        const F3 raw(n[0], n[1], n[2]);
        return packed ? PackedNormalRoundTrip(raw) : raw;
    }
    F3 GetTexCoord(int f, int v) const {
        if (uv) {
            const float *p = uv + (f * 3 + v) * 2;
            return F3(p[0], p[1], 1.f);
        }
        // Cycles math_float3.h::map_to_sphere on original object positions.
        const F3 p = GetPosition(f, v);
        const float l = p.length_squared();
        float u = 0.f, value = 0.f;
        if (l > 0.f) {
            if (p.x != 0.f || p.y != 0.f)
                u = .5f - atan2f(p.x, p.y) * 0.15915494309189533577f;
            value = 1.f - acosf(std::clamp(p.z / sqrtf(l), -1.f, 1.f)) * 0.31830988618379067154f;
        }
        return F3(u, value, 1.f);
    }
    void SetTangentSpace(int f, int v, F3 t, bool orientation) {
        float *p = tangents + (f * 3 + v) * 3;
        p[0] = t.x; p[1] = t.y; p[2] = t.z;
        signs[f * 3 + v] = orientation ? 1.f : -1.f;
    }
};
using Floats = py::array_t<float, py::array::c_style | py::array::forcecast>;
using UInts = py::array_t<unsigned int, py::array::c_style | py::array::forcecast>;
py::tuple Compute(Floats points, UInts triangles, Floats normals, py::object uvObject,
                  py::object smoothObject, const bool packed) {
    if (points.ndim() != 2 || points.shape(1) != 3 || triangles.ndim() != 2 || triangles.shape(1) != 3)
        throw std::runtime_error("Mikk positions/triangles must have shape (N, 3)");
    const py::ssize_t count = triangles.shape(0);
    if (count > std::numeric_limits<int>::max() / 4)
        throw std::runtime_error("Mikk triangle count exceeds index capacity");
    if (normals.ndim() != 3 || normals.shape(0) != count || normals.shape(1) != 3 || normals.shape(2) != 3)
        throw std::runtime_error("Mikk normals must have shape (triangles, 3, 3)");
    Floats uv;
    py::array_t<bool, py::array::c_style | py::array::forcecast> smooth;
    const float *uvData = nullptr;
    const bool *smoothData = nullptr;
    if (!uvObject.is_none()) {
        uv = py::cast<Floats>(uvObject);
        if (uv.ndim() != 3 || uv.shape(0) != count || uv.shape(1) != 3 || uv.shape(2) != 2)
            throw std::runtime_error("Mikk UVs must have shape (triangles, 3, 2)");
        uvData = uv.data();
    }
    if (!smoothObject.is_none()) {
        smooth = py::cast<decltype(smooth)>(smoothObject);
        if (smooth.ndim() != 1 || smooth.shape(0) != count)
            throw std::runtime_error("Mikk smoothing flags must have shape (triangles,)");
        smoothData = smooth.data();
    }
    for (py::ssize_t i = 0; i < triangles.size(); ++i)
        if (triangles.data()[i] >= points.shape(0))
            throw std::runtime_error("Mikk triangle vertex index is out of range");
    for (py::ssize_t i = 0; i < points.size(); ++i)
        if (!std::isfinite(points.data()[i])) throw std::runtime_error("Mikk positions must be finite");
    for (py::ssize_t i = 0; i < normals.size(); ++i)
        if (!std::isfinite(normals.data()[i])) throw std::runtime_error("Mikk normals must be finite");
    for (py::ssize_t i = 0; uvData && i < uv.size(); ++i)
        if (!std::isfinite(uvData[i])) throw std::runtime_error("Mikk UVs must be finite");
    py::array_t<float> tangents({count, py::ssize_t(3), py::ssize_t(3)});
    py::array_t<float> signs({count, py::ssize_t(3)});
    Mesh mesh{points.data(), normals.data(), uvData, triangles.data(), smoothData,
              tangents.mutable_data(), signs.mutable_data(), static_cast<int>(count), packed};
    {
        py::gil_scoped_release release;
        mikk::Mikktspace<Mesh>(mesh).genTangSpace();
    }
    return py::make_tuple(tangents, signs);
}
}
void RegisterMikkTangents(py::module_ &m) {
    m.def("ComputeMikkTangents", &Compute, py::arg("points"), py::arg("triangles"), py::arg("normals"),
          py::arg("uv") = py::none(), py::arg("smooth") = py::none(),
          py::arg("cycles_normal_precision") = false,
          "Compute Mikk triangle-corner tangents without changing source mesh data.");
}
