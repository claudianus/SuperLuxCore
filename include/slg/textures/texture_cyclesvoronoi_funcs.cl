// SPDX-License-Identifier: Apache-2.0
// CPU와 GPU가 공유하는 차원별 셀 거리·색·위치 평가다.
// 셀 씨앗은 Blender 5.2의 좌표 계약을 유지하며 렌더 추정기는 엔진 고유 경로를 사용한다.

typedef struct {
    float distance;
    float color[3];
    float position[4];
} CyclesVoronoiResult;

// 1D는 부동소수점 Jenkins, 2~4D는 정수 PCG 셀 씨앗을 사용한다.
// 음수 정수의 이동과 넘침은 명시적인 무부호 연산으로 장치 간 의미를 보존한다.
#define CYCLESVORONOI_ROT(x, k) (((x) << (k)) | ((x) >> (32u - (k))))
#define CYCLESVORONOI_FINAL(a, b, c) { \
    c ^= b; c -= CYCLESVORONOI_ROT(b, 14u); \
    a ^= c; a -= CYCLESVORONOI_ROT(c, 11u); \
    b ^= a; b -= CYCLESVORONOI_ROT(a, 25u); \
    c ^= b; c -= CYCLESVORONOI_ROT(b, 16u); \
    a ^= c; a -= CYCLESVORONOI_ROT(c, 4u); \
    b ^= a; b -= CYCLESVORONOI_ROT(a, 14u); \
    c ^= b; c -= CYCLESVORONOI_ROT(b, 24u); }

OPENCL_FORCE_INLINE float CyclesVoronoi_Hash1D(const float cell, const uint channel) {
    uint a, b, c;
    a = b = c = 0xdeadbeefu + (channel == 0u ? 4u : 8u) + 13u;
    a += as_uint(cell);
    if (channel != 0u) b += as_uint((float)channel);
    CYCLESVORONOI_FINAL(a, b, c);
    return (float)c * (1.f / (float)0xffffffffu);
}
#undef CYCLESVORONOI_ROT
#undef CYCLESVORONOI_FINAL

OPENCL_FORCE_INLINE void CyclesVoronoi_PCG(const float *cell, const uint dims, float *value) {
    uint v[4];
    for (uint d = 0u; d < 4u; ++d)
        v[d] = (uint)((int)clamp(cell[d], -2147483648.f, 2147483520.f)) * 1664525u + 1013904223u;
    for (uint round = 0u; round < 2u; ++round) {
        if (dims == 2u) {
            v[0] += v[1] * 1664525u; v[1] += v[0] * 1664525u;
        } else if (dims == 3u) {
            v[0] += v[1] * v[2]; v[1] += v[2] * v[0]; v[2] += v[0] * v[1];
        } else {
            v[0] += v[1] * v[3]; v[1] += v[2] * v[0];
            v[2] += v[0] * v[1]; v[3] += v[1] * v[2];
        }
        if (round == 0u) for (uint d = 0u; d < dims; ++d)
            v[d] ^= (v[d] >> 16u) | (v[d] & 0x80000000u ? 0xffff0000u : 0u);
    }
    for (uint d = 0u; d < 4u; ++d) value[d] = (float)(v[d] & 0x7fffffffu) * (1.f / (float)0x7fffffffu);
}

OPENCL_FORCE_INLINE float CyclesVoronoi_Distance(const float *a, const float *b,
        const uint dims, const uint metric, const float exponent) {
    float sum = 0.f;
    for (uint d = 0u; d < dims; ++d) {
        const float v = fabs(a[d] - b[d]);
        sum = metric == 2u ? fmax(sum, v) : sum +
                (metric == 0u ? v * v : metric == 3u ? pow(v, fmax(exponent, 1.e-6f)) : v);
    }
    return metric == 0u ? sqrt(sum) : metric == 3u ? pow(sum, 1.f / fmax(exponent, 1.e-6f)) : sum;
}

OPENCL_FORCE_INLINE CyclesVoronoiResult CyclesVoronoi_Empty() {
    CyclesVoronoiResult result;
    result.distance = 0.f;
    for (uint d = 0u; d < 3u; ++d) result.color[d] = 0.f;
    for (uint d = 0u; d < 4u; ++d) result.position[d] = 0.f;
    return result;
}

OPENCL_FORCE_INLINE void CyclesVoronoi_Point(const float *cell, const int *offset,
        const uint dims, const float randomness, const uint colorNeeded, float *point, float *color) {
    float key[4] = {0.f, 0.f, 0.f, 0.f};
    for (uint d = 0u; d < dims; ++d) key[d] = cell[d] + (float)offset[d];
    float random[4] = {0.f, 0.f, 0.f, 0.f};
    if (dims == 1u) {
        random[0] = CyclesVoronoi_Hash1D(key[0], 0u);
        for (uint d = 0u; d < 3u; ++d) color[d] = colorNeeded ? CyclesVoronoi_Hash1D(key[0], d) : 0.f;
    } else {
        if (randomness != 0.f || (colorNeeded && dims != 2u)) CyclesVoronoi_PCG(key, dims, random);
        if (colorNeeded && dims == 2u) {
            float colorRandom[4];
            CyclesVoronoi_PCG(key, 3u, colorRandom);
            for (uint d = 0u; d < 3u; ++d) color[d] = colorRandom[d];
        } else for (uint d = 0u; d < 3u; ++d) color[d] = colorNeeded ? random[d] : 0.f;
    }
    for (uint d = 0u; d < 4u; ++d)
        point[d] = d < dims ? (float)offset[d] + random[d] * randomness : 0.f;
}

OPENCL_FORCE_INLINE CyclesVoronoiResult CyclesVoronoi_Octave(const float *coordinate,
        const uint dims, const uint feature, const uint metric, const float exponent,
        const float randomness, const float smoothness, const uint colorNeeded) {
    CyclesVoronoiResult result = CyclesVoronoi_Empty();
    float cell[4] = {0.f, 0.f, 0.f, 0.f}, local[4] = {0.f, 0.f, 0.f, 0.f};
    for (uint d = 0u; d < dims; ++d) {
        cell[d] = floor(coordinate[d]); local[d] = coordinate[d] - cell[d];
    }
    const uint smooth = feature == 2u && smoothness > 0.f;
    const int radius = smooth ? 2 : 1;
    const int base = radius * 2 + 1;
    uint count = 1u;
    for (uint d = 0u; d < dims; ++d) count *= (uint)base;
    float first = 3.402823466e38f, second = 3.402823466e38f;
    float nearest[4] = {0.f, 0.f, 0.f, 0.f};
    int nearestOffset[4] = {0, 0, 0, 0};
    CyclesVoronoiResult closest = CyclesVoronoi_Empty();
    for (uint n = 0u; n < count; ++n) {
        uint index = n;
        int offset[4] = {0, 0, 0, 0};
        for (uint d = 0u; d < dims; ++d) { offset[d] = (int)(index % (uint)base) - radius; index /= (uint)base; }
        float point[4], color[3];
        CyclesVoronoi_Point(cell, offset, dims, randomness, colorNeeded, point, color);
        const float distance = CyclesVoronoi_Distance(point, local, dims, feature >= 3u ? 0u : metric, exponent);
        if (smooth) {
            float h = n == 0u ? 1.f : clamp(.5f + .5f * (result.distance - distance) / smoothness, 0.f, 1.f);
            h = h * h * (3.f - 2.f * h);
            const float correction = smoothness * h * (1.f - h);
            result.distance += h * (distance - result.distance) - correction;
            for (uint d = 0u; d < 3u; ++d)
                result.color[d] += h * (color[d] - result.color[d]) - correction / (1.f + 3.f * smoothness);
            for (uint d = 0u; d < dims; ++d)
                result.position[d] += h * (point[d] - result.position[d]) - correction / (1.f + 3.f * smoothness);
        } else if (distance < first) {
            second = first; result = closest; first = distance;
            closest.distance = distance;
            for (uint d = 0u; d < 3u; ++d) closest.color[d] = color[d];
            for (uint d = 0u; d < 4u; ++d) {
                closest.position[d] = point[d]; nearest[d] = point[d]; nearestOffset[d] = offset[d];
            }
        } else if (distance < second) {
            second = distance; result.distance = distance;
            for (uint d = 0u; d < 3u; ++d) result.color[d] = color[d];
            for (uint d = 0u; d < 4u; ++d) result.position[d] = point[d];
        }
    }
    if (!smooth && feature != 1u) result = closest;
    if (feature >= 3u) {
        result.distance = 3.402823466e38f;
        for (uint n = 0u; n < count; ++n) {
            uint index = n;
            int offset[4] = {0, 0, 0, 0};
            uint zero = 1u;
            for (uint d = 0u; d < dims; ++d) {
                offset[d] = (int)(index % (uint)base) - radius; index /= (uint)base;
                zero &= offset[d] == 0;
                if (feature == 4u) offset[d] += nearestOffset[d];
            }
            if (feature == 4u && zero) continue;
            float point[4], color[3];
            CyclesVoronoi_Point(cell, offset, dims, randomness, colorNeeded, point, color);
            if (feature == 4u) {
                result.distance = fmin(result.distance, .5f * CyclesVoronoi_Distance(nearest, point, dims, 0u, exponent));
            } else {
                float square = 0.f, numerator = 0.f;
                for (uint d = 0u; d < dims; ++d) {
                    const float edge = point[d] - nearest[d];
                    square += edge * edge;
                    numerator += ((nearest[d] + point[d]) * .5f - local[d]) * edge;
                }
                if (square > 1.e-4f) result.distance = fmin(result.distance, numerator / sqrt(square));
            }
        }
    }
    for (uint d = 0u; d < dims; ++d) result.position[d] += cell[d];
    return result;
}

OPENCL_FORCE_INLINE CyclesVoronoiResult CyclesVoronoi_Evaluate(const float *input,
        const float scale, const float detailIn, const float roughnessIn, const float lacunarity,
        const float smoothnessIn, const float exponent, const float randomnessIn,
        const uint dims, const uint feature, const uint metric, const uint normalize, const uint colorNeeded) {
    const float detail = clamp(detailIn, 0.f, 15.f), roughness = clamp(roughnessIn, 0.f, 1.f);
    const float randomness = clamp(randomnessIn, 0.f, 1.f), smoothness = clamp(smoothnessIn * .5f, 0.f, .5f);
    float coordinate[4], bound[4], zero[4] = {0.f, 0.f, 0.f, 0.f};
    for (uint d = 0u; d < 4u; ++d) { coordinate[d] = input[d] * scale; bound[d] = .5f + .5f * randomness; }
    float maxDistance = feature == 3u ? bound[0] : CyclesVoronoi_Distance(bound, zero, dims, metric, exponent) * (feature == 1u ? 2.f : 1.f);
    if (feature == 4u) return CyclesVoronoi_Octave(coordinate, dims, feature, metric, exponent, randomness, smoothness, colorNeeded);
    CyclesVoronoiResult result = CyclesVoronoi_Empty();
    if (feature == 3u) result.distance = 8.f;
    float amplitude = 1.f, frequency = 1.f, maxAmplitude = feature == 3u ? maxDistance : 0.f;
    for (int octave = 0; octave <= (int)ceil(detail); ++octave) {
        float point[4];
        for (uint d = 0u; d < 4u; ++d) point[d] = coordinate[d] * frequency;
        const CyclesVoronoiResult sample = CyclesVoronoi_Octave(point, dims, feature, metric, exponent, randomness, smoothness, colorNeeded);
        if (detail == 0.f || roughness == 0.f) { result = sample; maxAmplitude = feature == 3u ? maxDistance : 1.f; break; }
        const float fraction = octave <= detail ? 1.f : detail - floor(detail);
        if (feature == 3u) {
            const float distance = frequency != 0.f ? sample.distance / frequency : 0.f;
            const float candidate = result.distance + amplitude * (fmin(result.distance, distance) - result.distance);
            result.distance += fraction * (candidate - result.distance);
            const float boundValue = frequency != 0.f ? maxDistance / frequency : 0.f;
            maxAmplitude += fraction * amplitude * (boundValue - maxAmplitude);
        } else {
            maxAmplitude += amplitude * fraction;
            result.distance += sample.distance * amplitude * fraction;
            for (uint d = 0u; d < 3u; ++d) result.color[d] += sample.color[d] * amplitude * fraction;
            for (uint d = 0u; d < dims; ++d) {
                const float position = frequency != 0.f ? sample.position[d] / frequency : 0.f;
                result.position[d] += amplitude * fraction * (position - result.position[d]);
            }
        }
        frequency *= lacunarity; amplitude *= roughness;
    }
    if (normalize) {
        result.distance /= fmax(maxAmplitude * (feature == 3u ? 1.f : maxDistance), 1.e-8f);
        if (feature != 3u) for (uint d = 0u; d < 3u; ++d) result.color[d] /= fmax(maxAmplitude, 1.e-8f);
    }
    for (uint d = 0u; d < 4u; ++d) result.position[d] = scale != 0.f ? result.position[d] / scale : 0.f;
    return result;
}
