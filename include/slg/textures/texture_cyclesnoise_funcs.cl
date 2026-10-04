#line 2 "texture_cyclesnoise_funcs.cl"

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

//------------------------------------------------------------------------------
// Cycles Noise Texture node
//
// Mirrors slg::CyclesNoiseTexture (cyclesnoise.cpp): a port of the Cycles
// kernel (svm/noise.h, fractal_noise.h, noisetex.h). Coordinates use a
// float4 whose first `dims` components are live (1D noise lives in .x).
//------------------------------------------------------------------------------

#define CYCLESNOISE_ROT(x, k) (((x) << (k)) | ((x) >> (32u - (k))))

#define CYCLESNOISE_FINAL(a, b, c) { \
	c ^= b; c -= CYCLESNOISE_ROT(b, 14u); \
	a ^= c; a -= CYCLESNOISE_ROT(c, 11u); \
	b ^= a; b -= CYCLESNOISE_ROT(a, 25u); \
	c ^= b; c -= CYCLESNOISE_ROT(b, 16u); \
	a ^= c; a -= CYCLESNOISE_ROT(c, 4u); \
	b ^= a; b -= CYCLESNOISE_ROT(a, 14u); \
	c ^= b; c -= CYCLESNOISE_ROT(b, 24u); }

OPENCL_FORCE_INLINE uint CyclesNoise_HashUInt(const uint kx) {
	uint a, b, c;
	a = b = c = 0xdeadbeefu + (1u << 2) + 13u;
	a += kx;
	CYCLESNOISE_FINAL(a, b, c);
	return c;
}

OPENCL_FORCE_INLINE uint CyclesNoise_HashUInt2(const uint kx, const uint ky) {
	uint a, b, c;
	a = b = c = 0xdeadbeefu + (2u << 2) + 13u;
	b += ky;
	a += kx;
	CYCLESNOISE_FINAL(a, b, c);
	return c;
}

OPENCL_FORCE_INLINE uint CyclesNoise_HashUInt3(const uint kx, const uint ky, const uint kz) {
	uint a, b, c;
	a = b = c = 0xdeadbeefu + (3u << 2) + 13u;
	c += kz;
	b += ky;
	a += kx;
	CYCLESNOISE_FINAL(a, b, c);
	return c;
}

OPENCL_FORCE_INLINE uint CyclesNoise_HashUInt4(const uint kx, const uint ky, const uint kz, const uint kw) {
	uint a, b, c;
	a = b = c = 0xdeadbeefu + (4u << 2) + 13u;
	a += kx;
	b += ky;
	c += kz;
	// lookup3 mixing round
	a -= c; a ^= CYCLESNOISE_ROT(c, 4u); c += b;
	b -= a; b ^= CYCLESNOISE_ROT(a, 6u); a += c;
	c -= b; c ^= CYCLESNOISE_ROT(b, 8u); b += a;
	a -= c; a ^= CYCLESNOISE_ROT(c, 16u); c += b;
	b -= a; b ^= CYCLESNOISE_ROT(a, 19u); a += c;
	c -= b; c ^= CYCLESNOISE_ROT(b, 4u); b += a;
	a += kw;
	CYCLESNOISE_FINAL(a, b, c);
	return c;
}

OPENCL_FORCE_INLINE float CyclesNoise_UIntToFloatIncl(const uint n) {
	return (float)n * (1.f / (float)0xFFFFFFFFu);
}

// random_float*_offset(): component i of the seed offset in [100, 200]
OPENCL_FORCE_INLINE float CyclesNoise_RandomOffset(const uint dims,
		const float seed, const uint i) {
	const float h = (dims == 1u) ?
		CyclesNoise_UIntToFloatIncl(CyclesNoise_HashUInt(as_uint(seed))) :
		CyclesNoise_UIntToFloatIncl(CyclesNoise_HashUInt2(as_uint(seed), as_uint((float)i)));
	return 100.f + h * 100.f;
}

OPENCL_FORCE_INLINE float4 CyclesNoise_AddSeedOffset(const float4 p,
		const uint dims, const float seed) {
	float4 r = p;
	r.x += CyclesNoise_RandomOffset(dims, seed, 0u);
	if (dims > 1u)
		r.y += CyclesNoise_RandomOffset(dims, seed, 1u);
	if (dims > 2u)
		r.z += CyclesNoise_RandomOffset(dims, seed, 2u);
	if (dims > 3u)
		r.w += CyclesNoise_RandomOffset(dims, seed, 3u);
	return r;
}

OPENCL_FORCE_INLINE float CyclesNoise_Fade(const float t) {
	return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

OPENCL_FORCE_INLINE float CyclesNoise_NegateIf(const float v, const int c) {
	return c ? -v : v;
}

OPENCL_FORCE_INLINE float CyclesNoise_FloorFrac(const float x, int *i) {
	const float f = floor(x);
	*i = (int)f;
	return x - f;
}

OPENCL_FORCE_INLINE float CyclesNoise_Mix(const float a, const float b, const float t) {
	return a + t * (b - a);
}

OPENCL_FORCE_INLINE float CyclesNoise_Grad1(const int hash, const float x) {
	const int h = hash & 15;
	const float g = 1 + (h & 7);
	return CyclesNoise_NegateIf(g, h & 8) * x;
}

OPENCL_FORCE_INLINE float CyclesNoise_Grad2(const int hash, const float x, const float y) {
	const int h = hash & 7;
	const float u = h < 4 ? x : y;
	const float v = 2.f * (h < 4 ? y : x);
	return CyclesNoise_NegateIf(u, h & 1) + CyclesNoise_NegateIf(v, h & 2);
}

OPENCL_FORCE_INLINE float CyclesNoise_Grad3(const int hash, const float x, const float y, const float z) {
	const int h = hash & 15;
	const float u = h < 8 ? x : y;
	const float vt = ((h == 12) || (h == 14)) ? x : z;
	const float v = h < 4 ? y : vt;
	return CyclesNoise_NegateIf(u, h & 1) + CyclesNoise_NegateIf(v, h & 2);
}

OPENCL_FORCE_INLINE float CyclesNoise_Grad4(const int hash, const float x, const float y, const float z, const float w) {
	const int h = hash & 31;
	const float u = h < 24 ? x : y;
	const float v = h < 16 ? y : z;
	const float s = h < 8 ? z : w;
	return CyclesNoise_NegateIf(u, h & 1) + CyclesNoise_NegateIf(v, h & 2) + CyclesNoise_NegateIf(s, h & 4);
}

OPENCL_FORCE_INLINE float CyclesNoise_TriMix(const float v0, const float v1,
		const float v2, const float v3, const float v4, const float v5,
		const float v6, const float v7, const float x, const float y, const float z) {
	const float x1 = 1.f - x;
	const float y1 = 1.f - y;
	const float z1 = 1.f - z;
	return z1 * (y1 * (v0 * x1 + v1 * x) + y * (v2 * x1 + v3 * x)) +
			z * (y1 * (v4 * x1 + v5 * x) + y * (v6 * x1 + v7 * x));
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_Perlin1D(const float x) {
	int X;
	const float fx = CyclesNoise_FloorFrac(x, &X);
	const float u = CyclesNoise_Fade(fx);
	return CyclesNoise_Mix(CyclesNoise_Grad1(CyclesNoise_HashUInt((uint)X), fx),
			CyclesNoise_Grad1(CyclesNoise_HashUInt((uint)(X + 1)), fx - 1.f), u);
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_Perlin2D(const float x, const float y) {
	int X, Y;
	const float fx = CyclesNoise_FloorFrac(x, &X);
	const float fy = CyclesNoise_FloorFrac(y, &Y);
	const float u = CyclesNoise_Fade(fx);
	const float v = CyclesNoise_Fade(fy);
	const float v0 = CyclesNoise_Grad2(CyclesNoise_HashUInt2((uint)X, (uint)Y), fx, fy);
	const float v1 = CyclesNoise_Grad2(CyclesNoise_HashUInt2((uint)(X + 1), (uint)Y), fx - 1.f, fy);
	const float v2 = CyclesNoise_Grad2(CyclesNoise_HashUInt2((uint)X, (uint)(Y + 1)), fx, fy - 1.f);
	const float v3 = CyclesNoise_Grad2(CyclesNoise_HashUInt2((uint)(X + 1), (uint)(Y + 1)), fx - 1.f, fy - 1.f);
	const float x1 = 1.f - u;
	return (1.f - v) * (v0 * x1 + v1 * u) + v * (v2 * x1 + v3 * u);
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_Perlin3D(const float x, const float y, const float z) {
	int X, Y, Z;
	const float fx = CyclesNoise_FloorFrac(x, &X);
	const float fy = CyclesNoise_FloorFrac(y, &Y);
	const float fz = CyclesNoise_FloorFrac(z, &Z);
	const float u = CyclesNoise_Fade(fx);
	const float v = CyclesNoise_Fade(fy);
	const float w = CyclesNoise_Fade(fz);
	const uint X0 = (uint)X, X1 = (uint)(X + 1);
	const uint Y0 = (uint)Y, Y1 = (uint)(Y + 1);
	const uint Z0 = (uint)Z, Z1 = (uint)(Z + 1);
	return CyclesNoise_TriMix(
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X0, Y0, Z0), fx, fy, fz),
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X1, Y0, Z0), fx - 1.f, fy, fz),
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X0, Y1, Z0), fx, fy - 1.f, fz),
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X1, Y1, Z0), fx - 1.f, fy - 1.f, fz),
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X0, Y0, Z1), fx, fy, fz - 1.f),
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X1, Y0, Z1), fx - 1.f, fy, fz - 1.f),
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X0, Y1, Z1), fx, fy - 1.f, fz - 1.f),
			CyclesNoise_Grad3(CyclesNoise_HashUInt3(X1, Y1, Z1), fx - 1.f, fy - 1.f, fz - 1.f),
			u, v, w);
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_Perlin4DSlice(const uint X0, const uint Y0,
		const uint Z0, const uint W, const float fx, const float fy,
		const float fz, const float fw, const float u, const float v, const float t) {
	const uint X1 = X0 + 1u, Y1 = Y0 + 1u, Z1 = Z0 + 1u;
	return CyclesNoise_TriMix(
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X0, Y0, Z0, W), fx, fy, fz, fw),
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X1, Y0, Z0, W), fx - 1.f, fy, fz, fw),
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X0, Y1, Z0, W), fx, fy - 1.f, fz, fw),
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X1, Y1, Z0, W), fx - 1.f, fy - 1.f, fz, fw),
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X0, Y0, Z1, W), fx, fy, fz - 1.f, fw),
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X1, Y0, Z1, W), fx - 1.f, fy, fz - 1.f, fw),
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X0, Y1, Z1, W), fx, fy - 1.f, fz - 1.f, fw),
			CyclesNoise_Grad4(CyclesNoise_HashUInt4(X1, Y1, Z1, W), fx - 1.f, fy - 1.f, fz - 1.f, fw),
			u, v, t);
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_Perlin4D(const float x, const float y, const float z, const float w) {
	int X, Y, Z, W;
	const float fx = CyclesNoise_FloorFrac(x, &X);
	const float fy = CyclesNoise_FloorFrac(y, &Y);
	const float fz = CyclesNoise_FloorFrac(z, &Z);
	const float fw = CyclesNoise_FloorFrac(w, &W);
	const float u = CyclesNoise_Fade(fx);
	const float v = CyclesNoise_Fade(fy);
	const float t = CyclesNoise_Fade(fz);
	const float s = CyclesNoise_Fade(fw);
	const float r0 = CyclesNoise_Perlin4DSlice((uint)X, (uint)Y, (uint)Z, (uint)W,
			fx, fy, fz, fw, u, v, t);
	const float r1 = CyclesNoise_Perlin4DSlice((uint)X, (uint)Y, (uint)Z, (uint)(W + 1),
			fx, fy, fz, fw - 1.f, u, v, t);
	return CyclesNoise_Mix(r0, r1, s);
}

// Repeat every 100000 to avoid float precision issues (snoise_*d)
OPENCL_FORCE_INLINE float CyclesNoise_SafeCoord(const float p) {
	const float precisionCorrection = (fabs(p) >= 1000000.f) ? .5f : 0.f;
	return fmod(p, 100000.f) + precisionCorrection;
}

// Signed noise in [-1, 1] for the first `dims` components of p
OPENCL_FORCE_NOT_INLINE float CyclesNoise_SNoise(const float4 p, const uint dims) {
	if (dims == 1u)
		return .25f * CyclesNoise_Perlin1D(CyclesNoise_SafeCoord(p.x));
	else if (dims == 2u)
		return .6616f * CyclesNoise_Perlin2D(CyclesNoise_SafeCoord(p.x),
				CyclesNoise_SafeCoord(p.y));
	else if (dims == 4u)
		return .8344f * CyclesNoise_Perlin4D(CyclesNoise_SafeCoord(p.x),
				CyclesNoise_SafeCoord(p.y), CyclesNoise_SafeCoord(p.z),
				CyclesNoise_SafeCoord(p.w));
	else
		return .982f * CyclesNoise_Perlin3D(CyclesNoise_SafeCoord(p.x),
				CyclesNoise_SafeCoord(p.y), CyclesNoise_SafeCoord(p.z));
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_FBM(const float4 p, const uint dims,
		const float detail, const float roughness, const float lacunarity,
		const uint normalize) {
	float fscale = 1.f;
	float amp = 1.f;
	float maxamp = 0.f;
	float sum = 0.f;

	const int octaves = (int)detail;
	for (int i = 0; i <= octaves; i++) {
		const float t = CyclesNoise_SNoise(fscale * p, dims);
		sum += t * amp;
		maxamp += amp;
		amp *= roughness;
		fscale *= lacunarity;
	}
	const float rmd = detail - floor(detail);
	if (rmd != 0.f) {
		const float t = CyclesNoise_SNoise(fscale * p, dims);
		const float sum2 = sum + t * amp;
		return normalize ?
				CyclesNoise_Mix(.5f * sum / maxamp + .5f, .5f * sum2 / (maxamp + amp) + .5f, rmd) :
				CyclesNoise_Mix(sum, sum2, rmd);
	}
	return normalize ? .5f * sum / maxamp + .5f : sum;
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_MultiFractal(float4 p, const uint dims,
		const float detail, const float roughness, const float lacunarity) {
	float value = 1.f;
	float pwr = 1.f;

	const int octaves = (int)detail;
	for (int i = 0; i <= octaves; i++) {
		value *= (pwr * CyclesNoise_SNoise(p, dims) + 1.f);
		pwr *= roughness;
		p *= lacunarity;
	}

	const float rmd = detail - floor(detail);
	if (rmd != 0.f)
		value *= (rmd * pwr * CyclesNoise_SNoise(p, dims) + 1.f);

	return value;
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_HeteroTerrain(float4 p, const uint dims,
		const float detail, const float roughness, const float lacunarity,
		const float offset) {
	float pwr = roughness;

	// First unscaled octave of function; later octaves are scaled
	float value = offset + CyclesNoise_SNoise(p, dims);
	p *= lacunarity;

	const int octaves = (int)detail;
	for (int i = 1; i <= octaves; i++) {
		const float increment = (CyclesNoise_SNoise(p, dims) + offset) * pwr * value;
		value += increment;
		pwr *= roughness;
		p *= lacunarity;
	}

	const float rmd = detail - floor(detail);
	if (rmd != 0.f) {
		const float increment = (CyclesNoise_SNoise(p, dims) + offset) * pwr * value;
		value += rmd * increment;
	}

	return value;
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_HybridMultiFractal(float4 p, const uint dims,
		const float detail, const float roughness, const float lacunarity,
		const float offset, const float gain) {
	float pwr = 1.f;
	float value = 0.f;
	float weight = 1.f;

	const int octaves = (int)detail;
	for (int i = 0; (weight > .001f) && (i <= octaves); i++) {
		weight = fmin(weight, 1.f);

		const float signal = (CyclesNoise_SNoise(p, dims) + offset) * pwr;
		pwr *= roughness;
		value += weight * signal;
		weight *= gain * signal;
		p *= lacunarity;
	}

	const float rmd = detail - floor(detail);
	if ((rmd != 0.f) && (weight > .001f)) {
		weight = fmin(weight, 1.f);
		const float signal = (CyclesNoise_SNoise(p, dims) + offset) * pwr;
		value += rmd * weight * signal;
	}

	return value;
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_RidgedMultiFractal(float4 p, const uint dims,
		const float detail, const float roughness, const float lacunarity,
		const float offset, const float gain) {
	float pwr = roughness;

	float signal = offset - fabs(CyclesNoise_SNoise(p, dims));
	signal *= signal;
	float value = signal;
	float weight = 1.f;

	const int octaves = (int)detail;
	for (int i = 1; i <= octaves; i++) {
		p *= lacunarity;
		weight = clamp(signal * gain, 0.f, 1.f);
		signal = offset - fabs(CyclesNoise_SNoise(p, dims));
		signal *= signal;
		signal *= weight;
		value += signal * pwr;
		pwr *= roughness;
	}

	return value;
}

OPENCL_FORCE_NOT_INLINE float CyclesNoise_Select(const float4 p, const uint dims,
		const float detail, const float roughness, const float lacunarity,
		const float offset, const float gain, const uint type,
		const uint normalize) {
	// Order matches slg::CyclesNoiseType
	if (type == 1u)
		return CyclesNoise_MultiFractal(p, dims, detail, roughness, lacunarity);
	else if (type == 2u)
		return CyclesNoise_HybridMultiFractal(p, dims, detail, roughness, lacunarity, offset, gain);
	else if (type == 3u)
		return CyclesNoise_RidgedMultiFractal(p, dims, detail, roughness, lacunarity, offset, gain);
	else if (type == 4u)
		return CyclesNoise_HeteroTerrain(p, dims, detail, roughness, lacunarity, offset);
	else
		return CyclesNoise_FBM(p, dims, detail, roughness, lacunarity, normalize);
}

// Returns the Color output (x = Value) when colorNeeded, else (Value, 0, 0)
OPENCL_FORCE_NOT_INLINE float3 CyclesNoise_Evaluate(const float4 pIn,
		const float scale, const float detailIn, const float roughnessIn,
		const float lacunarity, const float offset, const float gain,
		const float distortion, const uint type, const uint dims,
		const uint normalize, const uint colorNeeded) {
	const float detail = clamp(detailIn, 0.f, 15.f);
	const float roughness = fmax(roughnessIn, 0.f);

	float4 p = pIn * scale;

	if (distortion != 0.f) {
		// Distortion seeds 0..dims-1, computed from the undistorted p
		float4 d = MAKE_FLOAT4(0.f, 0.f, 0.f, 0.f);
		d.x = CyclesNoise_SNoise(CyclesNoise_AddSeedOffset(p, dims, 0.f), dims) * distortion;
		if (dims > 1u)
			d.y = CyclesNoise_SNoise(CyclesNoise_AddSeedOffset(p, dims, 1.f), dims) * distortion;
		if (dims > 2u)
			d.z = CyclesNoise_SNoise(CyclesNoise_AddSeedOffset(p, dims, 2.f), dims) * distortion;
		if (dims > 3u)
			d.w = CyclesNoise_SNoise(CyclesNoise_AddSeedOffset(p, dims, 3.f), dims) * distortion;
		p += d;
	}

	const float value = CyclesNoise_Select(p, dims, detail, roughness,
			lacunarity, offset, gain, type, normalize);
	if (!colorNeeded)
		return MAKE_FLOAT3(value, 0.f, 0.f);

	const float g = CyclesNoise_Select(CyclesNoise_AddSeedOffset(p, dims, (float)dims),
			dims, detail, roughness, lacunarity, offset, gain, type, normalize);
	const float b = CyclesNoise_Select(CyclesNoise_AddSeedOffset(p, dims, (float)(dims + 1u)),
			dims, detail, roughness, lacunarity, offset, gain, type, normalize);
	return MAKE_FLOAT3(value, g, b);
}

OPENCL_FORCE_NOT_INLINE void CyclesNoiseTexture_EvalOp(
		__global const Texture* restrict texture,
		const TextureEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint,
		const float sampleDistance,
		const uint spectralRawDepth
		TEXTURES_PARAM_DECL) {
	switch (evalType) {
		case EVAL_FLOAT:
		case EVAL_SPECTRUM: {
			float w, scale, detail, roughness, lacunarity, offset, gain, distortion;
			EvalStack_PopFloat(distortion);
			EvalStack_PopFloat(gain);
			EvalStack_PopFloat(offset);
			EvalStack_PopFloat(lacunarity);
			EvalStack_PopFloat(roughness);
			EvalStack_PopFloat(detail);
			EvalStack_PopFloat(scale);
			EvalStack_PopFloat(w);
			float3 vec;
			EvalStack_PopFloat3(vec);

			const uint dims = texture->cyclesNoiseTex.dimensions;
			// 1D noise reads only W; 4D appends W to the vector
			const float4 p = (dims == 1u) ? MAKE_FLOAT4(w, 0.f, 0.f, 0.f) :
					MAKE_FLOAT4(vec.x, vec.y, vec.z, (dims == 4u) ? w : 0.f);
			const uint colorOutput = texture->cyclesNoiseTex.colorOutput;
			const float3 eval = CyclesNoise_Evaluate(p, scale, detail,
					roughness, lacunarity, offset, gain, distortion,
					texture->cyclesNoiseTex.noiseType, dims,
					texture->cyclesNoiseTex.normalize, colorOutput);

			if (evalType == EVAL_FLOAT) {
				EvalStack_PushFloat(colorOutput ? Spectrum_Y(eval) : eval.x);
			} else if (!colorOutput) {
				EvalStack_PushFloat3(MAKE_FLOAT3(eval.x, eval.x, eval.x));
			} else if (texture->cyclesNoiseTex.isColor) {
				EvalStack_PushFloat3(SLG_SPECTRAL_LEAF_EVAL_DEPTH(eval, spectralRawDepth));
			} else {
				EvalStack_PushFloat3(eval);
			}
			break;
		}
		case EVAL_BUMP_GENERIC_OFFSET_U:
			Texture_EvalOpGenericBumpOffsetU(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		case EVAL_BUMP_GENERIC_OFFSET_V:
			Texture_EvalOpGenericBumpOffsetV(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		case EVAL_BUMP:
			Texture_EvalOpGenericBump(evalStack, evalStackOffset,
					hitPoint, sampleDistance);
			break;
		default:
			// Something wrong here
			break;
	}
}
