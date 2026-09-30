#line 2 "materialdefs_funcs_glass.cl"

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
// Glass material
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void GlassMaterial_Albedo(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
    const float3 albedo = WHITE;

	EvalStack_PushFloat3(albedo);
}

OPENCL_FORCE_INLINE void GlassMaterial_GetInteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void GlassMaterial_GetExteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void GlassMaterial_GetPassThroughTransparency(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void GlassMaterial_GetEmittedRadiance(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE float3 GlassMaterial_WaveLength2RGB(const float waveLength) {
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

	// The intensity fall off near the upper and lower limits
	float factor;
	if ((waveLength >= 380.f) && (waveLength < 420.f))
		factor = .3f + .7f * (waveLength - 380.f) / (420.f - 380.f);
	else if ((waveLength >= 420) && (waveLength < 700))
		factor = 1.f;
	else
		factor = .3f + .7f * (780.f - waveLength) / (780.f - 700.f);

	const float3 result = MAKE_FLOAT3(r, g, b) * factor;

	// To normalize the output
	const float3 normFactor = MAKE_FLOAT3(1.f / .5652729f, 1.f / .36875f, 1.f / .265375f);
	
	return result * normFactor;
}

OPENCL_FORCE_INLINE float GlassMaterial_WaveLength2IOR(const float waveLength, const float IOR, const float B) {
	// Cauchy's equation for relationship between the refractive index and wavelength
	// note: Cauchy's lambda is expressed in micrometers while waveLength is in nanometers

	// This is the formula suggested by Neo here, with a changed naming convention from B->A and C-> B:
	// https://github.com/LuxCoreRender/BlendLuxCore/commit/d3fed046ab62e18226e410b42a16ca1bccefb530#commitcomment-26617643
	
	// Compute Cauchy-A assuming the user input IOR at 587.56 nm 
	// (Fraunhofer d-line, Helium, used in one definition of the Abbe number)
	//const float A = IOR - B / Sqr(587.56f / 1000.f);

	// Use the user input IOR directly as Cauchy-A. Equivalent to the B used by old LuxRender.
	const float A = IOR;

	// Cauchy's equation
	const float cauchyEq = A + B / Sqr(waveLength / 1000.f);

	return cauchyEq;
}

OPENCL_FORCE_INLINE float3 GlassMaterial_EvalSpecularReflection(__global const HitPoint *hitPoint,
		const float3 localFixedDir, const float3 kr,
		const float nc, const float nt, const float cauchyB,
		const float3 sellB, const float3 sellC,
		float3 *sampledDir, const float localFilmThickness, const float localFilmIor) {
	if (Spectrum_IsBlack(kr))
		return BLACK;

	const float costheta = CosTheta(localFixedDir);
	*sampledDir = MAKE_FLOAT3(-localFixedDir.x, -localFixedDir.y, localFixedDir.z);

#if defined(SLG_SPECTRAL)
	// Per-bin dielectric Fresnel under spectral transport (each wavelength
	// sees its own IOR); scalar Fresnel otherwise.
	const float3 result = kr * Spectral_DispersiveFresnelR(nt, nc, cauchyB,
			sellB, sellC, costheta, hitPoint);
#else
	// Effective IOR at 560nm so Sellmeier still reflects (nt alone is not
	// the physical index when coefficient-driven)
	const float ntc = Spectral_RefIOR(nt, cauchyB, sellB, sellC) / nc;
	const float3 result = kr * FresnelCauchy_Evaluate(ntc, costheta);
#endif
	
	if (localFilmThickness > 0.f) {
		const float3 filmColor = CalcFilmColor(localFixedDir, localFilmThickness, localFilmIor);
		return result * filmColor;
	}
	return result;
}

OPENCL_FORCE_INLINE float3 GlassMaterial_EvalSpecularTransmission(__global const HitPoint *hitPoint,
		const float3 localFixedDir, const float u0,
		const float3 kt, const float nc, const float nt, const float cauchyB,
		const float3 sellB, const float3 sellC,
		float3 *sampledDir) {
	if (Spectrum_IsBlack(kt))
		return BLACK;

	const bool dispersive = (sellB.x >= 0.f) || (cauchyB > 0.f);

	// Compute transmitted ray direction
	float3 lkt;
	float lnt;
#if defined(SLG_SPECTRAL)
	if (dispersive) {
		// Hero-wavelength refraction: the direction-defining wavelength is
		// the path's hero bin (no per-path RGB tint -- the bins already are
		// the spectral samples).
		const uint hero = min((hitPoint->spectralHeroAlive & SLG_SW_HERO_MASK) >> SLG_SW_HERO_SHIFT,
				SLG_SPECTRAL_BINS - 1u);
		lnt = Spectral_WaveLength2IORSellmeier(hitPoint->spectralW[hero],
				nt, cauchyB, sellB, sellC);
		lkt = kt;
	} else
#endif
	if (dispersive) {
		// Select the wavelength to sample
		const float waveLength = mix(380.f, 780.f, u0);

		lnt = Spectral_WaveLength2IORSellmeier(waveLength, nt, cauchyB,
				sellB, sellC);

		lkt = kt * GlassMaterial_WaveLength2RGB(waveLength);
	} else {
		lnt = nt;
		lkt = kt;
	}

	const float ntc = lnt / nc;
	const float costheta = CosTheta(localFixedDir);
	const bool entering = (costheta > 0.f);
	const float eta = entering ? (nc / lnt) : ntc;
	const float eta2 = eta * eta;
	const float sini2 = SinTheta2(localFixedDir);
	const float sint2 = eta2 * sini2;

	// Handle total internal reflection for transmission
	if (sint2 >= 1.f)
		return BLACK;

	const float cost = sqrt(fmax(0.f, 1.f - sint2)) * (entering ? -1.f : 1.f);
	*sampledDir = MAKE_FLOAT3(-eta * localFixedDir.x, -eta * localFixedDir.y, cost);

	float ce;
//	if (!hitPoint.fromLight)
		ce = (1.f - FresnelCauchy_Evaluate(ntc, cost)) * eta2;
//	else {
//		const float absCosSampledDir = fabsf(CosTheta(*sampledDir));
//		ce = (1.f - FresnelTexture::CauchyEvaluate(ntc, costheta)) * fabsf(localFixedDir.z / absCosSampledDir);
//	}

	return lkt * ce;
}

OPENCL_FORCE_INLINE void GlassMaterial_GGXDielectricEval(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		float3 lightDir, float3 eyeDir,
		float3 *result, BSDFEvent *event, float *directPdfW
		MATERIALS_PARAM_DECL) {
	const float3 ktVal = Texture_GetSpectrumValue(material->glass.ktTexIndex, hitPoint TEXTURES_PARAM);
	const float3 krVal = Texture_GetSpectrumValue(material->glass.krTexIndex, hitPoint TEXTURES_PARAM);
	const float3 kt = Spectrum_Clamp(ktVal);
	const float3 kr = Spectrum_Clamp(krVal);

	const bool isKtBlack = Spectrum_IsBlack(kt);
	const bool isKrBlack = Spectrum_IsBlack(kr);
	if (isKtBlack && isKrBlack) {
		*result = BLACK;
		*event = NONE;
		*directPdfW = 0.f;
		return;
	}

	const float nc = ExtractExteriorIors(hitPoint, material->glass.exteriorIorTexIndex TEXTURES_PARAM);
	const float nt = ExtractInteriorIors(hitPoint, material->glass.interiorIorTexIndex TEXTURES_PARAM);
	const float cauchyB = (material->glass.cauchyBTex != NULL_INDEX) ? Texture_GetFloatValue(material->glass.cauchyBTex, hitPoint TEXTURES_PARAM) : -1.f;
	const float3 sellB = (material->glass.sellmeierBTex != NULL_INDEX &&
			material->glass.sellmeierCTex != NULL_INDEX) ?
			Texture_GetSpectrumValue(material->glass.sellmeierBTex, hitPoint TEXTURES_PARAM) :
			MAKE_FLOAT3(-1.f, 0.f, 0.f);
	const float3 sellC = (sellB.x >= 0.f) ?
			Texture_GetSpectrumValue(material->glass.sellmeierCTex, hitPoint TEXTURES_PARAM) :
			BLACK;
#if defined(SLG_SPECTRAL)
	// Dispersion: the direction-defining wavelength is the hero bin
	const float ntEff = Spectral_DispersiveIOR(nt, cauchyB, sellB, sellC, hitPoint);
#else
	const float ntEff = Spectral_RefIOR(nt, cauchyB, sellB, sellC);
#endif
	const float ntc = ntEff / nc;

	// CPU GlassMicrofacet_Evaluate: isotropic alpha = regularization.
	const float alpha = hitPoint->regularization;
	const float threshold = isKrBlack ? 1.f : (isKtBlack ? 0.f : .5f);

	if (lightDir.z * eyeDir.z < 0.f) {
		// Transmit
		const bool entering = (CosTheta(lightDir) > 0.f);
		const float eta = entering ? (nc / ntEff) : ntc;

		float3 wh = eta * lightDir + eyeDir;
		if (wh.z < 0.f)
			wh = -wh;

		const float lengthSquared = dot(wh, wh);
		if (!(lengthSquared > 0.f)) {
			*result = BLACK;
			*event = NONE;
			*directPdfW = 0.f;
			return;
		}
		wh /= sqrt(lengthSquared);
		const float cosThetaI = fabs(CosTheta(eyeDir));
		const float cosThetaIH = fabs(dot(eyeDir, wh));
		const float cosThetaOH = dot(lightDir, wh);

		const float D = Microfacet_GgxD(wh, alpha, alpha);
		const float G = Microfacet_GgxG2(lightDir, eyeDir, alpha, alpha);
		const float specPdfD = Microfacet_GgxVNDFHalfPdf(eyeDir, wh, alpha, alpha);
#if defined(SLG_SPECTRAL)
		const float3 F = Spectral_DispersiveFresnelR(nt, nc, cauchyB, sellB, sellC, cosThetaOH, hitPoint);
#else
		const float3 F = MAKE_FLOAT3(FresnelCauchy_Evaluate(ntc, cosThetaOH),
				FresnelCauchy_Evaluate(ntc, cosThetaOH), FresnelCauchy_Evaluate(ntc, cosThetaOH));
#endif

		const bool fromLight = (hitPoint->rayFlags & LIGHT_RAY) != 0;
		*directPdfW = threshold * specPdfD *
				(fromLight ? fabs(cosThetaIH) :
						(fabs(cosThetaOH) * eta * eta)) / lengthSquared;

		*result = (fabs(cosThetaOH) * cosThetaIH * D *
				G / (cosThetaI * lengthSquared)) *
				kt * (WHITE - F);
		*event = GLOSSY | TRANSMIT;
	} else {
		// Reflect
		const float cosThetaO = fabs(CosTheta(lightDir));
		const float cosThetaI = fabs(CosTheta(eyeDir));
		if ((cosThetaO == 0.f) || (cosThetaI == 0.f)) {
			*result = BLACK;
			*event = NONE;
			*directPdfW = 0.f;
			return;
		}
		float3 wh = lightDir + eyeDir;
		if (wh.x == 0.f && wh.y == 0.f && wh.z == 0.f) {
			*result = BLACK;
			*event = NONE;
			*directPdfW = 0.f;
			return;
		}
		wh = normalize(wh);
		if (wh.z < 0.f)
			wh = -wh;

		const float cosThetaH = dot(eyeDir, wh);
		const float D = Microfacet_GgxD(wh, alpha, alpha);
		const float G = Microfacet_GgxG2(lightDir, eyeDir, alpha, alpha);
		const float specPdf = Microfacet_GgxVNDFReflectionPdf(eyeDir, wh, alpha, alpha);
#if defined(SLG_SPECTRAL)
		const float3 F = Spectral_DispersiveFresnelR(nt, nc, cauchyB, sellB, sellC, cosThetaH, hitPoint);
#else
		const float3 F = MAKE_FLOAT3(FresnelCauchy_Evaluate(ntc, cosThetaH),
				FresnelCauchy_Evaluate(ntc, cosThetaH), FresnelCauchy_Evaluate(ntc, cosThetaH));
#endif

		*directPdfW = (1.f - threshold) * specPdf;
		// Regularized delta lobe is single-scatter: no multibounce.
		*result = (D * G / (4.f * cosThetaI)) * kr * F;
		*event = GLOSSY | REFLECT;
	}
}

OPENCL_FORCE_INLINE void GlassMaterial_Evaluate(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 lightDir, eyeDir;
	EvalStack_PopFloat3(eyeDir);
	EvalStack_PopFloat3(lightDir);

	// PSR parity with CPU GlassMaterial::Evaluate: under regularization
	// the vertex answers the isotropic GGX-dielectric lobe
	// (GlassMicrofacet_Evaluate, allowTransmit = true).
	if (hitPoint->regularization > 0.f) {
		float3 result;
		BSDFEvent event;
		float directPdfW;
		GlassMaterial_GGXDielectricEval(material, hitPoint, lightDir, eyeDir,
				&result, &event, &directPdfW MATERIALS_PARAM);
		EvalStack_PushFloat3(result);
		EvalStack_PushBSDFEvent(event);
		EvalStack_PushFloat(directPdfW);
		return;
	}

	MATERIAL_EVALUATE_RETURN_BLACK;
}

OPENCL_FORCE_INLINE void GlassMaterial_Sample(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float u0, u1, passThroughEvent;
	EvalStack_PopFloat(passThroughEvent);
	EvalStack_PopFloat(u1);
	EvalStack_PopFloat(u0);
	float3 fixedDir;
	EvalStack_PopFloat3(fixedDir);

	const float3 ktTexVal = Texture_GetSpectrumValue(material->glass.ktTexIndex, hitPoint TEXTURES_PARAM);
	const float3 krTexVal = Texture_GetSpectrumValue(material->glass.krTexIndex, hitPoint TEXTURES_PARAM);
	const float3 kt = Spectrum_Clamp(ktTexVal);
	const float3 kr = Spectrum_Clamp(krTexVal);

	const float nc = ExtractExteriorIors(hitPoint, material->glass.exteriorIorTexIndex TEXTURES_PARAM);
	const float nt = ExtractInteriorIors(hitPoint, material->glass.interiorIorTexIndex TEXTURES_PARAM);

	const float cauchyB = (material->glass.cauchyBTex != NULL_INDEX) ? Texture_GetFloatValue(material->glass.cauchyBTex, hitPoint TEXTURES_PARAM) : -1.f;
	// sellB.x >= 0 selects Sellmeier dispersion (sellmeierb/c textures)
	const float3 sellB = (material->glass.sellmeierBTex != NULL_INDEX &&
			material->glass.sellmeierCTex != NULL_INDEX) ?
			Texture_GetSpectrumValue(material->glass.sellmeierBTex, hitPoint TEXTURES_PARAM) :
			MAKE_FLOAT3(-1.f, 0.f, 0.f);
	const float3 sellC = (sellB.x >= 0.f) ?
			Texture_GetSpectrumValue(material->glass.sellmeierCTex, hitPoint TEXTURES_PARAM) :
			BLACK;

	// PSR parity with CPU GlassMaterial::Sample: a regularized glass
	// vertex scatters the isotropic GGX-dielectric lobe (alpha = sigma),
	// allowTransmit = true - same structure as RoughGlassMaterial_Sample
	// with useGgx forced and no multibounce/thin-film extras.
	if (hitPoint->regularization > 0.f) {
		if (fabs(fixedDir.z) < DEFAULT_COS_EPSILON_STATIC) {
			MATERIAL_SAMPLE_RETURN_BLACK;
		}
		const bool isKtBlack = Spectrum_IsBlack(kt);
		const bool isKrBlack = Spectrum_IsBlack(kr);
		if (isKtBlack && isKrBlack) {
			MATERIAL_SAMPLE_RETURN_BLACK;
		}
		const float alpha = hitPoint->regularization;
#if defined(SLG_SPECTRAL)
		// Dispersion: the direction-defining wavelength is the hero bin
		const float ntEff = Spectral_DispersiveIOR(nt, cauchyB, sellB, sellC, hitPoint);
#else
		const float ntEff = Spectral_RefIOR(nt, cauchyB, sellB, sellC);
#endif
		const float ntc = ntEff / nc;

		float3 wh = Microfacet_GgxSampleVNDF(fixedDir, alpha, alpha, u0, u1);
		const float specPdf = Microfacet_GgxVNDFHalfPdf(fixedDir, wh, alpha, alpha);
		if (specPdf <= 0.f) {
			MATERIAL_SAMPLE_RETURN_BLACK;
		}
		if (wh.z < 0.f)
			wh = -wh;
		const float cosThetaOH = dot(fixedDir, wh);

		float threshold;
		if (!isKrBlack) {
			threshold = isKtBlack ? 0.f : .5f;
		} else {
			if (!isKtBlack)
				threshold = 1.f;
			else {
				MATERIAL_SAMPLE_RETURN_BLACK;
			}
		}

		float3 sampledDir;
		BSDFEvent event;
		float pdfW;
		float3 result;
		if (passThroughEvent < threshold) {
			// Transmit
			const bool entering = (CosTheta(fixedDir) > 0.f);
			const float eta = entering ? (nc / ntEff) : ntc;
			const float eta2 = eta * eta;
			const float sinThetaIH2 = eta2 * fmax(0.f, 1.f - cosThetaOH * cosThetaOH);
			if (sinThetaIH2 >= 1.f) {
				MATERIAL_SAMPLE_RETURN_BLACK;
			}
			float cosThetaIH = sqrt(1.f - sinThetaIH2);
			if (entering)
				cosThetaIH = -cosThetaIH;
			const float length = eta * cosThetaOH + cosThetaIH;
			sampledDir = length * wh - eta * fixedDir;

			const float lengthSquared = length * length;
			pdfW = specPdf * fabs(cosThetaIH) / lengthSquared;
			if (pdfW <= 0.f) {
				MATERIAL_SAMPLE_RETURN_BLACK;
			}

			// (f*cos)/pdf = kt*(1-F)*G2/G1(wo) with VNDF sampling
			const float g1 = Microfacet_GgxG1(fixedDir, alpha, alpha);
			if (g1 <= 0.f) {
				MATERIAL_SAMPLE_RETURN_BLACK;
			}
			const float g2 = Microfacet_GgxG2(sampledDir, fixedDir, alpha, alpha);
			const bool fromLight = (hitPoint->rayFlags & LIGHT_RAY) != 0;
#if defined(SLG_SPECTRAL)
			const float3 F = Spectral_DispersiveFresnelR(nt, nc, cauchyB, sellB, sellC,
					fromLight ? cosThetaOH : cosThetaIH, hitPoint);
#else
			const float3 F = MAKE_FLOAT3(FresnelCauchy_Evaluate(ntc,
						fromLight ? cosThetaOH : cosThetaIH),
					FresnelCauchy_Evaluate(ntc, fromLight ? cosThetaOH : cosThetaIH),
					FresnelCauchy_Evaluate(ntc, fromLight ? cosThetaOH : cosThetaIH));
#endif
			result = kt * (WHITE - F) * (g2 / (g1 * threshold));

			pdfW *= threshold;
			event = GLOSSY | TRANSMIT;
#if defined(SLG_SPECTRAL)
			// Dispersive refraction terminates the secondary wavelengths
			if ((cauchyB > 0.f) || (sellB.x >= 0.f))
				result *= Spectral_CollapseToHero(&((__global HitPoint *)hitPoint)->spectralHeroAlive);
#endif
		} else {
			// Reflect
			pdfW = specPdf / (4.f * fabs(cosThetaOH));
			if (pdfW <= 0.f) {
				MATERIAL_SAMPLE_RETURN_BLACK;
			}
			sampledDir = 2.f * cosThetaOH * wh - fixedDir;

			const float cosi = fabs(sampledDir.z);
			if ((cosi < DEFAULT_COS_EPSILON_STATIC) ||
					(fixedDir.z * sampledDir.z < 0.f)) {
				MATERIAL_SAMPLE_RETURN_BLACK;
			}

#if defined(SLG_SPECTRAL)
			const float3 F = Spectral_DispersiveFresnelR(nt, nc, cauchyB, sellB, sellC,
					cosThetaOH, hitPoint);
#else
			const float3 F = MAKE_FLOAT3(FresnelCauchy_Evaluate(ntc, cosThetaOH),
					FresnelCauchy_Evaluate(ntc, cosThetaOH),
					FresnelCauchy_Evaluate(ntc, cosThetaOH));
#endif
			const float g1 = Microfacet_GgxG1(fixedDir, alpha, alpha);
			if (g1 <= 0.f) {
				MATERIAL_SAMPLE_RETURN_BLACK;
			}
			result = kr * F *
					(Microfacet_GgxG2(sampledDir, fixedDir, alpha, alpha) /
					(g1 * (1.f - threshold)));

			pdfW *= (1.f - threshold);
			event = GLOSSY | REFLECT;
		}

		EvalStack_PushFloat3(result);
		EvalStack_PushFloat3(sampledDir);
		EvalStack_PushFloat(pdfW);
		EvalStack_PushBSDFEvent(event);
		return;
	}

	float3 transLocalSampledDir; 
	const float3 trans = GlassMaterial_EvalSpecularTransmission(hitPoint, fixedDir, u0,
			kt, nc, nt, cauchyB, sellB, sellC, &transLocalSampledDir);
	
	const float localFilmThickness = (material->glass.filmThicknessTexIndex != NULL_INDEX) 
									 ? Texture_GetFloatValue(material->glass.filmThicknessTexIndex, hitPoint TEXTURES_PARAM) : 0.f;
	const float localFilmIor = (localFilmThickness > 0.f && material->glass.filmIorTexIndex != NULL_INDEX) 
							   ? Texture_GetFloatValue(material->glass.filmIorTexIndex, hitPoint TEXTURES_PARAM) : 1.f;
	
	float3 reflLocalSampledDir;
	const float3 refl = GlassMaterial_EvalSpecularReflection(hitPoint, fixedDir,
			kr, nc, nt, cauchyB, sellB, sellC, &reflLocalSampledDir, localFilmThickness, localFilmIor);

	// Decide to transmit or reflect
	float threshold;
	if (!Spectrum_IsBlack(refl)) {
		if (!Spectrum_IsBlack(trans)) {
			// Importance sampling
			const float reflFilter = Spectrum_Filter(refl);
			const float transFilter = Spectrum_Filter(trans);
			threshold = transFilter / (reflFilter + transFilter);

			// A place an upper and lower limit to not under sample
			// reflection or transmission
			threshold = clamp(threshold, .25f, .75f);
		} else
			threshold = 0.f;
	} else {
		if (!Spectrum_IsBlack(trans))
			threshold = 1.f;
		else {
			MATERIAL_SAMPLE_RETURN_BLACK;
		}
	}

	float3 sampledDir;
	BSDFEvent event;
	float pdfW;
	float3 result;
	if (passThroughEvent < threshold) {
		// Transmit

		sampledDir = transLocalSampledDir;

		event = SPECULAR | TRANSMIT;
		pdfW = threshold;

		result = trans;
#if defined(SLG_SPECTRAL)
		if ((cauchyB > 0.f) || (sellB.x >= 0.f))
			// Dispersive transmission: only the hero wavelength survives
			// (uniform pick -> the surviving bin carries x BINS weight).
			// The alive mask is read back into the SampleResult after
			// BSDF_Sample so it survives the next intersection.
			result *= Spectral_CollapseToHero(&((__global HitPoint *)hitPoint)->spectralHeroAlive);
#endif
	} else {
		// Reflect

		sampledDir = reflLocalSampledDir;

		event = SPECULAR | REFLECT;
		pdfW = 1.f - threshold;
		
		result = refl;
	}

	result /= pdfW;

	EvalStack_PushFloat3(result);
	EvalStack_PushFloat3(sampledDir);
	EvalStack_PushFloat(pdfW);
	EvalStack_PushBSDFEvent(event);
}

//------------------------------------------------------------------------------
// Material specific EvalOp
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE void GlassMaterial_EvalOp(
		__global const Material* restrict material,
		const MaterialEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint
		MATERIALS_PARAM_DECL) {
	switch (evalType) {
		case EVAL_ALBEDO:
			GlassMaterial_Albedo(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_INTERIOR_VOLUME:
			GlassMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EXTERIOR_VOLUME:
			GlassMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EMITTED_RADIANCE:
			GlassMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_PASS_TROUGH_TRANSPARENCY:
			GlassMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_EVALUATE:
			GlassMaterial_Evaluate(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_SAMPLE:
			GlassMaterial_Sample(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
