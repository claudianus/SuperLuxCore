// Copyright 2026 LuxCoreRender authors. Apache-2.0 (see COPYING.txt).
// Kept separate from the native triangle/curve shaders: this diagnostic path
// requires strict arithmetic and macOS 13 per-primitive acceleration data.
static const char *HWRT_MSL_SOURCE_PRECISE = R"MSL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;

struct LuxRay {
    float ox, oy, oz, dx, dy, dz, mint, maxt, time;
    uint flags;
    float pad0, pad1;
};
struct LuxRayHit {
    float t, b1, b2;
    uint meshIndex, triangleIndex;
};
struct PreciseTriangle { float v[9]; };

// Two-component float expansion. Strict math is required: reassociation would
// destroy the rounding residuals. FMA captures each product's low component.
struct Wide { float hi, lo; };
inline Wide wide(float a) { return {a, 0.f}; }
inline Wide wide_normalize(float a, float b) {
    const float s = a + b;
    return {s, b - (s - a)};
}
inline Wide wide_add(Wide a, Wide b) {
    const float s = a.hi + b.hi;
    const float v = s - a.hi;
    const float e = (a.hi - (s - v)) + (b.hi - v);
    return wide_normalize(s, (e + a.lo) + b.lo);
}
inline Wide wide_sub(Wide a, Wide b) {
    return wide_add(a, {-b.hi, -b.lo});
}
inline Wide wide_mul(Wide a, Wide b) {
    const float p = a.hi * b.hi;
    const float e = ((fma(a.hi, b.hi, -p) + a.hi*b.lo) + a.lo*b.hi) + a.lo*b.lo;
    return wide_normalize(p, e);
}
inline Wide wide_div(Wide a, Wide b) {
    const float q = a.hi / b.hi;
    const Wide r = wide_sub(a, wide_mul(b, wide(q)));
    return wide_normalize(q, (r.hi + r.lo) / b.hi);
}
inline bool wide_less(Wide a, Wide b) {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}
inline float wide_float(Wide a) { return a.hi + a.lo; }
inline int wide_sign(Wide a) {
    return a.hi > 0.f ? 1 : (a.hi < 0.f ? -1 : (a.lo > 0.f ? 1 : (a.lo < 0.f ? -1 : 0)));
}

// Ray-aligned projection preserves the ray and original t interval. There is
// no barycentric tolerance and no primitive-ID rejection or physical offset.
inline bool precise_triangle(const device PreciseTriangle &tri, float3 origin,
        float3 direction, float minT, float maxT, thread float &distance,
        thread float2 &bary) {
    uint kz = 0;
    if (abs(direction.y) > abs(direction[kz])) kz = 1;
    if (abs(direction.z) > abs(direction[kz])) kz = 2;
    if (direction[kz] == 0.f) return false;
    uint kx = (kz + 1) % 3, ky = (kx + 1) % 3;
    if (direction[kz] < 0.f) { uint tmp = kx; kx = ky; ky = tmp; }
    const Wide sx = wide_div(wide(direction[kx]), wide(direction[kz]));
    const Wide sy = wide_div(wide(direction[ky]), wide(direction[kz]));
    Wide x[3], y[3], z[3];
    for (uint i = 0; i < 3; ++i) {
        const Wide px = wide_sub(wide(tri.v[3*i+kx]), wide(origin[kx]));
        const Wide py = wide_sub(wide(tri.v[3*i+ky]), wide(origin[ky]));
        const Wide pz = wide_sub(wide(tri.v[3*i+kz]), wide(origin[kz]));
        x[i] = wide_sub(px, wide_mul(sx, pz));
        y[i] = wide_sub(py, wide_mul(sy, pz));
        z[i] = wide_div(pz, wide(direction[kz]));
    }
    const Wide e0 = wide_sub(wide_mul(x[1],y[2]), wide_mul(y[1],x[2]));
    const Wide e1 = wide_sub(wide_mul(x[2],y[0]), wide_mul(y[2],x[0]));
    const Wide e2 = wide_sub(wide_mul(x[0],y[1]), wide_mul(y[0],x[1]));
    const int s0 = wide_sign(e0), s1 = wide_sign(e1), s2 = wide_sign(e2);
    if ((s0 < 0 || s1 < 0 || s2 < 0) && (s0 > 0 || s1 > 0 || s2 > 0)) return false;
    const Wide det = wide_add(wide_add(e0,e1),e2);
    if (wide_sign(det) == 0 || !isfinite(det.hi) || !isfinite(det.lo)) return false;
    const Wide t = wide_div(wide_add(wide_add(wide_mul(e0,z[0]),
            wide_mul(e1,z[1])),wide_mul(e2,z[2])),det);
    if (!isfinite(t.hi) || !isfinite(t.lo) || wide_less(t,wide(minT)) ||
            wide_less(wide(maxT),t)) return false;
    distance = wide_float(t);
    bary = float2(wide_float(wide_div(e1,det)), wide_float(wide_div(e2,det)));
    return all(isfinite(bary)) && isfinite(distance);
}

struct PreciseResult {
    bool accept [[accept_intersection]];
    bool continue_search [[continue_search]];
    float distance [[distance]];
};
struct PrecisePayload { float2 bary; };
[[intersection(bounding_box, raytracing::instancing)]]
PreciseResult PreciseTriangleIntersection(float3 origin [[origin]],
        float3 direction [[direction]], float minT [[min_distance]],
        float maxT [[max_distance]],
        const device PreciseTriangle *tri [[primitive_data]],
        ray_data PrecisePayload &payload [[payload]]) {
    float t; float2 bary;
    if (!precise_triangle(*tri,origin,direction,minT,maxT,t,bary))
        return {false,true,0.f};
    payload.bary = bary;
    return {true,true,t};
}

kernel void Accelerator_Intersect_RayBuffer_HWRT(
        device const LuxRay *rays [[buffer(0)]],
        device LuxRayHit *rayHits [[buffer(1)]],
        constant uint &rayCount [[buffer(2)]],
        raytracing::instance_acceleration_structure sceneAS [[buffer(3)]],
        raytracing::intersection_function_table<raytracing::instancing> table [[buffer(4)]],
        uint gid [[thread_position_in_grid]]) {
    if (gid >= rayCount) return;
    const LuxRay r = rays[gid];
    if (r.flags & 1u) return;
    raytracing::ray ray;
    ray.origin = float3(r.ox,r.oy,r.oz);
    ray.direction = float3(r.dx,r.dy,r.dz);
    ray.min_distance = r.mint;
    ray.max_distance = r.maxt;
    PrecisePayload payload;
    raytracing::intersector<raytracing::instancing> itr;
    const auto hit = itr.intersect(ray,sceneAS,table,payload);
    if (hit.type == raytracing::intersection_type::none) {
        rayHits[gid].t = r.maxt;
        rayHits[gid].meshIndex = rayHits[gid].triangleIndex = 0xffffffffu;
        return;
    }
    rayHits[gid].t = hit.distance;
    rayHits[gid].b1 = payload.bary.x;
    rayHits[gid].b2 = payload.bary.y;
    rayHits[gid].meshIndex = hit.instance_id;
    rayHits[gid].triangleIndex = hit.primitive_id;
}
)MSL";
