#line 2 "materialdefs_funcs_openpbr.cl"

/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 *   Unless required by applicable law or agreed to in writing, software   *
 *   distributed under the License is distributed on an "AS IS" BASIS,     *
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or       *
 *   implied.                                                              *
 *   See the License for the specific language governing permissions and   *
 *   limitations under the License.                                        *
 ***************************************************************************/

//------------------------------------------------------------------------------
// OpenPBR Surface material (ASWF v1.1, lobe-mixture approximation)
// GPU twin of src/slg/materials/openpbr.cpp - keep the two in sync.
//------------------------------------------------------------------------------

#if defined(SLG_SPECTRAL)
// Defined in materialdefs_funcs_generic.cl; forward declaration keeps this
// file self-contained regardless of kernel source concatenation order.
float Spectral_WaveLength2IOR(const float waveLength, const float ior, const float B);
#endif

// Per-hitpoint evaluated parameters (mirrors OpenPBRMaterial::Params)
typedef struct {
	float3 baseColor, specColor, transColor, transScatter;
	float3 sssColor, sssRadiusScale, coatColor, fuzzColor;
	float baseWeight, metalness, diffuseRoughness;
	float specWeight, specRoughness, specAniso, specRotation, specIor;
	float transWeight, transDepth, transScatterAniso, dispersion;
	float sssWeight, sssRadius, sssAnisotropy;
	float coatWeight, coatRoughness, coatAniso, coatRotation, coatIor,
			coatDarkening;
	float fuzzWeight, fuzzRoughness;
	float filmWeight, filmThickness, filmIor;
	float extIor;
	// Non-zero when the material's interior volume is a homogeneous
	// albedo-parametrized SSS medium (its albedo already reproduces
	// subsurface_color, so the interface tint must stay white).
	int sssAlbedoMedium;
} OpenPBRParams;

OPENCL_FORCE_INLINE void OpenPBRMat_EvaluateParams(__global const Material* restrict material,
		__global const HitPoint *hitPoint, __private OpenPBRParams *p
		MATERIALS_PARAM_DECL) {
	p->baseColor = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.baseColorTexIndex, hitPoint TEXTURES_PARAM));
	p->baseWeight = clamp(Texture_GetFloatValue(material->openpbr.baseWeightTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->metalness = clamp(Texture_GetFloatValue(material->openpbr.baseMetalnessTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->diffuseRoughness = clamp(Texture_GetFloatValue(material->openpbr.baseDiffuseRoughnessTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);

	p->specWeight = clamp(Texture_GetFloatValue(material->openpbr.specWeightTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->specColor = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.specColorTexIndex, hitPoint TEXTURES_PARAM));
	p->specRoughness = clamp(Texture_GetFloatValue(material->openpbr.specRoughnessTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->specAniso = clamp(Texture_GetFloatValue(material->openpbr.specAnisotropyTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->specRotation = Texture_GetFloatValue(material->openpbr.specRotationTexIndex, hitPoint TEXTURES_PARAM);
	p->specIor = fmax(Texture_GetFloatValue(material->openpbr.specIorTexIndex, hitPoint TEXTURES_PARAM), 1.f);

	p->transWeight = clamp(Texture_GetFloatValue(material->openpbr.transWeightTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->transColor = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.transColorTexIndex, hitPoint TEXTURES_PARAM));
	p->transDepth = fmax(Texture_GetFloatValue(material->openpbr.transDepthTexIndex, hitPoint TEXTURES_PARAM), 0.f);
	p->transScatter = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.transScatterTexIndex, hitPoint TEXTURES_PARAM));
	p->transScatterAniso = clamp(Texture_GetFloatValue(material->openpbr.transScatterAnisoTexIndex, hitPoint TEXTURES_PARAM), -1.f, 1.f);
	p->dispersion = fmax(Texture_GetFloatValue(material->openpbr.dispersionTexIndex, hitPoint TEXTURES_PARAM), 0.f);

	p->sssWeight = clamp(Texture_GetFloatValue(material->openpbr.sssWeightTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->sssColor = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.sssColorTexIndex, hitPoint TEXTURES_PARAM));
	p->sssRadius = fmax(Texture_GetFloatValue(material->openpbr.sssRadiusTexIndex, hitPoint TEXTURES_PARAM), 0.f);
	p->sssRadiusScale = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.sssRadiusScaleTexIndex, hitPoint TEXTURES_PARAM));
	p->sssAnisotropy = clamp(Texture_GetFloatValue(material->openpbr.sssAnisotropyTexIndex, hitPoint TEXTURES_PARAM), -0.95f, 0.95f);

	p->coatWeight = clamp(Texture_GetFloatValue(material->openpbr.coatWeightTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->coatColor = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.coatColorTexIndex, hitPoint TEXTURES_PARAM));
	p->coatRoughness = clamp(Texture_GetFloatValue(material->openpbr.coatRoughnessTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->coatAniso = clamp(Texture_GetFloatValue(material->openpbr.coatAnisotropyTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->coatRotation = Texture_GetFloatValue(material->openpbr.coatRotationTexIndex, hitPoint TEXTURES_PARAM);
	p->coatIor = fmax(Texture_GetFloatValue(material->openpbr.coatIorTexIndex, hitPoint TEXTURES_PARAM), 1.f);
	p->coatDarkening = clamp(Texture_GetFloatValue(material->openpbr.coatDarkeningTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);

	p->fuzzWeight = clamp(Texture_GetFloatValue(material->openpbr.fuzzWeightTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	p->fuzzColor = Spectrum_Clamp(Texture_GetSpectrumValue(material->openpbr.fuzzColorTexIndex, hitPoint TEXTURES_PARAM));
	p->fuzzRoughness = clamp(Texture_GetFloatValue(material->openpbr.fuzzRoughnessTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);

	p->filmWeight = clamp(Texture_GetFloatValue(material->openpbr.filmWeightTexIndex, hitPoint TEXTURES_PARAM), 0.f, 1.f);
	// OpenPBR thin film thickness is in micrometers; convert to nm
	p->filmThickness = fmax(Texture_GetFloatValue(material->openpbr.filmThicknessTexIndex, hitPoint TEXTURES_PARAM), 0.f) * 1000.f;
	p->filmIor = fmax(Texture_GetFloatValue(material->openpbr.filmIorTexIndex, hitPoint TEXTURES_PARAM), 1.f);

	p->extIor = ExtractExteriorIors(hitPoint, NULL_INDEX TEXTURES_PARAM);

	const uint ivIdx = material->interiorVolumeIndex;
	p->sssAlbedoMedium = (ivIdx != NULL_INDEX) &&
			(mats[ivIdx].type == HOMOGENEOUS_VOL) &&
			(mats[ivIdx].volume.homogenous.sssAlbedoTexIndex != NULL_INDEX);
}

// specular_ior/exterior ratio blended toward the coat interface IOR by the
// coat coverage weight (specular_ior_ratio), with the TIR-preserving flip.
OPENCL_FORCE_INLINE float OpenPBRMat_EtaS(__private const OpenPBRParams *p, const float cauchyB,
		__global const HitPoint *hitPoint) {
	const float nS =
#if defined(SLG_SPECTRAL)
		Spectral_DispersiveIOR(p->specIor, cauchyB, hitPoint);
#else
		p->specIor;
#endif
	const float etaSC = nS / p->coatIor;
	const float coatTerm = (etaSC < 1.f) ? 1.f / etaSC : etaSC;
	return Lerp(p->coatWeight, nS / p->extIor, coatTerm);
}

//------------------------------------------------------------------------------
// Thin film: single-wavelength Airy reflectance (complex arithmetic on
// float2 = (re, im), 6-step summation per the OpenPBR reference)
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float2 cxmul(const float2 a, const float2 b) {
	return MAKE_FLOAT2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}
OPENCL_FORCE_INLINE float2 cxdiv(const float2 a, const float2 b) {
	const float d = b.x * b.x + b.y * b.y;
	return MAKE_FLOAT2((a.x * b.x + a.y * b.y) / d, (a.y * b.x - a.x * b.y) / d);
}
OPENCL_FORCE_INLINE float2 cxsqrt(const float2 z) {
	const float m = sqrt(z.x * z.x + z.y * z.y);
	const float re = sqrt(fmax(0.f, 0.5f * (m + z.x)));
	float im = sqrt(fmax(0.f, 0.5f * (m - z.x)));
	if (z.y < 0.f)
		im = -im;
	return MAKE_FLOAT2(re, im);
}
OPENCL_FORCE_INLINE float2 cxexpj(const float2 z) { // exp(i*z) = exp(-im)*(cos re, sin re)
	const float e = exp(-z.y);
	return MAKE_FLOAT2(e * cos(z.x), e * sin(z.x));
}

OPENCL_FORCE_INLINE float2 OpenPBRMat_CosRefracted(const float2 cosI,
		const float2 nI, const float2 nT) {
	const float2 sinI2 = MAKE_FLOAT2(1.f - cosI.x * cosI.x + cosI.y * cosI.y,
			-2.f * cosI.x * cosI.y);
	const float2 ratio = cxdiv(nI, nT);
	const float2 sinT2 = cxmul(ratio, cxmul(ratio, sinI2));
	float2 cosT = cxsqrt(MAKE_FLOAT2(1.f - sinT2.x, -sinT2.y));
	if (cxmul(nT, cosT).y < 0.f)
		cosT = -cosT;
	return cosT;
}

OPENCL_FORCE_INLINE void OpenPBRMat_FresnelCoeffs(const float2 cosI, const float2 cosT,
		const float2 nI, const float2 nT,
		__private float2 *rs, __private float2 *rp, __private float2 *ts, __private float2 *tp) {
	const float2 nIc = cxmul(nI, cosI), nTc = cxmul(nT, cosT);
	const float2 nIcT = cxmul(nI, cosT), nTcI = cxmul(nT, cosI);
	*rs = cxdiv(nIc - nTc, nIc + nTc);
	*rp = cxdiv(nTcI - nIcT, nTcI + nIcT);
	*ts = cxdiv(2.f * nIc, nIc + nTc);
	*tp = cxdiv(2.f * nIc, nTcI + nIcT);
}

// Film reflectance at wavelength lambda (nm): ambient n1, film n2/d, substrate
// n3 (complex). Mirrors the CPU ThinFilmR.
OPENCL_FORCE_INLINE float OpenPBRMat_ThinFilmR(const float cos1, const float n1,
		const float n2, const float2 n3, const float d, const float lambda) {
	const float2 cn1 = MAKE_FLOAT2(n1, 0.f), cn2 = MAKE_FLOAT2(n2, 0.f);
	const float2 cos1c = MAKE_FLOAT2(cos1, 0.f);
	const float2 cos2 = OpenPBRMat_CosRefracted(cos1c, cn1, cn2);
	const float2 cos3 = OpenPBRMat_CosRefracted(cos2, cn2, n3);

	float2 r12s, r12p, t12s, t12p, r23s, r23p, t23s, t23p;
	OpenPBRMat_FresnelCoeffs(cos1c, cos2, cn1, cn2, &r12s, &r12p, &t12s, &t12p);
	OpenPBRMat_FresnelCoeffs(cos2, cos3, cn2, n3, &r23s, &r23p, &t23s, &t23p);

	const float2 r21s = -r12s, r21p = -r12p;
	const float2 ratio = cxdiv(cxmul(cn2, cos2), cxmul(cn1, cos1c));
	const float2 t21s = cxmul(t12s, ratio), t21p = cxmul(t12p, ratio);

	const float2 delta = (4.f * M_PI_F * n2 * d / lambda) * cos2;
	const float2 phi = cxexpj(delta);

	const float2 rs = r12s + cxdiv(cxmul(cxmul(t12s, r23s), cxmul(t21s, phi)),
			MAKE_FLOAT2(1.f, 0.f) - cxmul(r21s, cxmul(r23s, phi)));
	const float2 rp = r12p + cxdiv(cxmul(cxmul(t12p, r23p), cxmul(t21p, phi)),
			MAKE_FLOAT2(1.f, 0.f) - cxmul(r21p, cxmul(r23p, phi)));

	return clamp(.5f * (rs.x * rs.x + rs.y * rs.y + rp.x * rp.x + rp.y * rp.y), 0.f, 1.f);
}

// Film-modulated Fresnel over the stack. Conductor: per-channel complex n3;
// dielectric: scalar n3 with optional Cauchy dispersion per bin.
OPENCL_FORCE_INLINE float3 OpenPBRMat_FilmFresnel(__global const HitPoint *hitPoint,
		const float cosI, const float etaFe, const float filmIor,
		const float thicknessNm, const float3 n3r, const float3 n3i,
		const bool conductor, const float dielectricN3, const float cauchyB) {
	const float nExt = filmIor / etaFe;
	float3 F = BLACK;
	for (uint i = 0; i < SLG_SPECTRAL_BINS; ++i) {
		float lambda;
#if defined(SLG_SPECTRAL)
		if (!(hitPoint->spectralHeroAlive & (1u << i)))
			continue;
		lambda = hitPoint->spectralW[i];
#else
		lambda = (i == 0) ? 615.f : ((i == 1) ? 540.f : 465.f);
#endif
		float2 n3;
		if (conductor)
			n3 = MAKE_FLOAT2((i == 0) ? n3r.x : ((i == 1) ? n3r.y : n3r.z),
					(i == 0) ? n3i.x : ((i == 1) ? n3i.y : n3i.z));
		else {
			float nD = dielectricN3;
#if defined(SLG_SPECTRAL)
			if (cauchyB > 0.f)
				nD = Spectral_WaveLength2IOR(lambda, dielectricN3, cauchyB);
#endif
			n3 = MAKE_FLOAT2(nD, 0.f);
		}
		const float r = OpenPBRMat_ThinFilmR(cosI, nExt, filmIor, n3, thicknessNm, lambda);
		if (i == 0) F.x = r; else if (i == 1) F.y = r; else F.z = r;
	}
	return F;
}

//------------------------------------------------------------------------------
// Lobe evaluation (all return f * |cosI|)
//------------------------------------------------------------------------------

// Interior medium IOR as seen by a ray inside the object: the interior
// volume's when set (the implicit SSS/transmission volume carries
// specular_ior), else the substrate ior itself.
OPENCL_FORCE_INLINE float OpenPBRMat_InteriorIor(__global const HitPoint *hitPoint,
		__private const OpenPBRParams *p MATERIALS_PARAM_DECL) {
	if (hitPoint->interiorIorTexIndex != NULL_INDEX)
		return Texture_GetFloatValue(hitPoint->interiorIorTexIndex, hitPoint
				TEXTURES_PARAM);
#if defined(SLG_SPECTRAL)
	return Spectral_DispersiveIOR(p->specIor, p->dispersion, hitPoint);
#else
	return p->specIor;
#endif
}

OPENCL_FORCE_INLINE float3 OpenPBRMat_EvalGlossyRefl(__global const HitPoint *hitPoint,
		__private const OpenPBRParams *p, const bool coat,
		const float3 wo, const float3 wi, __private float *pdf
		MATERIALS_PARAM_DECL) {
	const float rot = coat ? p->coatRotation : p->specRotation;
	const float alpha = coat ? p->coatRoughness : p->specRoughness;
	const float aniso = coat ? p->coatAniso : p->specAniso;

	const float cosA = cos(-rot * 2.f * M_PI_F), sinA = sin(-rot * 2.f * M_PI_F);
	// Reflection is symmetric under a global flip: evaluate in the
	// canonical (+z) hemisphere, like the sampler's wFl.
	const bool woAbove = (wo.z > 0.f);
	const float3 wor = Microfacet_RotateXY(woAbove ? wo : -wo, cosA, sinA);
	const float3 wir = Microfacet_RotateXY((wi.z > 0.f) ? wi : -wi, cosA, sinA);

	float alphaT, alphaB;
	Microfacet_OpenPBRAnisoAlphas(alpha, aniso, &alphaT, &alphaB);

	const float3 wh = normalize(wor + wir); // wh.z > 0
	if (dot(wor, wh) <= 0.f || dot(wir, wh) <= 0.f)
		return BLACK; // backfacing microfacet

	const float D = Microfacet_GgxD(wh, alphaT, alphaB);
	*pdf = Microfacet_GgxVNDFReflectionPdf(wor, wh, alphaT, alphaB);

	// eta_ti = n(far side) / n(wo side); front face blends toward coat IOR.
	// Back face: wo side is the interior volume, far side the exterior.
	const float nNear = woAbove ? p->extIor :
			OpenPBRMat_InteriorIor(hitPoint, p MATERIALS_PARAM);
	const float nFar = woAbove ?
			(coat ? p->coatIor :
#if defined(SLG_SPECTRAL)
				Spectral_DispersiveIOR(p->specIor, p->dispersion, hitPoint)
#else
				p->specIor
#endif
			) :
			p->extIor;
	const float etaTI = (woAbove && !coat) ?
			OpenPBRMat_EtaS(p, p->dispersion, hitPoint) : nFar / nNear;
	if (fabs(etaTI - 1.f) < 1e-4f)
		return BLACK;

	const float mu = fabs(dot(wor, wh));
	float3 F;
	if (coat)
		F = MAKE_FLOAT3(1.f, 1.f, 1.f) * Microfacet_FresnelDielectric(mu, etaTI);
	else if (p->filmWeight > 0.f && p->filmThickness > 0.f) {
		const float etaFe = Lerp(p->coatWeight, p->filmIor / p->extIor,
				p->filmIor / p->coatIor);
		const float3 Ffilm = OpenPBRMat_FilmFresnel(hitPoint, mu, etaFe,
				p->filmIor, p->filmThickness, BLACK, BLACK, false,
				p->specIor, p->dispersion);
		const float Fnofilm = Microfacet_FresnelDielectricModulated(mu, etaTI, p->specWeight);
		F = Lerp3(p->filmWeight, MAKE_FLOAT3(Fnofilm, Fnofilm, Fnofilm), Ffilm);
	} else {
		const float f = Microfacet_FresnelDielectricModulated(mu, etaTI, p->specWeight);
		F = MAKE_FLOAT3(f, f, f);
	}

	const float G2 = Microfacet_GgxG2(wir, wor, alphaT, alphaB);
	return F * (D * G2 * fabs(wir.z) / fmax(4.f * fabs(wir.z * wor.z), 1e-7f));
}

OPENCL_FORCE_INLINE float3 OpenPBRMat_EvalMetal(__global const HitPoint *hitPoint,
		__private const OpenPBRParams *p, const float3 wo, const float3 wi, __private float *pdf
		MATERIALS_PARAM_DECL) {
	const float cosA = cos(-p->specRotation * 2.f * M_PI_F);
	const float sinA = sin(-p->specRotation * 2.f * M_PI_F);
	const float3 wor = Microfacet_RotateXY((wo.z > 0.f) ? wo : -wo, cosA, sinA);
	const float3 wir = Microfacet_RotateXY((wi.z > 0.f) ? wi : -wi, cosA, sinA);

	float alphaT, alphaB;
	Microfacet_OpenPBRAnisoAlphas(p->specRoughness, p->specAniso, &alphaT, &alphaB);

	const float3 wh = normalize(wor + wir);
	const float D = Microfacet_GgxD(wh, alphaT, alphaB);
	*pdf = Microfacet_GgxVNDFReflectionPdf(wor, wh, alphaT, alphaB);

	const float mu = fabs(dot(wor, wh));
	const float3 F0 = Spectrum_Clamp(p->baseWeight * p->baseColor);
	float3 F;
	if (p->filmWeight > 0.f && p->filmThickness > 0.f) {
		float3 n3r, n3i;
		Microfacet_GulbrandsenNK(F0, p->specColor, &n3r, &n3i);
		const float etaFe = Lerp(p->coatWeight, p->filmIor / p->extIor,
				p->filmIor / p->coatIor);
		F = Lerp3(p->filmWeight, Microfacet_FresnelF82(mu, F0, p->specColor),
				OpenPBRMat_FilmFresnel(hitPoint, mu, etaFe, p->filmIor,
						p->filmThickness, n3r, n3i, true, 0.f, 0.f));
	} else
		F = Microfacet_FresnelF82(mu, F0, p->specColor);

	const float G2 = Microfacet_GgxG2(wir, wor, alphaT, alphaB);
	return clamp(p->specWeight * F, BLACK, WHITE) *
			(D * G2 * fabs(wir.z) / fmax(4.f * fabs(wir.z * wor.z), 1e-7f));
}

// Dielectric refraction lobe (transmission and subsurface share the
// interface; the interior volume realizes their media).
// eta = n(wi side)/n(wo side), wh proportional to eta*wi + wo.
OPENCL_FORCE_INLINE float3 OpenPBRMat_EvalBtdf(__global const HitPoint *hitPoint,
		__private const OpenPBRParams *p, const float3 wo, const float3 wi, __private float *pdf
		MATERIALS_PARAM_DECL) {
	*pdf = 0.f;

	// Above the surface wo sits in the exterior medium and wi refracts into
	// the interior (specular_ior); below the surface wo sits in the
	// interior volume and wi exits into the exterior medium.
	const float nWo = (wo.z > 0.f) ? p->extIor :
			OpenPBRMat_InteriorIor(hitPoint, p MATERIALS_PARAM);
	const float nWi = (wo.z > 0.f) ?
#if defined(SLG_SPECTRAL)
			Spectral_DispersiveIOR(p->specIor, p->dispersion, hitPoint) :
#else
			p->specIor :
#endif
			p->extIor;
	const float eta = nWi / nWo;
	const float eta2 = eta * eta;

	// Index-matched interface: the lobe degenerates to a straight
	// pass-through delta (handled in Sample); there is no glossy
	// transmission to evaluate (wh = eta*wi + wo would vanish anyway).
	if (fabs(eta - 1.f) < 1e-4f)
		return BLACK;

	const float cosA = cos(-p->specRotation * 2.f * M_PI_F);
	const float sinA = sin(-p->specRotation * 2.f * M_PI_F);
	const float3 wor = Microfacet_RotateXY(wo, cosA, sinA);
	const float3 wir = Microfacet_RotateXY(wi, cosA, sinA);

	float alphaT, alphaB;
	Microfacet_OpenPBRAnisoAlphas(p->specRoughness, p->specAniso, &alphaT, &alphaB);

	if (wo.z * wi.z > 0.f) {
		// TIR fallback of the sample side reaches reflection directions:
		// report the reflection pdf; f is carried by the specular lobe.
		const float3 worf = (wor.z > 0.f) ? wor : -wor;
		const float3 wirf = (wir.z > 0.f) ? wir : -wir;
		const float3 whR = normalize(worf + wirf); // whR.z > 0
		if (dot(worf, whR) <= 0.f || dot(wirf, whR) <= 0.f)
			return BLACK;
		const float c = dot(worf, whR);
		const float sinT2 = (nWo / nWi) * (nWo / nWi) * (1.f - c * c);
		if (sinT2 >= 1.f)
			*pdf = Microfacet_GgxVNDFReflectionPdf(worf, whR, alphaT, alphaB);
		return BLACK;
	}

	float3 wh = eta * wir + wor;
	const float lengthSquared = dot(wh, wh);
	if (!(lengthSquared > 0.f))
		return BLACK;
	wh /= sqrt(lengthSquared);
	if (wh.z < 0.f)
		wh = -wh;

	const float D = Microfacet_GgxD(wh, alphaT, alphaB);
	const float woH = fabs(dot(wor, wh));
	const float wiH = dot(wir, wh);
	// VNDF pdf of wh * Jacobian |dwh/dwi| = |wi.h| * eta^2 / |wh_unnorm|^2
	*pdf = (D * Microfacet_GgxG1(wor, alphaT, alphaB) * woH / fmax(fabs(wor.z), 1e-7f)) *
			fabs(wiH) * eta2 / lengthSquared;

	// Transmission Fresnel on the wi side (n_t/n_i = 1/eta)
	const float F = Microfacet_FresnelDielectricModulated(fabs(wiH), 1.f / eta, p->specWeight);
	const float T = clamp(1.f - F, 0.f, 1.f);

	const float G2 = Microfacet_GgxG2(wir, wor, alphaT, alphaB);
	// f*cosI per the Walter dielectric refraction lobe; the |wi.h|*eta^2/|wh_u|^2
	// Jacobian lives in the pdf, so the sampled weight reduces to
	// T * G2 / (G1 * eta^2) — the (n_i/n_t)^2 radiance scaling matches the
	// specular glass convention (same as roughglass).
	return T * (fabs(wiH) * woH * D * G2 /
			fmax(fabs(wor.z) * lengthSquared, 1e-7f));
}

//------------------------------------------------------------------------------
// Lobe weights + mixture probabilities
//------------------------------------------------------------------------------

#define OPENPBR_LOBE_FUZZ 0u
#define OPENPBR_LOBE_COAT 1u
#define OPENPBR_LOBE_METAL 2u
#define OPENPBR_LOBE_SPEC 3u
#define OPENPBR_LOBE_BTDF 4u
#define OPENPBR_LOBE_DIFF 5u
#define OPENPBR_LOBE_COUNT 6u

OPENCL_FORCE_INLINE void OpenPBRMat_ComputeWeights(__global const HitPoint *hitPoint,
		__private const OpenPBRParams *p, const float3 wFixed,
		float3 weights[OPENPBR_LOBE_COUNT], float probs[OPENPBR_LOBE_COUNT]
		MATERIALS_PARAM_DECL) {
	const float muF = fabs(wFixed.z);

	weights[OPENPBR_LOBE_FUZZ] = Spectrum_Clamp(p->fuzzWeight * p->fuzzColor);
	const float impFuzz = (wFixed.z > 0.f) ?
			Spectrum_Filter(weights[OPENPBR_LOBE_FUZZ]) *
			Zeltner_DirAlbedo(muF, p->fuzzRoughness) : 0.f;

	weights[OPENPBR_LOBE_COAT] = Spectrum_Clamp(p->coatWeight * p->coatColor);
	const float coatF = (wFixed.z > 0.f && fabs(p->coatIor - p->extIor) > 1e-4f) ?
			Microfacet_FresnelDielectric(muF, p->coatIor / p->extIor) : 0.f;
	const float impCoat = Spectrum_Filter(weights[OPENPBR_LOBE_COAT]) * coatF;

	const float rem = fmax(0.f, 1.f - impFuzz - impCoat);
	const float3 darkening = Lerp3(p->coatDarkening, WHITE, p->coatColor);

	weights[OPENPBR_LOBE_METAL] = (rem * p->metalness) * darkening;
	const float3 F0 = Spectrum_Clamp(p->baseWeight * p->baseColor);
	const float impMetal = Spectrum_Filter(weights[OPENPBR_LOBE_METAL]) *
			Spectrum_Filter(p->specWeight * Microfacet_FresnelF82(muF, F0, p->specColor));

	const float etaS = OpenPBRMat_EtaS(p, p->dispersion, hitPoint);
	const float Fspec = Microfacet_FresnelDielectricModulated(muF, etaS, p->specWeight);
	const float wDiel = rem * (1.f - p->metalness);

	weights[OPENPBR_LOBE_SPEC] = wDiel * darkening * p->specColor;
	const float impSpec = Spectrum_Filter(weights[OPENPBR_LOBE_SPEC]) * Fspec;

	// Refraction: transmission + subsurface share the interface
	const float3 transTint = (p->transDepth > 0.f) ? WHITE : p->transColor;
	// An albedo-parametrized SSS volume already reproduces subsurface_color
	// as its diffuse reflectance, so the interface tint stays white to
	// avoid double-counting (same convention as transmission depth > 0).
	const float3 sssTint = p->sssAlbedoMedium ? WHITE : p->sssColor;
	const float3 refrTint = p->transWeight * transTint +
			(1.f - p->transWeight) * p->sssWeight * sssTint;
	weights[OPENPBR_LOBE_BTDF] = wDiel * darkening * refrTint;
	const float impBtdf = Spectrum_Filter(weights[OPENPBR_LOBE_BTDF]) * (1.f - Fspec);

	const float wDiff = wDiel * (1.f - p->transWeight) * (1.f - p->sssWeight);
	weights[OPENPBR_LOBE_DIFF] = wDiff * darkening * p->baseWeight * p->baseColor;
	const float impDiff = (wFixed.z > 0.f) ?
			Spectrum_Filter(weights[OPENPBR_LOBE_DIFF]) * (1.f - Fspec) : 0.f;

	float imp[OPENPBR_LOBE_COUNT];
	imp[OPENPBR_LOBE_FUZZ] = impFuzz;
	imp[OPENPBR_LOBE_COAT] = impCoat;
	imp[OPENPBR_LOBE_METAL] = impMetal;
	imp[OPENPBR_LOBE_SPEC] = impSpec;
	imp[OPENPBR_LOBE_BTDF] = impBtdf;
	imp[OPENPBR_LOBE_DIFF] = impDiff;

	float sum = 0.f;
	for (uint i = 0; i < OPENPBR_LOBE_COUNT; ++i)
		sum += imp[i];
	const float invSum = (sum > 0.f) ? 1.f / sum : 0.f;
	for (uint i = 0; i < OPENPBR_LOBE_COUNT; ++i)
		probs[i] = imp[i] * invSum;
}

OPENCL_FORCE_INLINE float OpenPBRMat_LobePdf(__global const HitPoint *hitPoint,
		__private const OpenPBRParams *p, const uint lobe,
		const float3 wo, const float3 wi
		MATERIALS_PARAM_DECL) {
	float pdf = 0.f;
	switch (lobe) {
		case OPENPBR_LOBE_FUZZ:
			return Zeltner_Pdf(wo, wi, p->fuzzRoughness);
		case OPENPBR_LOBE_COAT:
			OpenPBRMat_EvalGlossyRefl(hitPoint, p, true, wo, wi, &pdf MATERIALS_PARAM);
			return pdf;
		case OPENPBR_LOBE_METAL:
			OpenPBRMat_EvalMetal(hitPoint, p, wo, wi, &pdf MATERIALS_PARAM);
			return pdf;
		case OPENPBR_LOBE_SPEC:
			OpenPBRMat_EvalGlossyRefl(hitPoint, p, false, wo, wi, &pdf MATERIALS_PARAM);
			return pdf;
		case OPENPBR_LOBE_BTDF:
			OpenPBRMat_EvalBtdf(hitPoint, p, wo, wi, &pdf MATERIALS_PARAM);
			return pdf;
		case OPENPBR_LOBE_DIFF:
			return EON_Pdf(wo, wi, p->diffuseRoughness);
		default:
			return 0.f;
	}
}

//------------------------------------------------------------------------------
// BSDF evaluation / sampling (GPU paths are always camera paths)
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 OpenPBRMat_EvaluateImpl(__global const HitPoint *hitPoint,
		__private const OpenPBRParams *p, const float3 lightDir, const float3 eyeDir,
		BSDFEvent *event, float *directPdfW
		MATERIALS_PARAM_DECL) {
	const float3 wi = lightDir;
	const float3 wo = eyeDir;
	const float3 wFixed = wo, wSmp = wi;

	float3 weights[OPENPBR_LOBE_COUNT];
	float probs[OPENPBR_LOBE_COUNT];
	OpenPBRMat_ComputeWeights(hitPoint, p, wFixed, weights, probs MATERIALS_PARAM);

	const float muI = fabs(wi.z);
	const bool sameHemisphere = (wi.z * wo.z) > 0.f;
	const bool frontSide = (wi.z > 0.f) && (wo.z > 0.f);

	float3 result = BLACK;
	float pdfF = 0.f;
	BSDFEvent ev = NONE;

	if (frontSide && probs[OPENPBR_LOBE_FUZZ] > 0.f) {
		result += weights[OPENPBR_LOBE_FUZZ] *
				Zeltner_EvalTimesCosI(wo, wi, p->fuzzRoughness);
		pdfF += probs[OPENPBR_LOBE_FUZZ] * OpenPBRMat_LobePdf(hitPoint, p,
				OPENPBR_LOBE_FUZZ, wFixed, wSmp MATERIALS_PARAM);
		ev |= DIFFUSE | REFLECT;
	}
	if (sameHemisphere) {
		if (probs[OPENPBR_LOBE_COAT] > 0.f) {
			float pdf;
			result += weights[OPENPBR_LOBE_COAT] * OpenPBRMat_EvalGlossyRefl(
					hitPoint, p, true, wo, wi, &pdf MATERIALS_PARAM);
			pdfF += probs[OPENPBR_LOBE_COAT] * OpenPBRMat_LobePdf(hitPoint, p,
					OPENPBR_LOBE_COAT, wFixed, wSmp MATERIALS_PARAM);
			ev |= GLOSSY | REFLECT;
		}
		if (probs[OPENPBR_LOBE_METAL] > 0.f) {
			float pdf;
			result += weights[OPENPBR_LOBE_METAL] * OpenPBRMat_EvalMetal(
					hitPoint, p, wo, wi, &pdf MATERIALS_PARAM);
			pdfF += probs[OPENPBR_LOBE_METAL] * OpenPBRMat_LobePdf(hitPoint, p,
					OPENPBR_LOBE_METAL, wFixed, wSmp MATERIALS_PARAM);
			ev |= GLOSSY | REFLECT;
		}
		if (Spectrum_Filter(weights[OPENPBR_LOBE_SPEC]) > 0.f) {
			// Evaluate even when probs[LOBE_SPEC] == 0 (specular_weight = 0):
			// the modulated Fresnel still reaches 1 at TIR directions, which
			// the BTDF sampler can produce via its reflection fallback.
			float pdf;
			result += weights[OPENPBR_LOBE_SPEC] * OpenPBRMat_EvalGlossyRefl(
					hitPoint, p, false, wo, wi, &pdf MATERIALS_PARAM);
			pdfF += probs[OPENPBR_LOBE_SPEC] * OpenPBRMat_LobePdf(hitPoint, p,
					OPENPBR_LOBE_SPEC, wFixed, wSmp MATERIALS_PARAM);
			ev |= GLOSSY | REFLECT;
		}
		if (probs[OPENPBR_LOBE_BTDF] > 0.f) {
			// BTDF sampling also reaches reflection directions via its TIR
			// fallback; EvalBtdf reports the reflection pdf there.
			float pdf;
			OpenPBRMat_EvalBtdf(hitPoint, p, wo, wi, &pdf MATERIALS_PARAM);
			pdfF += probs[OPENPBR_LOBE_BTDF] * pdf;
		}
		if (frontSide && probs[OPENPBR_LOBE_DIFF] > 0.f) {
			// Diffuse crosses the dielectric interface twice
			const float etaS = OpenPBRMat_EtaS(p, p->dispersion, hitPoint);
			const float att = (1.f - Microfacet_FresnelDielectricModulated(muI, etaS, p->specWeight)) *
					(1.f - Microfacet_FresnelDielectricModulated(fabs(wo.z), etaS, p->specWeight));
			result += weights[OPENPBR_LOBE_DIFF] * att *
					(EON_Eval(WHITE, p->diffuseRoughness, wi, wo) * muI);
			pdfF += probs[OPENPBR_LOBE_DIFF] * OpenPBRMat_LobePdf(hitPoint, p,
					OPENPBR_LOBE_DIFF, wFixed, wSmp MATERIALS_PARAM);
			ev |= DIFFUSE | REFLECT;
		}
	}
	if (!sameHemisphere && probs[OPENPBR_LOBE_BTDF] > 0.f) {
		float pdf;
		result += weights[OPENPBR_LOBE_BTDF] * OpenPBRMat_EvalBtdf(
				hitPoint, p, wo, wi, &pdf MATERIALS_PARAM);
		pdfF += probs[OPENPBR_LOBE_BTDF] * OpenPBRMat_LobePdf(hitPoint, p,
				OPENPBR_LOBE_BTDF, wFixed, wSmp MATERIALS_PARAM);
		ev |= GLOSSY | TRANSMIT;
	}

	*event = ev;
	if (directPdfW)
		*directPdfW = pdfF;

	return result;
}

//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void OpenPBRMat_Albedo(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	OpenPBRParams p;
	OpenPBRMat_EvaluateParams(material, hitPoint, &p MATERIALS_PARAM);

	const float3 diffuse = p.baseColor * (p.baseWeight * (1.f - p.metalness) *
			(1.f - p.transWeight) * (1.f - p.sssWeight));
	const float3 sss = p.sssColor * (p.baseWeight * (1.f - p.metalness) *
			(1.f - p.transWeight) * p.sssWeight);
	const float3 metal = clamp(p.specWeight * p.baseWeight * p.baseColor, BLACK, WHITE) *
			p.metalness;
	const float3 albedo = Spectrum_Clamp(diffuse + sss + metal +
			p.fuzzColor * p.fuzzWeight + p.coatWeight * p.coatColor);
	EvalStack_PushFloat3(albedo);
}

OPENCL_FORCE_INLINE void OpenPBRMat_GetInteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void OpenPBRMat_GetExteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void OpenPBRMat_GetPassThroughTransparency(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void OpenPBRMat_GetEmittedRadiance(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void OpenPBRMat_Evaluate(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 lightDir, eyeDir;
	EvalStack_PopFloat3(eyeDir);
	EvalStack_PopFloat3(lightDir);

	if (lightDir.z == 0.f || eyeDir.z == 0.f) {
		MATERIAL_EVALUATE_RETURN_BLACK;
	}

	OpenPBRParams p;
	OpenPBRMat_EvaluateParams(material, hitPoint, &p MATERIALS_PARAM);

	BSDFEvent event;
	float directPdfW;
	const float3 result = OpenPBRMat_EvaluateImpl(hitPoint, &p, lightDir, eyeDir,
			&event, &directPdfW MATERIALS_PARAM);

	if (Spectrum_IsBlack(result)) {
		MATERIAL_EVALUATE_RETURN_BLACK;
	}

	EvalStack_PushFloat3(result);
	EvalStack_PushBSDFEvent(event);
	EvalStack_PushFloat(directPdfW);
}

OPENCL_FORCE_INLINE void OpenPBRMat_Sample(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float u0, u1, passThroughEvent;
	EvalStack_PopFloat(passThroughEvent);
	EvalStack_PopFloat(u1);
	EvalStack_PopFloat(u0);
	float3 fixedDir;
	EvalStack_PopFloat3(fixedDir);

	if (fixedDir.z == 0.f) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	OpenPBRParams p;
	OpenPBRMat_EvaluateParams(material, hitPoint, &p MATERIALS_PARAM);

	float3 weights[OPENPBR_LOBE_COUNT];
	float probs[OPENPBR_LOBE_COUNT];
	OpenPBRMat_ComputeWeights(hitPoint, &p, fixedDir, weights, probs MATERIALS_PARAM);

	float cum[OPENPBR_LOBE_COUNT];
	cum[0] = probs[0];
	for (uint i = 1; i < OPENPBR_LOBE_COUNT; ++i)
		cum[i] = cum[i - 1] + probs[i];
	if (cum[OPENPBR_LOBE_COUNT - 1] <= 0.f) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	uint lobe = 0;
	while (lobe < OPENPBR_LOBE_COUNT - 1 && passThroughEvent > cum[lobe])
		++lobe;
	if (probs[lobe] <= 0.f) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	const float3 wo = fixedDir;
	const float3 wFl = (wo.z > 0.f) ? wo : -wo;
	bool sampledTransmit = false;
	float3 sampledDir;

	switch (lobe) {
		case OPENPBR_LOBE_FUZZ: {
			float pdf;
			sampledDir = Zeltner_Sample(wFl, p.fuzzRoughness, u0, u1, &pdf);
			break;
		}
		case OPENPBR_LOBE_COAT:
		case OPENPBR_LOBE_METAL:
		case OPENPBR_LOBE_SPEC: {
			const bool coat = (lobe == OPENPBR_LOBE_COAT);
			const float rot = coat ? p.coatRotation : p.specRotation;
			const float alpha = coat ? p.coatRoughness : p.specRoughness;
			const float aniso = coat ? p.coatAniso : p.specAniso;
			const float cosA = cos(-rot * 2.f * M_PI_F), sinA = sin(-rot * 2.f * M_PI_F);
			const float3 wor = Microfacet_RotateXY(wFl, cosA, sinA);
			float alphaT, alphaB;
			Microfacet_OpenPBRAnisoAlphas(alpha, aniso, &alphaT, &alphaB);
			float3 wh = Microfacet_GgxSampleVNDF(wor, alphaT, alphaB, u0, u1);
			if (wh.z < 0.f)
				wh = -wh;
			const float3 wir = 2.f * dot(wor, wh) * wh - wor;
			sampledDir = Microfacet_RotateXY(wir, cosA, -sinA);
			if (wo.z < 0.f)
				sampledDir = -sampledDir;
			break;
		}
		case OPENPBR_LOBE_BTDF: {
			const float cosA = cos(-p.specRotation * 2.f * M_PI_F);
			const float sinA = sin(-p.specRotation * 2.f * M_PI_F);
			const float3 wor = Microfacet_RotateXY(wFl, cosA, sinA);
			float alphaT, alphaB;
			Microfacet_OpenPBRAnisoAlphas(p.specRoughness, p.specAniso, &alphaT, &alphaB);
			float3 wh = Microfacet_GgxSampleVNDF(wor, alphaT, alphaB, u0, u1);
			if (wh.z < 0.f)
				wh = -wh;
			const float nWo = (wo.z > 0.f) ? p.extIor :
					OpenPBRMat_InteriorIor(hitPoint, &p MATERIALS_PARAM);
			const float nWi = (wo.z > 0.f) ?
#if defined(SLG_SPECTRAL)
					Spectral_DispersiveIOR(p.specIor, p.dispersion, hitPoint) :
#else
					p.specIor :
#endif
					p.extIor;
			const float eta = nWo / nWi; // n(fixed)/n(sampled)
			if (fabs(eta - 1.f) < 1e-4f) {
				// Index-matched interface: straight pass-through delta
				// (Fresnel is identically 0). Weighted by the lobe's
				// mixture share.
				const float3 result = weights[lobe] / probs[lobe];
				EvalStack_PushFloat3(result);
				const float3 dir = -wo;
				EvalStack_PushFloat3(dir);
				const float pdfW0 = probs[lobe];
				EvalStack_PushFloat(pdfW0);
				const BSDFEvent ev0 = SPECULAR | TRANSMIT;
				EvalStack_PushBSDFEvent(ev0);
				return;
			}
			const float c = dot(wor, wh);
			const float sinT2 = eta * eta * fmax(0.f, 1.f - c * c);
			if (sinT2 >= 1.f) {
				// Total internal reflection: reflect off the microfacet;
				// the specular lobe of the mixture scores the direction.
				const float3 wirR = 2.f * c * wh - wor;
				sampledDir = Microfacet_RotateXY(wirR, cosA, -sinA);
				if (wo.z < 0.f)
					sampledDir = -sampledDir;
				break;
			}
			float cosT = sqrt(1.f - sinT2);
			if (wor.z > 0.f)
				cosT = -cosT;
			const float3 wir = (eta * c + cosT) * wh - eta * wor;
			sampledDir = Microfacet_RotateXY(wir, cosA, -sinA);
			if (wo.z < 0.f)
				sampledDir = -sampledDir;
			sampledTransmit = true;
			break;
		}
		case OPENPBR_LOBE_DIFF: {
			float pdf;
			sampledDir = EON_Sample(wFl, p.diffuseRoughness, u0, u1, &pdf);
			break;
		}
		default:
			MATERIAL_SAMPLE_RETURN_BLACK;
	}

	const float3 localLightDir = sampledDir;
	const float3 localEyeDir = fixedDir;

	BSDFEvent event;
	float pdfW;
	float3 f = OpenPBRMat_EvaluateImpl(hitPoint, &p, localLightDir, localEyeDir,
			&event, &pdfW MATERIALS_PARAM);
	if (pdfW <= 0.f) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

#if defined(SLG_SPECTRAL)
	// Dispersive refraction terminates the secondary wavelengths
	if (sampledTransmit && p.dispersion > 0.f)
		f *= Spectral_CollapseToHero(&((__global HitPoint *)hitPoint)->spectralHeroAlive);
#endif

	const float3 result = f / pdfW;

	EvalStack_PushFloat3(result);
	EvalStack_PushFloat3(sampledDir);
	EvalStack_PushFloat(pdfW);
	EvalStack_PushBSDFEvent(event);
}

//------------------------------------------------------------------------------
// Material specific EvalOp
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE void OpenPBRMat_EvalOp(
		__global const Material* restrict material,
		const MaterialEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint
		MATERIALS_PARAM_DECL) {
	switch (evalType) {
		case EVAL_ALBEDO:
			OpenPBRMat_Albedo(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_INTERIOR_VOLUME:
			OpenPBRMat_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EXTERIOR_VOLUME:
			OpenPBRMat_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EMITTED_RADIANCE:
			OpenPBRMat_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_PASS_TROUGH_TRANSPARENCY:
			OpenPBRMat_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_EVALUATE:
			OpenPBRMat_Evaluate(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_SAMPLE:
			OpenPBRMat_Sample(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
