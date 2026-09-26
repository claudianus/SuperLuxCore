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

#include <complex>

#include "luxrays/core/color/spectral.h"
#include "slg/materials/openpbr.h"
#include "slg/materials/microfacet.h"
#include "slg/volumes/homogenous.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// OpenPBR Surface material
//
// ASWF OpenPBR Surface v1.1 as a lobe mixture (albedo-scaling approximation,
// spec "Reduction to a mixture of lobes"). See openpbr.h for references.
//------------------------------------------------------------------------------

OpenPBRMaterial::OpenPBRMaterial(
	TextureConstPtr frontTransp, TextureConstPtr backTransp,
	TextureConstPtr emitted, TextureConstPtr bump,
	TextureConstPtr baseColor, TextureConstPtr baseWeight,
	TextureConstPtr baseMetalness, TextureConstPtr baseDiffuseRoughness,
	TextureConstPtr specWeight, TextureConstPtr specColor,
	TextureConstPtr specRoughness, TextureConstPtr specAnisotropy,
	TextureConstPtr specRotation, TextureConstPtr specIor,
	TextureConstPtr transWeight, TextureConstPtr transColor,
	TextureConstPtr transDepth, TextureConstPtr transScatter,
	TextureConstPtr transScatterAniso, TextureConstPtr dispersion,
	TextureConstPtr sssWeight, TextureConstPtr sssColor,
	TextureConstPtr sssRadius, TextureConstPtr sssRadiusScale,
	TextureConstPtr sssAnisotropy,
	TextureConstPtr coatWeight, TextureConstPtr coatColor,
	TextureConstPtr coatRoughness, TextureConstPtr coatAnisotropy,
	TextureConstPtr coatRotation, TextureConstPtr coatIor,
	TextureConstPtr coatDarkening,
	TextureConstPtr fuzzWeight, TextureConstPtr fuzzColor,
	TextureConstPtr fuzzRoughness,
	TextureConstPtr filmWeight, TextureConstPtr filmThickness,
	TextureConstPtr filmIor) :
	Material(frontTransp, backTransp, emitted, bump),
	BaseColor(baseColor), BaseWeight(baseWeight),
	BaseMetalness(baseMetalness), BaseDiffuseRoughness(baseDiffuseRoughness),
	SpecularWeight(specWeight), SpecularColor(specColor),
	SpecularRoughness(specRoughness), SpecularAnisotropy(specAnisotropy),
	SpecularRotation(specRotation), SpecularIor(specIor),
	TransmissionWeight(transWeight), TransmissionColor(transColor),
	TransmissionDepth(transDepth), TransmissionScatter(transScatter),
	TransmissionScatterAniso(transScatterAniso), Dispersion(dispersion),
	SubsurfaceWeight(sssWeight), SubsurfaceColor(sssColor),
	SubsurfaceRadius(sssRadius), SubsurfaceRadiusScale(sssRadiusScale),
	SubsurfaceAnisotropy(sssAnisotropy),
	CoatWeight(coatWeight), CoatColor(coatColor),
	CoatRoughness(coatRoughness), CoatAnisotropy(coatAnisotropy),
	CoatRotation(coatRotation), CoatIor(coatIor),
	CoatDarkening(coatDarkening),
	FuzzWeight(fuzzWeight), FuzzColor(fuzzColor), FuzzRoughness(fuzzRoughness),
	FilmWeight(filmWeight), FilmThickness(filmThickness), FilmIor(filmIor) {
}

//------------------------------------------------------------------------------
// Parameter evaluation
//------------------------------------------------------------------------------

void OpenPBRMaterial::EvaluateParams(const HitPoint &hitPoint, Params &p) const {
	p.baseColor = BaseColor->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	p.baseWeight = Clamp(BaseWeight->GetFloatValue(hitPoint), 0.f, 1.f);
	p.metalness = Clamp(BaseMetalness->GetFloatValue(hitPoint), 0.f, 1.f);
	p.diffuseRoughness = Clamp(BaseDiffuseRoughness->GetFloatValue(hitPoint), 0.f, 1.f);

	p.specWeight = Clamp(SpecularWeight->GetFloatValue(hitPoint), 0.f, 1.f);
	p.specColor = SpecularColor->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	p.specRoughness = Clamp(SpecularRoughness->GetFloatValue(hitPoint), 0.f, 1.f);
	p.specAniso = Clamp(SpecularAnisotropy->GetFloatValue(hitPoint), 0.f, 1.f);
	p.specRotation = SpecularRotation->GetFloatValue(hitPoint);
	p.specIor = Max(SpecularIor->GetFloatValue(hitPoint), 1.f);

	p.transWeight = Clamp(TransmissionWeight->GetFloatValue(hitPoint), 0.f, 1.f);
	p.transColor = TransmissionColor->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	p.transDepth = Max(TransmissionDepth->GetFloatValue(hitPoint), 0.f);
	p.transScatter = TransmissionScatter->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	p.transScatterAniso = Clamp(TransmissionScatterAniso->GetFloatValue(hitPoint), -1.f, 1.f);
	p.dispersion = Max(Dispersion->GetFloatValue(hitPoint), 0.f);

	p.sssWeight = Clamp(SubsurfaceWeight->GetFloatValue(hitPoint), 0.f, 1.f);
	p.sssColor = SubsurfaceColor->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	p.sssRadius = Max(SubsurfaceRadius->GetFloatValue(hitPoint), 0.f);
	p.sssRadiusScale = SubsurfaceRadiusScale->GetSpectrumValue(hitPoint).Clamp(0.f, 10.f);
	p.sssAnisotropy = Clamp(SubsurfaceAnisotropy->GetFloatValue(hitPoint), -0.95f, 0.95f);

	p.coatWeight = Clamp(CoatWeight->GetFloatValue(hitPoint), 0.f, 1.f);
	p.coatColor = CoatColor->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	p.coatRoughness = Clamp(CoatRoughness->GetFloatValue(hitPoint), 0.f, 1.f);
	p.coatAniso = Clamp(CoatAnisotropy->GetFloatValue(hitPoint), 0.f, 1.f);
	p.coatRotation = CoatRotation->GetFloatValue(hitPoint);
	p.coatIor = Max(CoatIor->GetFloatValue(hitPoint), 1.f);
	p.coatDarkening = Clamp(CoatDarkening->GetFloatValue(hitPoint), 0.f, 1.f);

	p.fuzzWeight = Clamp(FuzzWeight->GetFloatValue(hitPoint), 0.f, 1.f);
	p.fuzzColor = FuzzColor->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	p.fuzzRoughness = Clamp(FuzzRoughness->GetFloatValue(hitPoint), 0.f, 1.f);

	p.filmWeight = Clamp(FilmWeight->GetFloatValue(hitPoint), 0.f, 1.f);
	// OpenPBR thin film thickness is in micrometers; convert to nm
	p.filmThickness = Max(FilmThickness->GetFloatValue(hitPoint), 0.f) * 1000.f;
	p.filmIor = Max(FilmIor->GetFloatValue(hitPoint), 1.f);

	p.extIor = ExtractExteriorIors(hitPoint, nullptr);
}

// specular_ior / exterior ratio blended toward the coat interface IOR by the
// coat coverage weight (spec eq. specular_ior_ratio), with the TIR-preserving
// ratio flip. Dispersion (Cauchy B) shifts the substrate IOR at the hero
// wavelength.
float OpenPBRMaterial::EtaS(const Params &p, const float cauchyB) const {
	const float nS = DispersiveIOR(p.specIor, cauchyB);
	const float etaSC = nS / p.coatIor;
	const float coatTerm = (etaSC < 1.f) ? 1.f / etaSC : etaSC;
	return Lerp(p.coatWeight, nS / p.extIor, coatTerm);
}

float OpenPBRMaterial::InteriorIor(const HitPoint &hitPoint, const Params &p) const {
	return hitPoint.interiorVolume ? hitPoint.interiorVolume->GetIOR(hitPoint) :
			DispersiveIOR(p.specIor, p.dispersion);
}

//------------------------------------------------------------------------------
// Thin film: single-wavelength Airy reflectance (complex arithmetic, 6-step
// summation per the OpenPBR reference implementation)
//------------------------------------------------------------------------------

typedef complex<float> cxf;

static cxf CxSqrt(const cxf &z) { return sqrt(z); }

// cos(theta_t) by complex Snell refraction
static cxf CosRefracted(const cxf &cosI, const cxf &nI, const cxf &nT) {
	const cxf sinI2 = 1.f - cosI * cosI;
	const cxf ratio = nI / nT;
	const cxf sinT2 = ratio * ratio * sinI2;
	cxf cosT = CxSqrt(1.f - sinT2);
	if ((nT * cosT).imag() < 0.f)
		cosT = -cosT;
	return cosT;
}

static void FresnelCoeffs(const cxf &cosI, const cxf &cosT,
		const cxf &nI, const cxf &nT,
		cxf &rs, cxf &rp, cxf &ts, cxf &tp) {
	rs = (nI * cosI - nT * cosT) / (nI * cosI + nT * cosT);
	rp = (nT * cosI - nI * cosT) / (nT * cosI + nI * cosT);
	ts = 2.f * nI * cosI / (nI * cosI + nT * cosT);
	tp = 2.f * nI * cosI / (nT * cosI + nI * cosT);
}

// Unpolarized power reflectance of a film (n2, thickness d nm) between
// ambient n1 and substrate n3 (complex) at wavelength lambda (nm).
static float ThinFilmR(const float cos1, const float n1, const float n2,
		const cxf &n3, const float d, const float lambda) {
	const cxf cn1(n1, 0.f), cn2(n2, 0.f);
	const cxf cos2 = CosRefracted(cxf(cos1, 0.f), cn1, cn2);
	const cxf cos3 = CosRefracted(cos2, cn2, n3);

	cxf r12s, r12p, t12s, t12p, r23s, r23p, t23s, t23p;
	FresnelCoeffs(cxf(cos1, 0.f), cos2, cn1, cn2, r12s, r12p, t12s, t12p);
	FresnelCoeffs(cos2, cos3, cn2, n3, r23s, r23p, t23s, t23p);

	const cxf r21s = -r12s, r21p = -r12p;
	const cxf ratio = cn2 * cos2 / (cn1 * cxf(cos1, 0.f));
	const cxf t21s = t12s * ratio, t21p = t12p * ratio;

	const cxf delta = cxf(4.f * float(M_PI) * n2 * d / lambda, 0.f) * cos2;
	const cxf phi = exp(cxf(-delta.imag(), delta.real()));

	const cxf rs = r12s + t12s * r23s * t21s * phi / (1.f - r21s * r23s * phi);
	const cxf rp = r12p + t12p * r23p * t21p * phi / (1.f - r21p * r23p * phi);

	return Clamp(.5f * (norm(rs) + norm(rp)), 0.f, 1.f);
}

// Film-modulated Fresnel over the material stack. n3r/n3i: per-channel
// substrate complex IOR (conductor) or real IOR with n3i=0 (dielectric).
// Dispersion cauchyB disperses a dielectric substrate per wavelength.
static Spectrum FilmFresnel(const HitPoint &hitPoint, const float cosI,
		const float etaFe, const float filmIor, const float thicknessNm,
		const Spectrum &n3r, const Spectrum &n3i, const bool conductor,
		const float dielectricN3, const float cauchyB) {
	const float nExt = filmIor / etaFe;
	const PathWavelengths *sw = Spectral::Current();
	Spectrum F(0.f);
	for (u_int i = 0; i < SPECTRAL_BINS; ++i) {
		if (sw && !(sw->aliveMask & (1U << i)))
			continue;
		// Reference wavelengths in non-spectral mode (approx. RGB primaries)
		const float lambda = sw ? sw->w[i] : (i == 0 ? 615.f : (i == 1 ? 540.f : 465.f));
		cxf n3;
		if (conductor)
			n3 = cxf(n3r.c[i], n3i.c[i]);
		else {
			const float nD = (cauchyB > 0.f) ?
					WaveLength2IOR(lambda, dielectricN3, cauchyB) : dielectricN3;
			n3 = cxf(nD, 0.f);
		}
		F.c[i] = ThinFilmR(cosI, nExt, filmIor, n3, thicknessNm, lambda);
	}
	return F;
}

//------------------------------------------------------------------------------
// Lobe evaluation helpers (all return f * |cosI|, LuxCore convention)
//------------------------------------------------------------------------------

// Shared GGX dielectric/coating reflection lobe. weight tint + modulated F
// are applied by the caller. etaTI: transmitted/incident IOR ratio for the
// reflection hemisphere.
Spectrum OpenPBRMaterial::EvalGlossyRefl(const HitPoint &hitPoint, const Params &p,
		const bool coat,
		const Vector &wo, const Vector &wi, float &pdf) const {
	const float rot = coat ? p.coatRotation : p.specRotation;
	const float alpha = coat ? p.coatRoughness : p.specRoughness;
	const float aniso = coat ? p.coatAniso : p.specAniso;

	const float cosA = cosf(-rot * 2.f * M_PI), sinA = sinf(-rot * 2.f * M_PI);
	// Reflection is symmetric under a global flip: evaluate the microfacet
	// math with both directions in the canonical (+z) hemisphere, like the
	// sampler's wFl. Without this the half vector lands below z = 0 for
	// interior rays and the backfacing test kills every reflection.
	const bool woAbove = (wo.z > 0.f);
	const Vector wor = RotateXY(woAbove ? wo : -wo, cosA, sinA);
	const Vector wir = RotateXY((wi.z > 0.f) ? wi : -wi, cosA, sinA);

	float alphaT, alphaB;
	OpenPBRAnisoAlphas(alpha, aniso, alphaT, alphaB);

	const Vector wh = Normalize(wor + wir); // wh.z > 0
	if (Dot(wor, wh) <= 0.f || Dot(wir, wh) <= 0.f)
		return Spectrum(0.f); // backfacing microfacet

	const float D = GgxD(wh, alphaT, alphaB);
	pdf = GgxVNDFReflectionPdf(wor, wh, alphaT, alphaB);

	// eta_ti = n(far side) / n(wo side). Front: coat -> coat_ior, spec ->
	// coat-blended substrate ratio (specular_ior_ratio). Back face: the wo
	// side is the interior volume, the far side is the exterior medium.
	const float nNear = woAbove ? p.extIor : InteriorIor(hitPoint, p);
	const float nFar = woAbove ?
			(coat ? p.coatIor : DispersiveIOR(p.specIor, p.dispersion)) :
			p.extIor;
	const float etaTI = (woAbove && !coat) ? EtaS(p, p.dispersion) : nFar / nNear;
	if (fabsf(etaTI - 1.f) < 1e-4f)
		return Spectrum(0.f); // index-matched interface: no reflection

	const float mu = fabsf(Dot(wor, wh));
	float F;
	if (coat)
		F = FresnelDielectric(mu, etaTI);
	else if (p.filmWeight > 0.f && p.filmThickness > 0.f) {
		const float etaFe = Lerp(p.coatWeight, p.filmIor / p.extIor,
				p.filmIor / p.coatIor);
		const Spectrum Ffilm = FilmFresnel(hitPoint, mu, etaFe,
				p.filmIor, p.filmThickness, Spectrum(0.f), Spectrum(0.f), false,
				p.specIor, p.dispersion);
		const float Fnofilm = FresnelDielectricModulated(mu, etaTI, p.specWeight);
		F = Lerp(p.filmWeight, Fnofilm, Ffilm.Filter());
	} else
		F = FresnelDielectricModulated(mu, etaTI, p.specWeight);

	const float G2 = GgxG2(wir, wor, alphaT, alphaB);
	return Spectrum(F) * (D * G2 * fabsf(wir.z) / Max(4.f * fabsf(wir.z * wor.z), 1e-7f));
}

// Conductor reflection lobe (F82 tint + optional thin film).
Spectrum OpenPBRMaterial::EvalMetal(const HitPoint &hitPoint, const Params &p,
		const Vector &wo, const Vector &wi, float &pdf) const {
	const float cosA = cosf(-p.specRotation * 2.f * M_PI);
	const float sinA = sinf(-p.specRotation * 2.f * M_PI);
	const Vector wor = RotateXY((wo.z > 0.f) ? wo : -wo, cosA, sinA);
	const Vector wir = RotateXY((wi.z > 0.f) ? wi : -wi, cosA, sinA);

	float alphaT, alphaB;
	OpenPBRAnisoAlphas(p.specRoughness, p.specAniso, alphaT, alphaB);

	const Vector wh = Normalize(wor + wir);
	const float D = GgxD(wh, alphaT, alphaB);
	pdf = GgxVNDFReflectionPdf(wor, wh, alphaT, alphaB);

	const float mu = fabsf(Dot(wor, wh));
	const Spectrum F0 = (p.baseWeight * p.baseColor).Clamp(0.f, 1.f);
	Spectrum F;
	if (p.filmWeight > 0.f && p.filmThickness > 0.f) {
		Spectrum n3r, n3i;
		GulbrandsenNK(F0, p.specColor, n3r, n3i);
		const float etaFe = Lerp(p.coatWeight, p.filmIor / p.extIor,
				p.filmIor / p.coatIor);
		F = Lerp(p.filmWeight, FresnelF82(mu, F0, p.specColor),
				FilmFresnel(hitPoint, mu, etaFe, p.filmIor, p.filmThickness,
						n3r, n3i, true, 0.f, 0.f));
	} else
		F = FresnelF82(mu, F0, p.specColor);

	const float G2 = GgxG2(wir, wor, alphaT, alphaB);
	return (p.specWeight * F).Clamp(0.f, 1.f) *
			(D * G2 * fabsf(wir.z) / Max(4.f * fabsf(wir.z * wor.z), 1e-7f));
}

// Dielectric refraction lobe (shared by transmission and bulk subsurface;
// the two differ only in the interior medium, handled by the volume system).
// LuxCore refraction convention: eta = n(wi side)/n(wo side), wh ∝ eta*wi + wo.
Spectrum OpenPBRMaterial::EvalBtdf(const HitPoint &hitPoint, const Params &p,
		const Vector &wo, const Vector &wi, float &pdf) const {
	pdf = 0.f;

	// eta = n(wi side) / n(wo side). Above the surface wo sits in the
	// exterior medium and wi refracts into the material interior
	// (specular_ior, dispersed); below the surface wo sits in the interior
	// volume and wi exits into the exterior medium.
	const float nWo = (wo.z > 0.f) ? p.extIor : InteriorIor(hitPoint, p);
	const float nWi = (wo.z > 0.f) ? DispersiveIOR(p.specIor, p.dispersion) :
			p.extIor;
	const float eta = nWi / nWo;
	const float eta2 = eta * eta;

	// Index-matched interface: the lobe degenerates to a straight
	// pass-through delta (handled in Sample); there is no glossy
	// transmission to evaluate (wh = eta*wi + wo would vanish anyway).
	if (fabsf(eta - 1.f) < 1e-4f)
		return Spectrum(0.f);

	const float cosA = cosf(-p.specRotation * 2.f * M_PI);
	const float sinA = sinf(-p.specRotation * 2.f * M_PI);
	const Vector wor = RotateXY(wo, cosA, sinA);
	const Vector wir = RotateXY(wi, cosA, sinA);

	float alphaT, alphaB;
	OpenPBRAnisoAlphas(p.specRoughness, p.specAniso, alphaT, alphaB);

	if (wo.z * wi.z > 0.f) {
		// Same hemisphere: this lobe still reaches reflection directions
		// when the sampled microfacet cannot refract (TIR). The sample
		// side falls back to reflection with the VNDF density of wh, so
		// the direction's pdf is the reflection pdf; f is carried by the
		// specular lobe (Fresnel = 1 at TIR).
		const Vector worf = (wor.z > 0.f) ? wor : -wor;
		const Vector wirf = (wir.z > 0.f) ? wir : -wir;
		const Vector wh = Normalize(worf + wirf); // wh.z > 0
		if (Dot(worf, wh) <= 0.f || Dot(wirf, wh) <= 0.f)
			return Spectrum(0.f);
		const float c = Dot(worf, wh);
		const float sinT2 = Sqr(nWo / nWi) * (1.f - c * c);
		if (sinT2 >= 1.f)
			pdf = GgxVNDFReflectionPdf(worf, wh, alphaT, alphaB);
		return Spectrum(0.f);
	}

	Vector wh = eta * wir + wor;
	const float lengthSquared = wh.LengthSquared();
	if (!(lengthSquared > 0.f))
		return Spectrum(0.f);
	wh /= sqrtf(lengthSquared);
	if (wh.z < 0.f)
		wh = -wh;

	const float D = GgxD(wh, alphaT, alphaB);
	const float woH = fabsf(Dot(wor, wh));
	const float wiH = Dot(wir, wh);
	// VNDF pdf of wh * Jacobian |dwh/dwi| = |wi.h| * eta^2 / |wh_unnorm|^2
	pdf = (D * GgxG1(wor, alphaT, alphaB) * woH / Max(fabsf(wor.z), 1e-7f)) *
			fabsf(wiH) * eta2 / lengthSquared;

	// Transmission Fresnel on the wi side: n_t/n_i = n(wo)/n(wi) = 1/eta
	// (specular_weight modulation applies per the OpenPBR spec; the refraction
	// direction itself uses the physical ratio above).
	const float F = FresnelDielectricModulated(fabsf(wiH), 1.f / eta, p.specWeight);
	const float T = Clamp(1.f - F, 0.f, 1.f);

	const float G2 = GgxG2(wir, wor, alphaT, alphaB);
	// f*cosI per the Walter dielectric refraction lobe; the |wi.h|*eta^2/|wh_u|^2
	// Jacobian lives in the pdf, so the sampled weight reduces to
	// T * G2 / (G1 * eta^2) — the (n_i/n_t)^2 radiance scaling matches the
	// specular glass convention (same as roughglass).
	return Spectrum(T) * (fabsf(wiH) * woH * D * G2 /
			Max(fabsf(wor.z) * lengthSquared, 1e-7f));
}

//------------------------------------------------------------------------------
// Lobe weights + mixture probabilities (albedo-scaling lobe mixture)
//
// weights[] carry the coverage/tint factors only; each lobe's own Fresnel/
// albedo lives inside its f. probs[] additionally multiply an estimate of the
// lobe's directional albedo so selection follows actual reflectance.
//------------------------------------------------------------------------------

void OpenPBRMaterial::ComputeWeights(const HitPoint &hitPoint, const Params &p,
		const Vector &wFixed,
		Spectrum weights[LOBE_COUNT], float probs[LOBE_COUNT]) const {
	const float muF = fabsf(wFixed.z);

	// Fuzz: top layer, statistically unoccluded (Zeltner directional albedo)
	weights[LOBE_FUZZ] = (p.fuzzWeight * p.fuzzColor).Clamp(0.f, 1.f);
	const float impFuzz = (wFixed.z > 0.f) ?
			weights[LOBE_FUZZ].Filter() * zeltner::DirAlbedo(muF, p.fuzzRoughness) : 0.f;

	// Coat: coverage * tint; the directional Fresnel is the lobe albedo
	weights[LOBE_COAT] = (p.coatWeight * p.coatColor).Clamp(0.f, 1.f);
	const float coatF = (wFixed.z > 0.f && fabsf(p.coatIor - p.extIor) > 1e-4f) ?
			FresnelDielectric(muF, p.coatIor / p.extIor) : 0.f;
	const float impCoat = weights[LOBE_COAT].Filter() * coatF;

	// Energy left for the base substrate; coat_color additionally darkens it
	const float rem = Max(0.f, 1.f - impFuzz - impCoat);
	const Spectrum darkening = Lerp(p.coatDarkening, Spectrum(1.f), p.coatColor);

	// Metal
	weights[LOBE_METAL] = Spectrum(rem * p.metalness) * darkening;
	const Spectrum F0 = (p.baseWeight * p.baseColor).Clamp(0.f, 1.f);
	const float impMetal = weights[LOBE_METAL].Filter() *
			Spectrum(p.specWeight * FresnelF82(muF, F0, p.specColor)).Filter();

	// Dielectric substrate: directional Fresnel splits reflection/refraction
	const float etaS = EtaS(p, p.dispersion);
	const float Fspec = FresnelDielectricModulated(muF, etaS, p.specWeight);
	const float wDiel = rem * (1.f - p.metalness);

	weights[LOBE_SPEC] = Spectrum(wDiel) * darkening * p.specColor;
	const float impSpec = weights[LOBE_SPEC].Filter() * Fspec;

	// Refraction lobe: transmission and subsurface share the interface; the
	// interior volume (auto-created by the parser) realizes their media.
	// OpenPBR: depth > 0 -> the volume absorbs, the surface tint is white.
	const Spectrum transTint = (p.transDepth > 0.f) ? Spectrum(1.f) : p.transColor;
	// An albedo-parametrized SSS volume already reproduces subsurface_color
	// as its diffuse reflectance, so the interface tint stays white to
	// avoid double-counting (same convention as transmission depth > 0).
	bool sssAlbedoMedium = false;
	if (const VolumeConstPtr iv = GetInteriorVolume()) {
		const HomogeneousVolume *hv = dynamic_cast<const HomogeneousVolume *>(&*iv);
		sssAlbedoMedium = hv && hv->IsSSSParametrized();
	}
	const Spectrum sssTint = sssAlbedoMedium ? Spectrum(1.f) : p.sssColor;
	const Spectrum refrTint = p.transWeight * transTint +
			(1.f - p.transWeight) * p.sssWeight * sssTint;
	weights[LOBE_BTDF] = Spectrum(wDiel) * darkening * refrTint;
	const float impBtdf = weights[LOBE_BTDF].Filter() * (1.f - Fspec);

	// Opaque diffuse share (attenuated by interface transmission at eval)
	const float wDiff = wDiel * (1.f - p.transWeight) * (1.f - p.sssWeight);
	weights[LOBE_DIFF] = Spectrum(wDiff) * darkening * p.baseWeight * p.baseColor;
	const float impDiff = (wFixed.z > 0.f) ?
			weights[LOBE_DIFF].Filter() * (1.f - Fspec) : 0.f;

	weights[LOBE_SSS] = Spectrum(0.f);

	const float imp[LOBE_COUNT] = {
		impFuzz, impCoat, impMetal, impSpec, impBtdf, impDiff, 0.f
	};
	float sum = 0.f;
	for (u_int i = 0; i < LOBE_COUNT; ++i)
		sum += imp[i];
	const float invSum = (sum > 0.f) ? 1.f / sum : 0.f;
	for (u_int i = 0; i < LOBE_COUNT; ++i)
		probs[i] = imp[i] * invSum;
}

// PDF of sampling `wi` given fixed `wo` for a single lobe.
float OpenPBRMaterial::LobePdf(const HitPoint &hitPoint, const Params &p,
		const LobeId lobe, const Vector &wo, const Vector &wi) const {
	float pdf = 0.f;
	switch (lobe) {
		case LOBE_FUZZ:
			return zeltner::Pdf(wo, wi, p.fuzzRoughness);
		case LOBE_COAT:
			EvalGlossyRefl(hitPoint, p, true, wo, wi, pdf);
			return pdf;
		case LOBE_METAL:
			EvalMetal(hitPoint, p, wo, wi, pdf);
			return pdf;
		case LOBE_SPEC:
			EvalGlossyRefl(hitPoint, p, false, wo, wi, pdf);
			return pdf;
		case LOBE_BTDF:
		case LOBE_SSS:
			EvalBtdf(hitPoint, p, wo, wi, pdf);
			return pdf;
		case LOBE_DIFF:
			return eon::Pdf(wo, wi, p.diffuseRoughness);
		default:
			return 0.f;
	}
}

//------------------------------------------------------------------------------
// BSDF evaluation
//------------------------------------------------------------------------------

Spectrum OpenPBRMaterial::EvalInternal(const HitPoint &hitPoint, const Params &p,
		const Vector &localLightDir, const Vector &localEyeDir,
		BSDFEvent *event, float *directPdfW, float *reversePdfW) const {
	const Vector &wi = localLightDir;
	const Vector &wo = localEyeDir;
	const Vector &wFixed = hitPoint.fromLight ? wi : wo;
	const Vector &wSmp = hitPoint.fromLight ? wo : wi;

	Spectrum weights[LOBE_COUNT];
	float probs[LOBE_COUNT];
	ComputeWeights(hitPoint, p, wFixed, weights, probs);

	const float muI = fabsf(wi.z);
	const bool sameHemisphere = (wi.z * wo.z) > 0.f;
	const bool frontSide = (wi.z > 0.f) && (wo.z > 0.f);

	Spectrum result(0.f);
	float pdfF = 0.f;
	BSDFEvent ev = NONE;

	if (frontSide && probs[LOBE_FUZZ] > 0.f) {
		// f * cosI = weight * E(muO) * D(wi|wo)
		result += weights[LOBE_FUZZ] *
				zeltner::EvalTimesCosI(wo, wi, p.fuzzRoughness);
		pdfF += probs[LOBE_FUZZ] * LobePdf(hitPoint, p, LOBE_FUZZ, wFixed, wSmp);
		ev = BSDFEvent(ev | DIFFUSE | REFLECT);
	}
	if (sameHemisphere) {
		if (probs[LOBE_COAT] > 0.f) {
			float pdf;
			result += weights[LOBE_COAT] * EvalGlossyRefl(hitPoint, p, true, wo, wi, pdf);
			pdfF += probs[LOBE_COAT] * LobePdf(hitPoint, p, LOBE_COAT, wFixed, wSmp);
			ev = BSDFEvent(ev | GLOSSY | REFLECT);
		}
		if (probs[LOBE_METAL] > 0.f) {
			float pdf;
			result += weights[LOBE_METAL] * EvalMetal(hitPoint, p, wo, wi, pdf);
			pdfF += probs[LOBE_METAL] * LobePdf(hitPoint, p, LOBE_METAL, wFixed, wSmp);
			ev = BSDFEvent(ev | GLOSSY | REFLECT);
		}
		if (weights[LOBE_SPEC].Filter() > 0.f) {
			// Evaluate even when probs[LOBE_SPEC] == 0 (specular_weight = 0):
			// the modulated Fresnel still reaches 1 at TIR directions, which
			// the BTDF sampler can produce via its reflection fallback.
			float pdf;
			result += weights[LOBE_SPEC] * EvalGlossyRefl(hitPoint, p, false, wo, wi, pdf);
			pdfF += probs[LOBE_SPEC] * LobePdf(hitPoint, p, LOBE_SPEC, wFixed, wSmp);
			ev = BSDFEvent(ev | GLOSSY | REFLECT);
		}
		if (probs[LOBE_BTDF] > 0.f) {
			// BTDF sampling can also land here via its TIR reflection
			// fallback; EvalBtdf reports the reflection pdf for those
			// directions (f is contributed by the specular lobe above).
			float pdf;
			EvalBtdf(hitPoint, p, wo, wi, pdf);
			pdfF += probs[LOBE_BTDF] * pdf;
		}
		if (frontSide && probs[LOBE_DIFF] > 0.f) {
			// Diffuse is attenuated crossing the dielectric interface twice
			const float etaS = EtaS(p, p.dispersion);
			const float att = (1.f - FresnelDielectricModulated(muI, etaS, p.specWeight)) *
					(1.f - FresnelDielectricModulated(fabsf(wo.z), etaS, p.specWeight));
			result += weights[LOBE_DIFF] * att *
					(eon::Eval(Spectrum(1.f), p.diffuseRoughness, wi, wo) * muI);
			pdfF += probs[LOBE_DIFF] * LobePdf(hitPoint, p, LOBE_DIFF, wFixed, wSmp);
			ev = BSDFEvent(ev | DIFFUSE | REFLECT);
		}
	}
	if (!sameHemisphere && probs[LOBE_BTDF] > 0.f) {
		float pdf;
		result += weights[LOBE_BTDF] * EvalBtdf(hitPoint, p, wo, wi, pdf);
		pdfF += probs[LOBE_BTDF] * LobePdf(hitPoint, p, LOBE_BTDF, wFixed, wSmp);
		ev = BSDFEvent(ev | GLOSSY | TRANSMIT);
	}

	if (event)
		*event = ev;
	if (directPdfW)
		*directPdfW = pdfF;
	if (reversePdfW) {
		// Reverse: sample wFixed from wSmp; coverage recomputed at wSmp
		Spectrum rWeights[LOBE_COUNT];
		float rProbs[LOBE_COUNT];
		ComputeWeights(hitPoint, p, wSmp, rWeights, rProbs);
		float pdfR = 0.f;
		if (frontSide && rProbs[LOBE_FUZZ] > 0.f)
			pdfR += rProbs[LOBE_FUZZ] * LobePdf(hitPoint, p, LOBE_FUZZ, wSmp, wFixed);
		if (sameHemisphere) {
			if (rProbs[LOBE_COAT] > 0.f)
				pdfR += rProbs[LOBE_COAT] * LobePdf(hitPoint, p, LOBE_COAT, wSmp, wFixed);
			if (rProbs[LOBE_METAL] > 0.f)
				pdfR += rProbs[LOBE_METAL] * LobePdf(hitPoint, p, LOBE_METAL, wSmp, wFixed);
			if (rProbs[LOBE_SPEC] > 0.f)
				pdfR += rProbs[LOBE_SPEC] * LobePdf(hitPoint, p, LOBE_SPEC, wSmp, wFixed);
			if (rProbs[LOBE_BTDF] > 0.f)
				pdfR += rProbs[LOBE_BTDF] * LobePdf(hitPoint, p, LOBE_BTDF, wSmp, wFixed);
			if (frontSide && rProbs[LOBE_DIFF] > 0.f)
				pdfR += rProbs[LOBE_DIFF] * LobePdf(hitPoint, p, LOBE_DIFF, wSmp, wFixed);
		}
		if (!sameHemisphere && rProbs[LOBE_BTDF] > 0.f)
			pdfR += rProbs[LOBE_BTDF] * LobePdf(hitPoint, p, LOBE_BTDF, wSmp, wFixed);
		*reversePdfW = pdfR;
	}

	return result;
}

Spectrum OpenPBRMaterial::Evaluate(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		BSDFEvent *event, float *directPdfW, float *reversePdfW) const {
	if (localLightDir.z == 0.f || localEyeDir.z == 0.f)
		return Spectrum();

	Params p;
	EvaluateParams(hitPoint, p);

	return EvalInternal(hitPoint, p, localLightDir, localEyeDir,
			event, directPdfW, reversePdfW);
}

Spectrum OpenPBRMaterial::Sample(const HitPoint &hitPoint,
		const Vector &localFixedDir, Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const {
	if (localFixedDir.z == 0.f)
		return Spectrum();

	Params p;
	EvaluateParams(hitPoint, p);

	Spectrum weights[LOBE_COUNT];
	float probs[LOBE_COUNT];
	ComputeWeights(hitPoint, p, localFixedDir, weights, probs);

	// Cumulative probabilities
	float cum[LOBE_COUNT];
	cum[0] = probs[0];
	for (u_int i = 1; i < LOBE_COUNT; ++i)
		cum[i] = cum[i - 1] + probs[i];
	if (cum[LOBE_COUNT - 1] <= 0.f)
		return Spectrum();

	u_int lobe = 0;
	while (lobe < LOBE_COUNT - 1 && passThroughEvent > cum[lobe])
		++lobe;
	if (probs[lobe] <= 0.f)
		return Spectrum();

	const Vector &wo = localFixedDir;
	const Vector wFl = (wo.z > 0.f) ? wo : -wo;
	bool sampledTransmit = false;

	switch (lobe) {
		case LOBE_FUZZ: {
			float pdf;
			*localSampledDir = zeltner::Sample(wFl, p.fuzzRoughness, u0, u1, pdf);
			break;
		}
		case LOBE_COAT:
		case LOBE_METAL:
		case LOBE_SPEC: {
			const bool coat = (lobe == LOBE_COAT);
			const float rot = coat ? p.coatRotation : p.specRotation;
			const float alpha = coat ? p.coatRoughness : p.specRoughness;
			const float aniso = coat ? p.coatAniso : p.specAniso;
			const float cosA = cosf(-rot * 2.f * M_PI), sinA = sinf(-rot * 2.f * M_PI);
			const Vector wor = RotateXY(wFl, cosA, sinA);
			float alphaT, alphaB;
			OpenPBRAnisoAlphas(alpha, aniso, alphaT, alphaB);
			Vector wh = GgxSampleVNDF(wor, alphaT, alphaB, u0, u1);
			if (wh.z < 0.f)
				wh = -wh;
			const Vector wir = 2.f * Dot(wor, wh) * wh - wor;
			*localSampledDir = RotateXY(wir, cosA, -sinA);
			if (wo.z < 0.f)
				*localSampledDir = -*localSampledDir;
			break;
		}
		case LOBE_BTDF:
		case LOBE_SSS: {
			const float cosA = cosf(-p.specRotation * 2.f * M_PI);
			const float sinA = sinf(-p.specRotation * 2.f * M_PI);
			const Vector wor = RotateXY(wFl, cosA, sinA);
			float alphaT, alphaB;
			OpenPBRAnisoAlphas(p.specRoughness, p.specAniso, alphaT, alphaB);
			Vector wh = GgxSampleVNDF(wor, alphaT, alphaB, u0, u1);
			if (wh.z < 0.f)
				wh = -wh;
			// eta' = n(fixed side)/n(sampled side) = 1/eta of EvalBtdf
			const float nWo = (wo.z > 0.f) ? p.extIor : InteriorIor(hitPoint, p);
			const float nWi = (wo.z > 0.f) ?
					DispersiveIOR(p.specIor, p.dispersion) : p.extIor;
			const float eta = nWo / nWi;
			if (fabsf(eta - 1.f) < 1e-4f) {
				// Index-matched interface: straight pass-through delta
				// (Fresnel is identically 0; the specular lobe has no
				// energy either). Weighted by the lobe's mixture share.
				*localSampledDir = -wo;
				*pdfW = probs[lobe];
				*event = SPECULAR | TRANSMIT;
				return weights[lobe] / probs[lobe];
			}
			const float c = Dot(wor, wh);
			const float sinT2 = eta * eta * Max(0.f, 1.f - c * c);
			if (sinT2 >= 1.f) {
				// Total internal reflection: reflect off the microfacet
				// instead of transmitting. The direction is scored by the
				// specular lobe of the mixture (Fresnel = 1 at TIR) and its
				// pdf is counted by EvalBtdf's same-hemisphere branch.
				const Vector wirR = 2.f * c * wh - wor;
				*localSampledDir = RotateXY(wirR, cosA, -sinA);
				if (wo.z < 0.f)
					*localSampledDir = -*localSampledDir;
				break;
			}
			float cosT = sqrtf(1.f - sinT2);
			if (wor.z > 0.f)
				cosT = -cosT;
			const Vector wir = (eta * c + cosT) * wh - eta * wor;
			*localSampledDir = RotateXY(wir, cosA, -sinA);
			if (wo.z < 0.f)
				*localSampledDir = -*localSampledDir;
			sampledTransmit = true;
			break;
		}
		case LOBE_DIFF: {
			float pdf;
			*localSampledDir = eon::Sample(wFl, p.diffuseRoughness, u0, u1, pdf);
			break;
		}
		default:
			return Spectrum();
	}

	const Vector &localLightDir = hitPoint.fromLight ? localFixedDir : *localSampledDir;
	const Vector &localEyeDir = hitPoint.fromLight ? *localSampledDir : localFixedDir;

	const Spectrum f = EvalInternal(hitPoint, p, localLightDir, localEyeDir,
			event, pdfW, nullptr);
	if (*pdfW <= 0.f)
		return Spectrum();

	// Dispersive refraction keeps only the hero wavelength alive
	if (sampledTransmit && p.dispersion > 0.f)
		return (f * Spectral::CollapseToHero()) / *pdfW;
	return f / *pdfW;
}

void OpenPBRMaterial::Pdf(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
	if (localLightDir.z == 0.f || localEyeDir.z == 0.f) {
		if (directPdfW)
			*directPdfW = 0.f;
		if (reversePdfW)
			*reversePdfW = 0.f;
		return;
	}

	Params p;
	EvaluateParams(hitPoint, p);
	EvalInternal(hitPoint, p, localLightDir, localEyeDir,
			nullptr, directPdfW, reversePdfW);
}

Spectrum OpenPBRMaterial::Albedo(const HitPoint &hitPoint) const {
	Params p;
	EvaluateParams(hitPoint, p);

	const Spectrum diffuse = p.baseColor * (p.baseWeight * (1.f - p.metalness) *
			(1.f - p.transWeight) * (1.f - p.sssWeight));
	const Spectrum sss = p.sssColor * (p.baseWeight * (1.f - p.metalness) *
			(1.f - p.transWeight) * p.sssWeight);
	const Spectrum metal = (p.specWeight * p.baseWeight * p.baseColor).Clamp(0.f, 1.f) *
			p.metalness;
	const Spectrum fuzz = p.fuzzColor * p.fuzzWeight;
	return (diffuse + sss + metal + fuzz + p.coatWeight * p.coatColor).Clamp(0.f, 1.f);
}

void OpenPBRMaterial::AddReferencedTextures(std::unordered_set<const Texture *> &referencedTexs) const {
	Material::AddReferencedTextures(referencedTexs);

	BaseColor->AddReferencedTextures(referencedTexs);
	BaseWeight->AddReferencedTextures(referencedTexs);
	BaseMetalness->AddReferencedTextures(referencedTexs);
	BaseDiffuseRoughness->AddReferencedTextures(referencedTexs);
	SpecularWeight->AddReferencedTextures(referencedTexs);
	SpecularColor->AddReferencedTextures(referencedTexs);
	SpecularRoughness->AddReferencedTextures(referencedTexs);
	SpecularAnisotropy->AddReferencedTextures(referencedTexs);
	SpecularRotation->AddReferencedTextures(referencedTexs);
	SpecularIor->AddReferencedTextures(referencedTexs);
	TransmissionWeight->AddReferencedTextures(referencedTexs);
	TransmissionColor->AddReferencedTextures(referencedTexs);
	TransmissionDepth->AddReferencedTextures(referencedTexs);
	TransmissionScatter->AddReferencedTextures(referencedTexs);
	TransmissionScatterAniso->AddReferencedTextures(referencedTexs);
	Dispersion->AddReferencedTextures(referencedTexs);
	SubsurfaceWeight->AddReferencedTextures(referencedTexs);
	SubsurfaceColor->AddReferencedTextures(referencedTexs);
	SubsurfaceRadius->AddReferencedTextures(referencedTexs);
	SubsurfaceRadiusScale->AddReferencedTextures(referencedTexs);
	SubsurfaceAnisotropy->AddReferencedTextures(referencedTexs);
	CoatWeight->AddReferencedTextures(referencedTexs);
	CoatColor->AddReferencedTextures(referencedTexs);
	CoatRoughness->AddReferencedTextures(referencedTexs);
	CoatAnisotropy->AddReferencedTextures(referencedTexs);
	CoatRotation->AddReferencedTextures(referencedTexs);
	CoatIor->AddReferencedTextures(referencedTexs);
	CoatDarkening->AddReferencedTextures(referencedTexs);
	FuzzWeight->AddReferencedTextures(referencedTexs);
	FuzzColor->AddReferencedTextures(referencedTexs);
	FuzzRoughness->AddReferencedTextures(referencedTexs);
	FilmWeight->AddReferencedTextures(referencedTexs);
	FilmThickness->AddReferencedTextures(referencedTexs);
	FilmIor->AddReferencedTextures(referencedTexs);
}

void OpenPBRMaterial::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	Material::UpdateTextureReferences(oldTex, newTex);

	if (BaseColor == &oldTex) BaseColor = &newTex;
	if (BaseWeight == &oldTex) BaseWeight = &newTex;
	if (BaseMetalness == &oldTex) BaseMetalness = &newTex;
	if (BaseDiffuseRoughness == &oldTex) BaseDiffuseRoughness = &newTex;
	if (SpecularWeight == &oldTex) SpecularWeight = &newTex;
	if (SpecularColor == &oldTex) SpecularColor = &newTex;
	if (SpecularRoughness == &oldTex) SpecularRoughness = &newTex;
	if (SpecularAnisotropy == &oldTex) SpecularAnisotropy = &newTex;
	if (SpecularRotation == &oldTex) SpecularRotation = &newTex;
	if (SpecularIor == &oldTex) SpecularIor = &newTex;
	if (TransmissionWeight == &oldTex) TransmissionWeight = &newTex;
	if (TransmissionColor == &oldTex) TransmissionColor = &newTex;
	if (TransmissionDepth == &oldTex) TransmissionDepth = &newTex;
	if (TransmissionScatter == &oldTex) TransmissionScatter = &newTex;
	if (TransmissionScatterAniso == &oldTex) TransmissionScatterAniso = &newTex;
	if (Dispersion == &oldTex) Dispersion = &newTex;
	if (SubsurfaceWeight == &oldTex) SubsurfaceWeight = &newTex;
	if (SubsurfaceColor == &oldTex) SubsurfaceColor = &newTex;
	if (SubsurfaceRadius == &oldTex) SubsurfaceRadius = &newTex;
	if (SubsurfaceRadiusScale == &oldTex) SubsurfaceRadiusScale = &newTex;
	if (SubsurfaceAnisotropy == &oldTex) SubsurfaceAnisotropy = &newTex;
	if (CoatWeight == &oldTex) CoatWeight = &newTex;
	if (CoatColor == &oldTex) CoatColor = &newTex;
	if (CoatRoughness == &oldTex) CoatRoughness = &newTex;
	if (CoatAnisotropy == &oldTex) CoatAnisotropy = &newTex;
	if (CoatRotation == &oldTex) CoatRotation = &newTex;
	if (CoatIor == &oldTex) CoatIor = &newTex;
	if (CoatDarkening == &oldTex) CoatDarkening = &newTex;
	if (FuzzWeight == &oldTex) FuzzWeight = &newTex;
	if (FuzzColor == &oldTex) FuzzColor = &newTex;
	if (FuzzRoughness == &oldTex) FuzzRoughness = &newTex;
	if (FilmWeight == &oldTex) FilmWeight = &newTex;
	if (FilmThickness == &oldTex) FilmThickness = &newTex;
	if (FilmIor == &oldTex) FilmIor = &newTex;
}

PropertiesUPtr OpenPBRMaterial::ToProperties(const ImageMapCache &imgMapCache,
		const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.materials." + name + ".type")("openpbr"));
	props->Set(Property("scene.materials." + name + ".basecolor")(BaseColor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".baseweight")(BaseWeight->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".basemetalness")(BaseMetalness->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".basediffuseroughness")(BaseDiffuseRoughness->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".specularweight")(SpecularWeight->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".specularcolor")(SpecularColor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".specularroughness")(SpecularRoughness->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".specularanisotropy")(SpecularAnisotropy->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".specularrotation")(SpecularRotation->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".specularior")(SpecularIor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".transmissionweight")(TransmissionWeight->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".transmissioncolor")(TransmissionColor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".transmissiondepth")(TransmissionDepth->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".transmissionscatter")(TransmissionScatter->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".transmissionscatteranisotropy")(TransmissionScatterAniso->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".dispersion")(Dispersion->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".subsurfaceweight")(SubsurfaceWeight->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".subsurfacecolor")(SubsurfaceColor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".subsurfaceradius")(SubsurfaceRadius->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".subsurfaceradiusscale")(SubsurfaceRadiusScale->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".subsurfaceanisotropy")(SubsurfaceAnisotropy->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".coatweight")(CoatWeight->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".coatcolor")(CoatColor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".coatroughness")(CoatRoughness->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".coatanisotropy")(CoatAnisotropy->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".coatrotation")(CoatRotation->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".coatior")(CoatIor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".coatdarkening")(CoatDarkening->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".fuzzweight")(FuzzWeight->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".fuzzcolor")(FuzzColor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".fuzzroughness")(FuzzRoughness->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".filmweight")(FilmWeight->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".filmthickness")(FilmThickness->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".filmior")(FilmIor->GetSDLValue()));
	props->Set(Material::ToProperties(imgMapCache, useRealFileName));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
