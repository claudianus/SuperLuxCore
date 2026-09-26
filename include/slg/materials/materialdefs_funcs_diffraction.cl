#line 2 "materialdefs_funcs_diffraction.cl"

/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 *   Licensed under the Apache License, Version 2.0 (the "License");       *
 *   you may not use this file except in compliance with the License.      *
 *   You may obtain a copy of the License at                               *
 *                                                                         *
 *   Unless required by applicable law or agreed to in writing, software   *
 *   distributed under the License is distributed on an "AS IS" BASIS,     *
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or       *
 *   implied.                                                              *
 *   See the License for the specific language governing permissions and   *
 *   limitations under the License.                                        *
 ***************************************************************************/

//------------------------------------------------------------------------------
// DiffractionMaterial (1D reflective grating) -- GPU twin of
// src/slg/materials/diffraction.cpp. Keep the math bit-identical.
//
// Per order m the diffracted direction is the delta lobe
//   a_m = -a_f + m * lambda / d,  b_m = -b_f,  c_m = +sqrt(1 - a_m^2 - b_m^2)
// in the (s, t, n) frame, where s is the grating direction and t runs along
// the grooves. Orders are importance sampled by the lamellar groove
// envelope so the returned weight is the constant kr.
//------------------------------------------------------------------------------

// Same piecewise-linear approximation as GlassMaterial_WaveLength2RGB
// (kept private to this file so include order does not matter)
OPENCL_FORCE_INLINE float3 DiffractionMaterial_WaveLength2RGB(const float waveLength) {
	float r, g, b;
	if ((waveLength >= 380.f) && (waveLength < 440.f)) {
		r = -(waveLength - 440.f) / (440 - 380.f);
		g = 0.f;
		b = 1.f;
	} else if ((waveLength >= 440.f) && (waveLength < 490.f)) {
		r = 0.f;
		g = (waveLength - 440.f) / (490.f - 440.f);
		b = 1.f;
	} else if ((waveLength >= 490.f) && (waveLength < 510.f)) {
		r = 0.f;
		g = 1.f;
		b = -(waveLength - 510.f) / (510.f - 490.f);
	} else if ((waveLength >= 510.f) && (waveLength < 580.f)) {
		r = (waveLength - 510.f) / (580.f - 510.f);
		g = 1.f;
		b = 0.f;
	} else if ((waveLength >= 580.f) && (waveLength < 645.f)) {
		r = 1.f;
		g = -(waveLength - 645.f) / (645 - 580.f);
		b = 0.f;
	} else if ((waveLength >= 645.f) && (waveLength < 780.f)) {
		r = 1.f;
		g = 0.f;
		b = 0.f;
	} else
		return BLACK;

	float factor;
	if ((waveLength >= 380.f) && (waveLength < 420.f))
		factor = .3f + .7f * (waveLength - 380.f) / (420.f - 380.f);
	else if ((waveLength >= 420) && (waveLength < 700))
		factor = 1.f;
	else
		factor = .3f + .7f * (780.f - waveLength) / (780.f - 700.f);

	return MAKE_FLOAT3(
			r * factor / .5652729f,
			g * factor / .36875f,
			b * factor / .265375f);
}

// Bit-exact Wang hash -> [0,1) (CPU twin: DfrHash01 in diffraction.cpp)
OPENCL_FORCE_INLINE float DiffractionMaterial_Hash01(const float a, const float b, const float c) {
	uint h = as_uint(a) ^ (as_uint(b) << 1u) ^ as_uint(c);
	h = (h ^ 61u) ^ (h >> 16u);
	h += h << 3u;
	h ^= h >> 4u;
	h *= 0x27d4eb2du;
	h ^= h >> 15u;
	return (float)(h >> 8u) * (1.f / 16777216.f);
}

OPENCL_FORCE_INLINE float DiffractionMaterial_Sinc2(const float x) {
	const float s = (fabs(x) < 1e-6f) ? 1.f : sin(x) / x;
	return s * s;
}

//------------------------------------------------------------------------------
// Diffraction material
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void DiffractionMaterial_Albedo(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	const float3 albedo = Spectrum_Clamp(Texture_GetSpectrumValue(
			material->diffraction.krTexIndex, hitPoint TEXTURES_PARAM));

	EvalStack_PushFloat3(albedo);
}

OPENCL_FORCE_INLINE void DiffractionMaterial_GetInteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void DiffractionMaterial_GetExteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void DiffractionMaterial_GetPassThroughTransparency(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void DiffractionMaterial_GetEmittedRadiance(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void DiffractionMaterial_Evaluate(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 lightDir, eyeDir;
	EvalStack_PopFloat3(eyeDir);
	EvalStack_PopFloat3(lightDir);

	// Delta material: only SPECULAR|REFLECT discrete lobes exist
	MATERIAL_EVALUATE_RETURN_BLACK;
}

OPENCL_FORCE_INLINE void DiffractionMaterial_Sample(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float u0, u1, passThroughEvent;
	EvalStack_PopFloat(passThroughEvent);
	EvalStack_PopFloat(u1);
	EvalStack_PopFloat(u0);
	float3 fixedDir;
	EvalStack_PopFloat3(fixedDir);

	const float3 kr = Spectrum_Clamp(Texture_GetSpectrumValue(
			material->diffraction.krTexIndex, hitPoint TEXTURES_PARAM));
	if (Spectrum_IsBlack(kr)) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	const float d = fmax(1e-3f, Texture_GetFloatValue(
			material->diffraction.spacingTexIndex, hitPoint TEXTURES_PARAM)); // nm
	const float fill = clamp(Texture_GetFloatValue(
			material->diffraction.fillTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);

	//------------------------------------------------------------------
	// Groove frame: s (grating dir), t (groove dir) in local coords
	//------------------------------------------------------------------
	float sx, sy;
	const uint orient = material->diffraction.orientation;
	if (orient == 0u) {
		// DIFFRACTION_U: grooves along U
		sx = 0.f; sy = 1.f;
	} else if (orient == 1u) {
		// DIFFRACTION_V: grooves along V
		sx = 1.f; sy = 0.f;
	} else {
		// Radial modes: world-space radial dir projected on the tangent plane
		float3 radial;
		if (orient == 2u) {
			// DIFFRACTION_RADIAL_UV
			const float du = hitPoint->defaultUV.u - material->diffraction.centerU;
			const float dv = hitPoint->defaultUV.v - material->diffraction.centerV;
			radial = du * VLOAD3F(&hitPoint->dpdu.x) + dv * VLOAD3F(&hitPoint->dpdv.x);
		} else {
			// DIFFRACTION_RADIAL: object-space center -> world
			const float3 cLocal = MAKE_FLOAT3(material->diffraction.centerX,
					material->diffraction.centerY, material->diffraction.centerZ);
			const float3 cWorld = Transform_ApplyPoint(&hitPoint->localToWorld, cLocal);
			radial = VLOAD3F(&hitPoint->p.x) - cWorld;
		}
		const float3 n = VLOAD3F(&hitPoint->shadeN.x);
		radial -= dot(radial, n) * n;

		if (dot(radial, radial) < 1e-12f) {
			sx = 1.f; sy = 0.f;
		} else {
			const float3 r = normalize(radial);
			Frame frame;
			Frame_Set_Private(&frame, VLOAD3F(&hitPoint->dpdu.x),
					VLOAD3F(&hitPoint->dpdv.x), n);
			const float3 rl = Frame_ToLocal_Private(&frame, r);
			const float len = sqrt(rl.x * rl.x + rl.y * rl.y);
			if (len < 1e-6f) {
				sx = 1.f; sy = 0.f;
			} else {
				sx = rl.x / len; sy = rl.y / len;
			}
		}
	}
	// t = n x s
	const float tx = -sy, ty = sx;

	const float aF = fixedDir.x * sx + fixedDir.y * sy;
	const float bF = -fixedDir.x * tx - fixedDir.y * ty;
	const float cF = fixedDir.z;

	//------------------------------------------------------------------
	// Wavelength: hero bin in spectral mode, jittered RGB otherwise
	//------------------------------------------------------------------
	float lambda, heroWeight = 1.f;
	float3 waveColor = WHITE;
#if defined(SLG_SPECTRAL)
	const uint hero = min((hitPoint->spectralHeroAlive & SLG_SW_HERO_MASK) >> SLG_SW_HERO_SHIFT,
			SLG_SPECTRAL_BINS - 1u);
	lambda = hitPoint->spectralW[hero];
	heroWeight = Spectral_CollapseToHero(&((__global HitPoint *)hitPoint)->spectralHeroAlive);
#else
	lambda = mix(380.f, 780.f, passThroughEvent);
	waveColor = DiffractionMaterial_WaveLength2RGB(lambda);
#endif

	const float lOverD = lambda / d;

	//------------------------------------------------------------------
	// Valid order range and envelope weights
	//------------------------------------------------------------------
	const float sb = sqrt(fmax(0.f, 1.f - bF * bF));
	const int maxO = min((int)material->diffraction.maxOrder, 32);
	const int mMin = max(-maxO, (int)ceil((aF - sb) / lOverD - 1e-4f));
	const int mMax = min(maxO, (int)floor((aF + sb) / lOverD + 1e-4f));

	float3 result, sampledDir;
	float pdfW;
	BSDFEvent event;
	if (mMin > mMax) {
		// No propagating diffracted order: plain mirror fallback
		sampledDir = MAKE_FLOAT3(-fixedDir.x, -fixedDir.y, fixedDir.z);
		result = kr * waveColor * heroWeight;
		pdfW = 1.f;
		event = SPECULAR | REFLECT;
	} else {
		const float aSpec = -aF * cos(2.f * material->diffraction.blaze) +
				cF * sin(2.f * material->diffraction.blaze);
		const float envScale = M_PI_F * fill / lOverD;
		float w[65];
		float wSum = 0.f;
		for (int m = mMin; m <= mMax; ++m) {
			const float am = -aF + m * lOverD;
			w[m - mMin] = DiffractionMaterial_Sinc2(envScale * (am - aSpec));
			wSum += w[m - mMin];
		}

		int mPick = 0;
		if (wSum > 1e-8f) {
			const float target = u0 * wSum;
			float acc = 0.f;
			for (int m = mMin; m <= mMax; ++m) {
				acc += w[m - mMin];
				if (target <= acc) {
					mPick = m;
					break;
				}
			}
			if (mPick == 0 && mMin == mMax)
				mPick = mMin;
		} else {
			// Degenerate envelope: pick the order closest to facet specular
			float best = MAXFLOAT;
			for (int m = mMin; m <= mMax; ++m) {
				const float am = -aF + m * lOverD;
				const float e = fabs(am - aSpec);
				if (e < best) {
					best = e;
					mPick = m;
				}
			}
		}

		float aM = -aF + mPick * lOverD;
		float bM = -bF;

		// Groove roughness jitter (bit-exact hash keeps CPU/GPU in sync)
		const float rough = clamp(Texture_GetFloatValue(
				material->diffraction.roughTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
		if (rough > 0.f) {
			const float sigma = rough * .25f;
			const float uj2 = DiffractionMaterial_Hash01(u0, u1, passThroughEvent);
			const float r = sqrt(-2.f * log(fmax(1e-30f, 1.f - u1)));
			const float ga = r * cos(2.f * M_PI_F * uj2) * sigma;
			const float gb = r * sin(2.f * M_PI_F * uj2) * sigma;
			const float aj = aM + ga, bj = bM + gb;
			if (aj * aj + bj * bj < 1.f) {
				aM = aj; bM = bj;
			}
		}

		const float cM = sqrt(fmax(0.f, 1.f - aM * aM - bM * bM));

		sampledDir = MAKE_FLOAT3(
				aM * sx + bM * tx,
				aM * sy + bM * ty,
				cM);
		result = kr * waveColor * heroWeight;
		pdfW = 1.f;
		event = SPECULAR | REFLECT;
	}

	EvalStack_PushFloat3(result);
	EvalStack_PushFloat3(sampledDir);
	EvalStack_PushFloat(pdfW);
	EvalStack_PushBSDFEvent(event);
}

//------------------------------------------------------------------------------
// Material specific EvalOp
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE void DiffractionMaterial_EvalOp(
		__global const Material* restrict material,
		const MaterialEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint
		MATERIALS_PARAM_DECL) {
	switch (evalType) {
		case EVAL_ALBEDO:
			DiffractionMaterial_Albedo(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_INTERIOR_VOLUME:
			DiffractionMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EXTERIOR_VOLUME:
			DiffractionMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EMITTED_RADIANCE:
			DiffractionMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_PASS_TROUGH_TRANSPARENCY:
			DiffractionMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_EVALUATE:
			DiffractionMaterial_Evaluate(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_SAMPLE:
			DiffractionMaterial_Sample(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
