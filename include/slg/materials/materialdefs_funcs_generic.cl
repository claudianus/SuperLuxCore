#line 2 "materialdefs_funcs_generic.cl"

/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Main material functions
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Material_GetEventTypes
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE BSDFEvent Material_GetEventTypes(const uint matIndex
		MATERIALS_PARAM_DECL) {
	__global const Material *material = &mats[matIndex];

	return material->eventTypes;
}

//------------------------------------------------------------------------------
// Material_IsDelta
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE bool Material_IsDelta(const uint matIndex
		MATERIALS_PARAM_DECL) {
	__global const Material *material = &mats[matIndex];

	return material->isDelta;
}

//------------------------------------------------------------------------------
// Material_GetEmittedCosThetaMax
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float Material_GetEmittedCosThetaMax(const uint matIndex
		MATERIALS_PARAM_DECL) {
	__global const Material *material = &mats[matIndex];

	return material->emittedCosThetaMax;
}

//------------------------------------------------------------------------------
// Material_Bump
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void Material_Bump(const uint matIndex, __global HitPoint *hitPoint
	MATERIALS_PARAM_DECL) {
	__global const Material *material = &mats[matIndex];

	uint bumpTexIndex;
	
	// A special case here for TWOSIDED material
	if (material->type == TWOSIDED) {
		bumpTexIndex = hitPoint->intoObject ? mats[material->twosided.frontMatIndex].bumpTexIndex : 
			mats[material->twosided.backMatIndex].bumpTexIndex;
	} else
		bumpTexIndex = material->bumpTexIndex;

	if (bumpTexIndex != NULL_INDEX) {
		const float3 shadeN = Texture_Bump(bumpTexIndex, hitPoint, material->bumpSampleDistance
			TEXTURES_PARAM);

		// Update dpdu and dpdv so they are still orthogonal to shadeN
		float3 dpdu = VLOAD3F(&hitPoint->dpdu.x);
		float3 dpdv = VLOAD3F(&hitPoint->dpdv.x);
		dpdu = cross(shadeN, cross(dpdu, shadeN));
		dpdv = cross(shadeN, cross(dpdv, shadeN));
		// Update HitPoint structure
		VSTORE3F(shadeN, &hitPoint->shadeN.x);
		VSTORE3F(dpdu, &hitPoint->dpdu.x);
		VSTORE3F(dpdv, &hitPoint->dpdv.x);
	}
}

//------------------------------------------------------------------------------
// Material_GetGlossiness
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float Material_GetGlossiness(const uint matIndex
		MATERIALS_PARAM_DECL) {
	return mats[matIndex].glossiness;
}

//------------------------------------------------------------------------------
// Material_IsPhotonGIEnabled
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float Material_IsPhotonGIEnabled(const uint matIndex
		MATERIALS_PARAM_DECL) {
	return mats[matIndex].isPhotonGIEnabled;
}

//------------------------------------------------------------------------------
// Material_IsHoldout
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float Material_IsHoldout(const uint matIndex
		MATERIALS_PARAM_DECL) {
	return mats[matIndex].isHoldout;
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Generic material related functions
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float SchlickDistribution_SchlickZ(const float roughness, float cosNH) {
	if (roughness > 0.f) {
		const float cosNH2 = cosNH * cosNH;
		// expanded for increased numerical stability
		const float d = cosNH2 * roughness + (1.f - cosNH2);
		// use double division to avoid overflow in d*d product
		return (roughness / d) / d;
	}
	return 0.f;
}

OPENCL_FORCE_INLINE float SchlickDistribution_SchlickA(const float3 H, const float anisotropy) {
	const float h = sqrt(H.x * H.x + H.y * H.y);
	if (h > 0.f) {
		const float w = (anisotropy > 0.f ? H.x : H.y) / h;
		const float p = 1.f - fabs(anisotropy);
		return sqrt(p / (p * p + w * w * (1.f - p * p)));
	}

	return 1.f;
}

OPENCL_FORCE_INLINE float SchlickDistribution_D(const float roughness, const float3 wh, const float anisotropy) {
	const float cosTheta = fabs(wh.z);
	return SchlickDistribution_SchlickZ(roughness, cosTheta) * SchlickDistribution_SchlickA(wh, anisotropy) * M_1_PI_F;
}

OPENCL_FORCE_INLINE float SchlickDistribution_SchlickG(const float roughness, const float costheta) {
	return costheta / (costheta * (1.f - roughness) + roughness);
}

OPENCL_FORCE_INLINE float SchlickDistribution_G(const float roughness, const float3 fixedDir, const float3 sampledDir) {
	return SchlickDistribution_SchlickG(roughness, fabs(fixedDir.z)) *
			SchlickDistribution_SchlickG(roughness, fabs(sampledDir.z));
}

OPENCL_FORCE_INLINE float GetPhi(const float a, const float b) {
	return M_PI_F * .5f * sqrt(a * b / (1.f - a * (1.f - b)));
}

OPENCL_FORCE_INLINE void SchlickDistribution_SampleH(const float roughness, const float anisotropy,
		const float u0, const float u1, float3 *wh, float *d, float *pdf) {
	float u1x4 = u1 * 4.f;
	const float cos2Theta = u0 / (roughness * (1.f - u0) + u0);
	const float cosTheta = sqrt(cos2Theta);
	const float sinTheta = sqrt(1.f - cos2Theta);
	const float p = 1.f - fabs(anisotropy);
	float phi;
	if (u1x4 < 1.f) {
		phi = GetPhi(u1x4 * u1x4, p * p);
	} else if (u1x4 < 2.f) {
		u1x4 = 2.f - u1x4;
		phi = M_PI_F - GetPhi(u1x4 * u1x4, p * p);
	} else if (u1x4 < 3.f) {
		u1x4 -= 2.f;
		phi = M_PI_F + GetPhi(u1x4 * u1x4, p * p);
	} else {
		u1x4 = 4.f - u1x4;
		phi = M_PI_F * 2.f - GetPhi(u1x4 * u1x4, p * p);
	}

	if (anisotropy > 0.f)
		phi += M_PI_F * .5f;

	*wh = MAKE_FLOAT3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
	*d = SchlickDistribution_SchlickZ(roughness, cosTheta) * SchlickDistribution_SchlickA(*wh, anisotropy) * M_1_PI_F;
	*pdf = *d;
}

OPENCL_FORCE_INLINE float SchlickDistribution_Pdf(const float roughness, const float3 wh,
		const float anisotropy) {
	return SchlickDistribution_D(roughness, wh, anisotropy);
}

OPENCL_FORCE_INLINE float3 FresnelSchlick_Evaluate(const float3 normalIncidence, const float cosi) {
	return normalIncidence + (WHITE - normalIncidence) *
		pow(1.f - cosi, 5.f);
}

OPENCL_FORCE_INLINE float3 CoatingAbsorption(const float cosi, const float coso,
		const float3 alpha, const float depth) {
	if (depth > 0.f) {
		// 1/cosi+1/coso=(cosi+coso)/(cosi*coso)
		const float depthFactor = depth * (cosi + coso) / (cosi * coso);
		return Spectrum_Exp(alpha * -depthFactor);
	} else
		return WHITE;
}

OPENCL_FORCE_INLINE float SchlickBSDF_CoatingWeight(const float3 ks, const float3 fixedDir) {
	// Approximate H by using reflection direction for wi
	const float u = fabs(fixedDir.z);
	const float3 S = FresnelSchlick_Evaluate(ks, u);

	// Ensures coating is never sampled less than half the time
	return .5f * (1.f + Spectrum_Filter(S));
}

OPENCL_FORCE_INLINE float3 SchlickBSDF_CoatingF(const float3 ks, const float roughness,
		const float anisotropy, const int multibounce, const float3 fixedDir,
		const float3 sampledDir) {
	const float coso = fabs(fixedDir.z);
	const float cosi = fabs(sampledDir.z);

	const float3 wh = normalize(fixedDir + sampledDir);
	const float cosi_Schlick = clamp(fabs(dot(sampledDir, wh)), 0.f, 1.f);
	const float3 S = FresnelSchlick_Evaluate(ks, cosi_Schlick);

	const float G = SchlickDistribution_G(roughness, fixedDir, sampledDir);

	// Multibounce - alternative with interreflection in the coating creases
	float factor = SchlickDistribution_D(roughness, wh, anisotropy) * G;
	//if (!fromLight)
		factor = factor / (4.f * coso) +
				(multibounce ? cosi * clamp((1.f - G) / (4.f * coso * cosi), 0.f, 1.f) : 0.f);
	//else
	//	factor = factor / (4.f * cosi) + 
	//			(multibounce ? coso * Clamp((1.f - G) / (4.f * cosi * coso), 0.f, 1.f) : 0.f);

	return factor * S;
}

OPENCL_FORCE_INLINE float3 SchlickBSDF_CoatingSampleF(const float3 ks,
		const float roughness, const float anisotropy, const int multibounce,
		const float3 fixedDir, float3 *sampledDir,
		float u0, float u1, float *pdf) {
	float3 wh;
	float d, specPdf;
	SchlickDistribution_SampleH(roughness, anisotropy, u0, u1, &wh, &d, &specPdf);
	const float cosWH = dot(fixedDir, wh);
	*sampledDir = 2.f * cosWH * wh - fixedDir;

	if ((fabs((*sampledDir).z) < DEFAULT_COS_EPSILON_STATIC) || (fixedDir.z * (*sampledDir).z < 0.f))
		return BLACK;

	const float coso = fabs(fixedDir.z);
	const float cosi = fabs((*sampledDir).z);

	*pdf = specPdf / (4.f * cosWH);
	if (*pdf <= 0.f)
		return BLACK;

	float3 S = FresnelSchlick_Evaluate(ks, fabs(cosWH));

	const float G = SchlickDistribution_G(roughness, fixedDir, *sampledDir);

	//CoatingF(sw, *wi, wo, f_);
	S *= (d / *pdf) * G / (4.f * coso) + 
			(multibounce ? cosi * clamp((1.f - G) / (4.f * coso * cosi), 0.f, 1.f) / *pdf : 0.f);

	return S;
}

OPENCL_FORCE_INLINE float SchlickBSDF_CoatingPdf(const float roughness, const float anisotropy,
		const float3 fixedDir, const float3 sampledDir) {
	const float3 wh = normalize(fixedDir + sampledDir);
	return SchlickDistribution_Pdf(roughness, wh, anisotropy) / (4.f * fabs(dot(fixedDir, wh)));
}

OPENCL_FORCE_INLINE float FrDiel2(const float cosi, const float cost, const float eta) {
	float Rparl = eta * cosi;
	Rparl = (cost - Rparl) / (cost + Rparl);
	float Rperp = eta * cost;
	Rperp = (cosi - Rperp) / (cosi + Rperp);

	return (Rparl * Rparl + Rperp * Rperp) * .5f;
}

OPENCL_FORCE_INLINE float3 FrFull(const float cosi, const float3 cost, const float3 eta, const float3 k) {
	const float3 tmp = (eta * eta + k * k) * (cosi * cosi) + (cost * cost);
	const float3 Rparl2 = (tmp - (2.f * cosi * cost) * eta) /
		(tmp + (2.f * cosi * cost) * eta);
	const float3 tmp_f = (eta * eta + k * k) * (cost * cost) + (cosi * cosi);
	const float3 Rperp2 = (tmp_f - (2.f * cosi * cost) * eta) /
		(tmp_f + (2.f * cosi * cost) * eta);
	return (Rparl2 + Rperp2) * .5f;
}

OPENCL_FORCE_INLINE float3 FresnelGeneral_Evaluate(const float3 eta, const float3 k, const float cosi) {
	float3 sint2 = TO_FLOAT3(fmax(0.f, 1.f - cosi * cosi));
	if (cosi > 0.f)
		sint2 /= eta * eta;
	else
		sint2 *= eta * eta;
	sint2 = Spectrum_Clamp(sint2);

	const float3 cost2 = 1.f - sint2;
	if (cosi > 0.f) {
		const float3 a = 2.f * k * k * sint2;
		return FrFull(cosi, Spectrum_Sqrt((cost2 + Spectrum_Sqrt(cost2 * cost2 + a * a)) / 2.f), eta, k);
	} else {
		const float3 a = 2.f * k * k * sint2;
		const float3 d2 = eta * eta + k * k;
		return FrFull(-cosi, Spectrum_Sqrt((cost2 + Spectrum_Sqrt(cost2 * cost2 + a * a)) / 2.f), eta / d2, -k / d2);
	}
}

OPENCL_FORCE_INLINE float FresnelCauchy_Evaluate(const float eta, const float cosi) {
	// Compute indices of refraction for dielectric
	const bool entering = (cosi > 0.f);

	// Compute _sint_ using Snell's law
	const float eta2 = eta * eta;
	const float sint2 = (entering ? 1.f / eta2 : eta2) *
		fmax(0.f, 1.f - cosi * cosi);
	// Handle total internal reflection
	if (sint2 >= 1.f)
		return 1.f;
	else
		return FrDiel2(fabs(cosi), sqrt(fmax(0.f, 1.f - sint2)),
			entering ? eta : 1.f / eta);
}

// SSS shared leaf helpers (CPU mirrors in homogenous.cpp): d'Eon
// van-de-Hulst closed-form albedo inversion and the Jensen-Buhler/CB15
// diffusion-theory reflectance used as the boundary ratio
// K = Rd(a,eta)/Rd(a,1).
OPENCL_FORCE_INLINE float SSSAlphaVanDeHulst(const float A, const float g) {
	const float x = 4.20863f * A + 4.09712f -
			sqrt(9.59217f + 41.6808f * A + 17.7126f * A * A);
	const float s2 = x * x;
	return clamp((1.f - s2) / (1.f - g * s2), 0.f, 0.999999f);
}

OPENCL_FORCE_INLINE float SSSRdDiffusion(const float a, const float eta) {
	const float e = fmax(eta, 1.0001f);
	const float rhoE = -1.4399f / (e * e) + .7099f / e + .6681f + .0636f * e;
	const float Ab = (1.f + rhoE) / fmax(1.f - rhoE, 1e-4f);
	return .5f * a * (1.f + exp(-4.f / 3.f * Ab * sqrt(3.f * (1.f - a))));
}

// Sellmeier 3-term: n^2(lambda_um) = 1 + sum_i B_i l^2/(l^2 - C_i).
// sellB.x < 0 marks "no Sellmeier" (falls back to Cauchy via cauchyB).
// Wavelength in nm; valid outside spectral transport too.
OPENCL_FORCE_INLINE float Spectral_WaveLength2IORSellmeier(
		const float waveLength, const float ior, const float cauchyB,
		const float3 sellB, const float3 sellC) {
	if (sellB.x < 0.f)
		return ior + cauchyB / ((waveLength * 0.001f) * (waveLength * 0.001f));
	const float l2 = waveLength * 0.001f * waveLength * 0.001f;
	// l2-C denominators keep their sign (IR resonance terms are negative
	// across the visible range) - clamp magnitude only, never to +eps.
	const float dx = l2 - sellC.x;
	const float dy = l2 - sellC.y;
	const float dz = l2 - sellC.z;
	float n2 = 1.f;
	n2 += sellB.x * l2 / (fabs(dx) > 1e-9f ? dx : ((dx >= 0.f) ? 1e-9f : -1e-9f));
	n2 += sellB.y * l2 / (fabs(dy) > 1e-9f ? dy : ((dy >= 0.f) ? 1e-9f : -1e-9f));
	n2 += sellB.z * l2 / (fabs(dz) > 1e-9f ? dz : ((dz >= 0.f) ? 1e-9f : -1e-9f));
	return sqrt(fmax(n2, 1.f));
}

// Effective scalar IOR at the 560nm reference wavelength for non-spectral
// paths (Sellmeier defines n absolutely; Cauchy adds B/l^2 to the base IOR).
OPENCL_FORCE_INLINE float Spectral_RefIOR(const float nt, const float cauchyB,
		const float3 sellB, const float3 sellC) {
	if (sellB.x < 0.f && cauchyB <= 0.f)
		return nt;
	return Spectral_WaveLength2IORSellmeier(560.f, nt, cauchyB, sellB, sellC);
}

#if defined(SLG_SPECTRAL)
// Device mirrors of slg::WaveLength2IOR / DispersiveIOR /
// DispersiveFresnelR (material.cpp): Cauchy IOR at the hero wavelength for
// direction-defining events, per-bin dielectric Fresnel for reflectance.

OPENCL_FORCE_INLINE float Spectral_WaveLength2IOR(const float waveLength,
		const float ior, const float B) {
	// Cauchy's equation (waveLength in nm, Cauchy lambda in micrometers)
	return ior + B / ((waveLength * 0.001f) * (waveLength * 0.001f));
}

// Single-form dispersive IOR (OpenCL C has no overloading): sellB.x
// < 0 selects the Cauchy path via cauchyB - identical to the old
// 3-arg variant since Spectral_WaveLength2IORSellmeier falls back to
// Cauchy internally. Otherwise 3-term Sellmeier n(lambda).
OPENCL_FORCE_INLINE float Spectral_DispersiveIOR(const float nt,
		const float cauchyB, const float3 sellB, const float3 sellC,
		__global const HitPoint *hitPoint) {
	if (sellB.x < 0.f && cauchyB <= 0.f)
		return nt;
	const uint hero = min((hitPoint->spectralHeroAlive & SLG_SW_HERO_MASK) >> SLG_SW_HERO_SHIFT,
			SLG_SPECTRAL_BINS - 1u);
	return Spectral_WaveLength2IORSellmeier(hitPoint->spectralW[hero], nt,
			cauchyB, sellB, sellC);
}

// Single-form dispersive Fresnel: sellB.x < 0 -> per-bin Cauchy,
// otherwise per-bin Sellmeier. sellB.x < 0 && cauchyB <= 0 means no
// dispersion at all - uniform base-IOR Fresnel.
OPENCL_FORCE_INLINE float3 Spectral_DispersiveFresnelR(const float nt,
		const float nc, const float cauchyB, const float3 sellB,
		const float3 sellC, const float cosTheta,
		__global const HitPoint *hitPoint) {
	if (sellB.x < 0.f && cauchyB <= 0.f)
		return MAKE_FLOAT3(FresnelCauchy_Evaluate(nt / nc, cosTheta),
				FresnelCauchy_Evaluate(nt / nc, cosTheta),
				FresnelCauchy_Evaluate(nt / nc, cosTheta));

	// Per-bin dielectric Fresnel: each sampled wavelength sees its own IOR
	const uint aliveMask = hitPoint->spectralHeroAlive & SLG_SW_ALIVE_MASK;
	float3 F = BLACK;
	for (uint i = 0; i < SLG_SPECTRAL_BINS; ++i) {
		if (aliveMask & (1u << i)) {
			const float ior = (sellB.x < 0.f) ?
					Spectral_WaveLength2IOR(hitPoint->spectralW[i],
							nt, cauchyB) :
					Spectral_WaveLength2IORSellmeier(hitPoint->spectralW[i],
							nt, cauchyB, sellB, sellC);
			const float r = FresnelCauchy_Evaluate(ior / nc, cosTheta);
			if (i == 0) F.x = r; else if (i == 1) F.y = r; else F.z = r;
		}
	}
	return F;
}
#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
