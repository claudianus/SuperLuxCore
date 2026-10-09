/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <cmath>
#include <cstring>

#include "slg/textures/cyclesnoise.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Cycles noise kernel port (see cyclesnoise.h). Keep in sync with
// texture_cyclesnoise_funcs.cl.
//------------------------------------------------------------------------------

namespace {

inline u_int Rot(const u_int x, const u_int k) {
	return (x << k) | (x >> (32u - k));
}

// Jenkins lookup3 final()
inline void HashFinal(u_int &a, u_int &b, u_int &c) {
	c ^= b; c -= Rot(b, 14);
	a ^= c; a -= Rot(c, 11);
	b ^= a; b -= Rot(a, 25);
	c ^= b; c -= Rot(b, 16);
	a ^= c; a -= Rot(c, 4);
	b ^= a; b -= Rot(a, 14);
	c ^= b; c -= Rot(b, 24);
}

inline u_int HashUInt(const u_int kx) {
	u_int a, b, c;
	a = b = c = 0xdeadbeefu + (1u << 2) + 13u;
	a += kx;
	HashFinal(a, b, c);
	return c;
}

inline u_int HashUInt2(const u_int kx, const u_int ky) {
	u_int a, b, c;
	a = b = c = 0xdeadbeefu + (2u << 2) + 13u;
	b += ky;
	a += kx;
	HashFinal(a, b, c);
	return c;
}

inline u_int HashUInt3(const u_int kx, const u_int ky, const u_int kz) {
	u_int a, b, c;
	a = b = c = 0xdeadbeefu + (3u << 2) + 13u;
	c += kz;
	b += ky;
	a += kx;
	HashFinal(a, b, c);
	return c;
}

inline u_int HashUInt4(const u_int kx, const u_int ky, const u_int kz, const u_int kw) {
	u_int a, b, c;
	a = b = c = 0xdeadbeefu + (4u << 2) + 13u;
	a += kx;
	b += ky;
	c += kz;
	// lookup3 mixing round
	a -= c; a ^= Rot(c, 4); c += b;
	b -= a; b ^= Rot(a, 6); a += c;
	c -= b; c ^= Rot(b, 8); b += a;
	a -= c; a ^= Rot(c, 16); c += b;
	b -= a; b ^= Rot(a, 19); a += c;
	c -= b; c ^= Rot(b, 4); b += a;
	a += kw;
	HashFinal(a, b, c);
	return c;
}

inline u_int FloatAsUInt(const float f) {
	u_int u;
	memcpy(&u, &f, sizeof(u));
	return u;
}

inline float UIntToFloatIncl(const u_int n) {
	return (float)n * (1.f / (float)0xFFFFFFFFu);
}

// random_float*_offset(): component i of the seed offset in [100, 200]
inline float RandomOffset(const u_int dims, const float seed, const u_int i) {
	const float h = (dims == 1) ? UIntToFloatIncl(HashUInt(FloatAsUInt(seed))) :
			UIntToFloatIncl(HashUInt2(FloatAsUInt(seed), FloatAsUInt((float)i)));
	return 100.f + h * 100.f;
}

inline float Fade(const float t) {
	return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

inline float NegateIf(const float v, const int c) {
	return c ? -v : v;
}

inline float FloorFrac(const float x, int *i) {
	const float f = floorf(x);
	*i = (int)f;
	return x - f;
}

inline float Mix(const float a, const float b, const float t) {
	return a + t * (b - a);
}

inline float Grad1(const int hash, const float x) {
	const int h = hash & 15;
	const float g = 1 + (h & 7);
	return NegateIf(g, h & 8) * x;
}

inline float Grad2(const int hash, const float x, const float y) {
	const int h = hash & 7;
	const float u = h < 4 ? x : y;
	const float v = 2.f * (h < 4 ? y : x);
	return NegateIf(u, h & 1) + NegateIf(v, h & 2);
}

inline float Grad3(const int hash, const float x, const float y, const float z) {
	const int h = hash & 15;
	const float u = h < 8 ? x : y;
	const float vt = ((h == 12) || (h == 14)) ? x : z;
	const float v = h < 4 ? y : vt;
	return NegateIf(u, h & 1) + NegateIf(v, h & 2);
}

inline float Grad4(const int hash, const float x, const float y, const float z, const float w) {
	const int h = hash & 31;
	const float u = h < 24 ? x : y;
	const float v = h < 16 ? y : z;
	const float s = h < 8 ? z : w;
	return NegateIf(u, h & 1) + NegateIf(v, h & 2) + NegateIf(s, h & 4);
}

inline float BiMix(const float v0, const float v1, const float v2, const float v3,
		const float x, const float y) {
	const float x1 = 1.f - x;
	return (1.f - y) * (v0 * x1 + v1 * x) + y * (v2 * x1 + v3 * x);
}

inline float TriMix(const float v0, const float v1, const float v2, const float v3,
		const float v4, const float v5, const float v6, const float v7,
		const float x, const float y, const float z) {
	const float x1 = 1.f - x;
	const float y1 = 1.f - y;
	const float z1 = 1.f - z;
	return z1 * (y1 * (v0 * x1 + v1 * x) + y * (v2 * x1 + v3 * x)) +
			z * (y1 * (v4 * x1 + v5 * x) + y * (v6 * x1 + v7 * x));
}

float Perlin1D(const float x) {
	int X;
	const float fx = FloorFrac(x, &X);
	const float u = Fade(fx);
	return Mix(Grad1(HashUInt(X), fx), Grad1(HashUInt(X + 1), fx - 1.f), u);
}

float Perlin2D(const float x, const float y) {
	int X, Y;
	const float fx = FloorFrac(x, &X);
	const float fy = FloorFrac(y, &Y);
	const float u = Fade(fx);
	const float v = Fade(fy);
	return BiMix(Grad2(HashUInt2(X, Y), fx, fy),
			Grad2(HashUInt2(X + 1, Y), fx - 1.f, fy),
			Grad2(HashUInt2(X, Y + 1), fx, fy - 1.f),
			Grad2(HashUInt2(X + 1, Y + 1), fx - 1.f, fy - 1.f),
			u, v);
}

float Perlin3D(const float x, const float y, const float z) {
	int X, Y, Z;
	const float fx = FloorFrac(x, &X);
	const float fy = FloorFrac(y, &Y);
	const float fz = FloorFrac(z, &Z);
	const float u = Fade(fx);
	const float v = Fade(fy);
	const float w = Fade(fz);
	return TriMix(Grad3(HashUInt3(X, Y, Z), fx, fy, fz),
			Grad3(HashUInt3(X + 1, Y, Z), fx - 1.f, fy, fz),
			Grad3(HashUInt3(X, Y + 1, Z), fx, fy - 1.f, fz),
			Grad3(HashUInt3(X + 1, Y + 1, Z), fx - 1.f, fy - 1.f, fz),
			Grad3(HashUInt3(X, Y, Z + 1), fx, fy, fz - 1.f),
			Grad3(HashUInt3(X + 1, Y, Z + 1), fx - 1.f, fy, fz - 1.f),
			Grad3(HashUInt3(X, Y + 1, Z + 1), fx, fy - 1.f, fz - 1.f),
			Grad3(HashUInt3(X + 1, Y + 1, Z + 1), fx - 1.f, fy - 1.f, fz - 1.f),
			u, v, w);
}

float Perlin4D(const float x, const float y, const float z, const float w) {
	int X, Y, Z, W;
	const float fx = FloorFrac(x, &X);
	const float fy = FloorFrac(y, &Y);
	const float fz = FloorFrac(z, &Z);
	const float fw = FloorFrac(w, &W);
	const float u = Fade(fx);
	const float v = Fade(fy);
	const float t = Fade(fz);
	const float s = Fade(fw);
	const float r0 = TriMix(
			Grad4(HashUInt4(X, Y, Z, W), fx, fy, fz, fw),
			Grad4(HashUInt4(X + 1, Y, Z, W), fx - 1.f, fy, fz, fw),
			Grad4(HashUInt4(X, Y + 1, Z, W), fx, fy - 1.f, fz, fw),
			Grad4(HashUInt4(X + 1, Y + 1, Z, W), fx - 1.f, fy - 1.f, fz, fw),
			Grad4(HashUInt4(X, Y, Z + 1, W), fx, fy, fz - 1.f, fw),
			Grad4(HashUInt4(X + 1, Y, Z + 1, W), fx - 1.f, fy, fz - 1.f, fw),
			Grad4(HashUInt4(X, Y + 1, Z + 1, W), fx, fy - 1.f, fz - 1.f, fw),
			Grad4(HashUInt4(X + 1, Y + 1, Z + 1, W), fx - 1.f, fy - 1.f, fz - 1.f, fw),
			u, v, t);
	const float r1 = TriMix(
			Grad4(HashUInt4(X, Y, Z, W + 1), fx, fy, fz, fw - 1.f),
			Grad4(HashUInt4(X + 1, Y, Z, W + 1), fx - 1.f, fy, fz, fw - 1.f),
			Grad4(HashUInt4(X, Y + 1, Z, W + 1), fx, fy - 1.f, fz, fw - 1.f),
			Grad4(HashUInt4(X + 1, Y + 1, Z, W + 1), fx - 1.f, fy - 1.f, fz, fw - 1.f),
			Grad4(HashUInt4(X, Y, Z + 1, W + 1), fx, fy, fz - 1.f, fw - 1.f),
			Grad4(HashUInt4(X + 1, Y, Z + 1, W + 1), fx - 1.f, fy, fz - 1.f, fw - 1.f),
			Grad4(HashUInt4(X, Y + 1, Z + 1, W + 1), fx, fy - 1.f, fz - 1.f, fw - 1.f),
			Grad4(HashUInt4(X + 1, Y + 1, Z + 1, W + 1), fx - 1.f, fy - 1.f, fz - 1.f, fw - 1.f),
			u, v, t);
	return Mix(r0, r1, s);
}

// Repeat every 100000 to avoid float precision issues (snoise_*d)
inline float SafeCoord(const float p) {
	const float precisionCorrection = (fabsf(p) >= 1000000.f) ? .5f : 0.f;
	return fmodf(p, 100000.f) + precisionCorrection;
}

// Signed noise in [-1, 1] for the first `dims` components of p
float SNoise(const float p[4], const u_int dims) {
	switch (dims) {
		case 1:
			return .25f * Perlin1D(SafeCoord(p[0]));
		case 2:
			return .6616f * Perlin2D(SafeCoord(p[0]), SafeCoord(p[1]));
		case 4:
			return .8344f * Perlin4D(SafeCoord(p[0]), SafeCoord(p[1]),
					SafeCoord(p[2]), SafeCoord(p[3]));
		default:
			return .982f * Perlin3D(SafeCoord(p[0]), SafeCoord(p[1]),
					SafeCoord(p[2]));
	}
}

inline void Scale4(const float p[4], const float s, float r[4]) {
	r[0] = p[0] * s; r[1] = p[1] * s; r[2] = p[2] * s; r[3] = p[3] * s;
}

float NoiseFBM(const float p[4], const u_int dims, const float detail,
		const float roughness, const float lacunarity, const bool normalize) {
	float fscale = 1.f;
	float amp = 1.f;
	float maxamp = 0.f;
	float sum = 0.f;
	float q[4];

	for (int i = 0; i <= (int)detail; i++) {
		Scale4(p, fscale, q);
		const float t = SNoise(q, dims);
		sum += t * amp;
		maxamp += amp;
		amp *= roughness;
		fscale *= lacunarity;
	}
	const float rmd = detail - floorf(detail);
	if (rmd != 0.f) {
		Scale4(p, fscale, q);
		const float t = SNoise(q, dims);
		const float sum2 = sum + t * amp;
		return normalize ?
				Mix(.5f * sum / maxamp + .5f, .5f * sum2 / (maxamp + amp) + .5f, rmd) :
				Mix(sum, sum2, rmd);
	}
	return normalize ? .5f * sum / maxamp + .5f : sum;
}

float NoiseMultiFractal(const float pIn[4], const u_int dims, const float detail,
		const float roughness, const float lacunarity) {
	float p[4] = { pIn[0], pIn[1], pIn[2], pIn[3] };
	float value = 1.f;
	float pwr = 1.f;

	for (int i = 0; i <= (int)detail; i++) {
		value *= (pwr * SNoise(p, dims) + 1.f);
		pwr *= roughness;
		Scale4(p, lacunarity, p);
	}

	const float rmd = detail - floorf(detail);
	if (rmd != 0.f)
		value *= (rmd * pwr * SNoise(p, dims) + 1.f);

	return value;
}

float NoiseHeteroTerrain(const float pIn[4], const u_int dims, const float detail,
		const float roughness, const float lacunarity, const float offset) {
	float p[4] = { pIn[0], pIn[1], pIn[2], pIn[3] };
	float pwr = roughness;

	// First unscaled octave of function; later octaves are scaled
	float value = offset + SNoise(p, dims);
	Scale4(p, lacunarity, p);

	for (int i = 1; i <= (int)detail; i++) {
		const float increment = (SNoise(p, dims) + offset) * pwr * value;
		value += increment;
		pwr *= roughness;
		Scale4(p, lacunarity, p);
	}

	const float rmd = detail - floorf(detail);
	if (rmd != 0.f) {
		const float increment = (SNoise(p, dims) + offset) * pwr * value;
		value += rmd * increment;
	}

	return value;
}

float NoiseHybridMultiFractal(const float pIn[4], const u_int dims, const float detail,
		const float roughness, const float lacunarity, const float offset,
		const float gain) {
	float p[4] = { pIn[0], pIn[1], pIn[2], pIn[3] };
	float pwr = 1.f;
	float value = 0.f;
	float weight = 1.f;

	for (int i = 0; (weight > .001f) && (i <= (int)detail); i++) {
		weight = fminf(weight, 1.f);

		const float signal = (SNoise(p, dims) + offset) * pwr;
		pwr *= roughness;
		value += weight * signal;
		weight *= gain * signal;
		Scale4(p, lacunarity, p);
	}

	const float rmd = detail - floorf(detail);
	if ((rmd != 0.f) && (weight > .001f)) {
		weight = fminf(weight, 1.f);
		const float signal = (SNoise(p, dims) + offset) * pwr;
		value += rmd * weight * signal;
	}

	return value;
}

float NoiseRidgedMultiFractal(const float pIn[4], const u_int dims, const float detail,
		const float roughness, const float lacunarity, const float offset,
		const float gain) {
	float p[4] = { pIn[0], pIn[1], pIn[2], pIn[3] };
	float pwr = roughness;

	float signal = offset - fabsf(SNoise(p, dims));
	signal *= signal;
	float value = signal;
	float weight = 1.f;

	for (int i = 1; i <= (int)detail; i++) {
		Scale4(p, lacunarity, p);
		weight = Clamp(signal * gain, 0.f, 1.f);
		signal = offset - fabsf(SNoise(p, dims));
		signal *= signal;
		signal *= weight;
		value += signal * pwr;
		pwr *= roughness;
	}

	return value;
}

float NoiseSelect(const float p[4], const u_int dims, const float detail,
		const float roughness, const float lacunarity, const float offset,
		const float gain, const CyclesNoiseType type, const bool normalize) {
	switch (type) {
		case CYCLESNOISE_MULTIFRACTAL:
			return NoiseMultiFractal(p, dims, detail, roughness, lacunarity);
		case CYCLESNOISE_HYBRID_MULTIFRACTAL:
			return NoiseHybridMultiFractal(p, dims, detail, roughness, lacunarity, offset, gain);
		case CYCLESNOISE_RIDGED_MULTIFRACTAL:
			return NoiseRidgedMultiFractal(p, dims, detail, roughness, lacunarity, offset, gain);
		case CYCLESNOISE_HETERO_TERRAIN:
			return NoiseHeteroTerrain(p, dims, detail, roughness, lacunarity, offset);
		default:
			return NoiseFBM(p, dims, detail, roughness, lacunarity, normalize);
	}
}

// p + random_float*_offset(seed)
inline void AddSeedOffset(const float p[4], const u_int dims, const float seed,
		float r[4]) {
	for (u_int i = 0; i < 4; ++i)
		r[i] = (i < dims) ? p[i] + RandomOffset(dims, seed, i) : p[i];
}

}

// Cycles svm_wave()
float CyclesNoiseTexture::Wave(const float pIn[3], const u_int waveMode,
		const float distortion, const float detail, const float detailScale,
		const float detailRoughness, const float phase) {
	// Prevent precision issues on unit coordinates
	const float p[3] = { (pIn[0] + 1e-6f) * .999999f, (pIn[1] + 1e-6f) * .999999f,
			(pIn[2] + 1e-6f) * .999999f };
	const u_int dir = (waveMode >> 1) & 3u;
	float n;
	if (!(waveMode & 1u)) {
		// Bands
		n = (dir == 0u) ? p[0] * 20.f : (dir == 1u) ? p[1] * 20.f :
				(dir == 2u) ? p[2] * 20.f : (p[0] + p[1] + p[2]) * 10.f;
	} else {
		// Rings (dir 3 = spherical)
		const float rx = (dir == 0u) ? 0.f : p[0];
		const float ry = (dir == 1u) ? 0.f : p[1];
		const float rz = (dir == 2u) ? 0.f : p[2];
		n = sqrtf(rx * rx + ry * ry + rz * rz) * 20.f;
	}
	n += phase;
	if (distortion != 0.f) {
		float q[4] = { p[0] * detailScale, p[1] * detailScale, p[2] * detailScale, 0.f };
		n += distortion * (NoiseFBM(q, 3, Clamp(detail, 0.f, 15.f),
				fmaxf(detailRoughness, 0.f), 2.f, true) * 2.f - 1.f);
	}
	const u_int profile = (waveMode >> 3) & 3u;
	if (profile == 0u)
		return .5f + .5f * sinf(n - .5f * M_PI);
	n *= .5f * INV_PI;
	if (profile == 1u)
		return n - floorf(n);
	return fabsf(n - floorf(n + .5f)) * 2.f;
}

void CyclesNoiseTexture::Evaluate(const float pIn[4], const float scale,
		const float detailIn, const float roughnessIn, const float lacunarity,
		const float offset, const float gain, const float distortion,
		const CyclesNoiseType noiseType, const u_int dims,
		const bool normalize, const bool colorNeeded,
		float &value, float color[3], const u_int waveMode) {
	// White Noise는 스케일·왜곡 없이 입력 비트를 직접 해시한다.
	if (noiseType == CYCLESNOISE_WHITENOISE) {
		const u_int x = FloatAsUInt(pIn[0]), y = FloatAsUInt(pIn[1]);
		const u_int z = FloatAsUInt(pIn[2]), w = FloatAsUInt(pIn[3]);
		value = UIntToFloatIncl(dims == 1u ? HashUInt(x) : dims == 2u ?
				HashUInt2(x, y) : dims == 3u ? HashUInt3(x, y, z) : HashUInt4(x, y, z, w));
		color[0] = value;
		color[1] = UIntToFloatIncl(dims == 1u ? HashUInt2(x, FloatAsUInt(1.f)) : dims == 2u ?
				HashUInt3(x, y, FloatAsUInt(1.f)) : dims == 3u ? HashUInt4(x, y, z, FloatAsUInt(1.f)) :
				HashUInt4(z, x, w, y));
		color[2] = UIntToFloatIncl(dims == 1u ? HashUInt2(x, FloatAsUInt(2.f)) : dims == 2u ?
				HashUInt3(x, y, FloatAsUInt(2.f)) : dims == 3u ? HashUInt4(x, y, z, FloatAsUInt(2.f)) :
				HashUInt4(w, z, y, x));
		return;
	}
	const float detail = Clamp(detailIn, 0.f, 15.f);
	const float roughness = fmaxf(roughnessIn, 0.f);

	float p[4];
	Scale4(pIn, scale, p);

	if (noiseType == CYCLESNOISE_WAVE) {
		value = Wave(p, waveMode, distortion, detail, gain, roughness, offset);
		color[0] = color[1] = color[2] = value;
		return;
	}

	if (distortion != 0.f) {
		// Distortion seeds 0..dims-1, computed from the undistorted p
		float d[4] = { 0.f, 0.f, 0.f, 0.f };
		for (u_int i = 0; i < dims; ++i) {
			float q[4];
			AddSeedOffset(p, dims, (float)i, q);
			d[i] = SNoise(q, dims) * distortion;
		}
		for (u_int i = 0; i < dims; ++i)
			p[i] += d[i];
	}

	value = NoiseSelect(p, dims, detail, roughness, lacunarity, offset, gain,
			noiseType, normalize);
	if (colorNeeded) {
		float q[4];
		color[0] = value;
		AddSeedOffset(p, dims, (float)dims, q);
		color[1] = NoiseSelect(q, dims, detail, roughness, lacunarity, offset,
				gain, noiseType, normalize);
		AddSeedOffset(p, dims, (float)(dims + 1), q);
		color[2] = NoiseSelect(q, dims, detail, roughness, lacunarity, offset,
				gain, noiseType, normalize);
	}
}

CyclesNoiseTexture::CyclesNoiseTexture(TextureRef v, TextureRef wt,
		TextureRef s, TextureRef d, TextureRef r, TextureRef l, TextureRef o,
		TextureRef g, TextureRef dist, const CyclesNoiseType t, const u_int dims,
		const bool norm, const bool colOut, const bool isCol, const u_int wm) :
		vec(v), w(wt), scale(s), detail(d), roughness(r), lacunarity(l),
		offset(o), gain(g), distortion(dist), noiseType(t),
		dimensions(Clamp(dims, 1u, 4u)), normalize(norm), colorOutput(colOut),
		isColor(isCol), waveMode(wm) { }

void CyclesNoiseTexture::EvalInputs(const HitPoint &hitPoint, float &value,
		float color[3], const bool colorNeeded) const {
	// Coordinates are a vector, never upsampled
	Spectrum v;
	{
		Spectral::ScopePause pause;
		v = vec.get().GetSpectrumValue(hitPoint);
	}
	// 1D noise reads only W; 4D appends W to the vector
	float p[4] = { v.c[0], v.c[1], v.c[2], 0.f };
	if (dimensions == 1)
		p[0] = w.get().GetFloatValue(hitPoint);
	else if (dimensions == 4)
		p[3] = w.get().GetFloatValue(hitPoint);

	Evaluate(p, scale.get().GetFloatValue(hitPoint),
			detail.get().GetFloatValue(hitPoint),
			roughness.get().GetFloatValue(hitPoint),
			lacunarity.get().GetFloatValue(hitPoint),
			offset.get().GetFloatValue(hitPoint),
			gain.get().GetFloatValue(hitPoint),
			distortion.get().GetFloatValue(hitPoint),
			noiseType, dimensions, normalize, colorNeeded, value, color, waveMode);
}

float CyclesNoiseTexture::GetFloatValue(const HitPoint &hitPoint) const {
	float value, color[3];
	EvalInputs(hitPoint, value, color, colorOutput);
	return colorOutput ? Spectrum(color).Y() : value;
}

Spectrum CyclesNoiseTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	float value, color[3];
	EvalInputs(hitPoint, value, color, colorOutput);
	return colorOutput ? Spectrum(color) : Spectrum(value);
}

Spectrum CyclesNoiseTexture::EvalSpectralValue(const HitPoint &hitPoint,
		const PathWavelengths &sw, const bool emission) const {
	const Spectrum v = EvalSpectrumValue(hitPoint);
	if (!(colorOutput && isColor))
		return v;
	return emission ? Spectral::Emission(v, sw) : Spectral::Reflectance(v, sw);
}

void CyclesNoiseTexture::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Texture::AddReferencedTextures(referencedTexs);
	for (const Texture *t : { &vec.get(), &w.get(), &scale.get(), &detail.get(),
			&roughness.get(), &lacunarity.get(), &offset.get(), &gain.get(),
			&distortion.get() })
		t->AddReferencedTextures(referencedTexs);
}

void CyclesNoiseTexture::AddReferencedImageMaps(std::unordered_set<const ImageMap * > &referencedImgMaps) const {
	for (const Texture *t : { &vec.get(), &w.get(), &scale.get(), &detail.get(),
			&roughness.get(), &lacunarity.get(), &offset.get(), &gain.get(),
			&distortion.get() })
		t->AddReferencedImageMaps(referencedImgMaps);
}

void CyclesNoiseTexture::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	updtex(vec, oldTex, newTex);
	updtex(w, oldTex, newTex);
	updtex(scale, oldTex, newTex);
	updtex(detail, oldTex, newTex);
	updtex(roughness, oldTex, newTex);
	updtex(lacunarity, oldTex, newTex);
	updtex(offset, oldTex, newTex);
	updtex(gain, oldTex, newTex);
	updtex(distortion, oldTex, newTex);
}

PropertiesUPtr CyclesNoiseTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	const string prefix = "scene.textures." + name;
	props->Set(Property(prefix + ".type")("cyclesnoise"));
	props->Set(Property(prefix + ".vector")(vec.get().GetSDLValue()));
	props->Set(Property(prefix + ".w")(w.get().GetSDLValue()));
	props->Set(Property(prefix + ".scale")(scale.get().GetSDLValue()));
	props->Set(Property(prefix + ".detail")(detail.get().GetSDLValue()));
	props->Set(Property(prefix + ".roughness")(roughness.get().GetSDLValue()));
	props->Set(Property(prefix + ".lacunarity")(lacunarity.get().GetSDLValue()));
	props->Set(Property(prefix + ".offset")(offset.get().GetSDLValue()));
	props->Set(Property(prefix + ".gain")(gain.get().GetSDLValue()));
	props->Set(Property(prefix + ".distortion")(distortion.get().GetSDLValue()));
	static const char *types[] = { "fbm", "multifractal", "hybrid_multifractal",
			"ridged_multifractal", "hetero_terrain", "wave", "white" };
	props->Set(Property(prefix + ".noisetype")(types[noiseType]));
	if (noiseType == CYCLESNOISE_WAVE)
		props->Set(Property(prefix + ".wavemode")(waveMode));
	props->Set(Property(prefix + ".dimensions")(dimensions));
	props->Set(Property(prefix + ".normalize")(normalize));
	props->Set(Property(prefix + ".output")(colorOutput ? "color" : "fac"));
	props->Set(Property(prefix + ".color")(isColor));

	return props;
}
