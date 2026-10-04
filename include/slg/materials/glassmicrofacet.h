// SPDX-License-Identifier: Apache-2.0
// Shared microfacet-GGX dielectric/reflector lobe used by the delta
// materials (glass, mirror) when path-space regularization promotes a
// secondary vertex to a rough lobe (hitPoint.regularization > 0):
// BSDF::IsDelta() reports non-delta for those vertices, so NEE and the
// strategy caches can connect, and the BSDF must answer
// Evaluate/Sample/Pdf like a real glossy dielectric. The math is the
// GGX half of RoughGlassMaterial (isotropic alphaT=alphaB=reg), kept
// as free functions so both CPU material classes and the OpenCL
// kernels converge on identical behaviour.
#pragma once

#include "slg/materials/material.h"
#include "slg/materials/microfacet.h"
#include "slg/textures/fresnel/fresneltexture.h"
#include "luxrays/core/color/spectral.h"

namespace slg {


// Evaluate: full microfacet-GGX dielectric/reflector lobe - the
// regularized vertex must answer light connects (NEE / MIS). Math is
// the GGX half of RoughGlassMaterial::Evaluate with isotropic
// alphaT = alphaB = hitPoint.regularization.
inline luxrays::Spectrum GlassMicrofacet_Evaluate(
		const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir,
		const luxrays::Vector &localEyeDir,
		BSDFEvent *event,
		float *directPdfW, float *reversePdfW,
		const luxrays::Spectrum &kr,
		const luxrays::Spectrum &kt,
		const float nc, const float nt, const Dispersion &disp,
		const bool allowTransmit) {
	const bool isKtBlack = !allowTransmit || kt.Black();
	const bool isKrBlack = kr.Black();
	if (isKtBlack && isKrBlack)
		return luxrays::Spectrum();

	const float alpha = hitPoint.regularization;
	if (alpha <= 0.f)
		return luxrays::Spectrum();

	const float ntEff = DispersiveIOR(nt, disp);
	const float ntc = ntEff / nc;

	const float threshold = isKrBlack ? 1.f : (isKtBlack ? 0.f : .5f);
	if (localLightDir.z * localEyeDir.z < 0.f) {
		// Transmit
		const bool entering = (luxrays::CosTheta(localLightDir) > 0.f);
		const float eta = entering ? (nc / ntEff) : ntc;

		luxrays::Vector wh = eta * localLightDir + localEyeDir;
		if (wh.z < 0.f)
			wh = -wh;

		const float lengthSquared = wh.LengthSquared();
		if (!(lengthSquared > 0.f))
			return luxrays::Spectrum();
		wh /= sqrtf(lengthSquared);
		const float cosThetaI = fabsf(luxrays::CosTheta(localEyeDir));
		const float cosThetaIH = luxrays::AbsDot(localEyeDir, wh);
		const float cosThetaOH = luxrays::Dot(localLightDir, wh);

		const float D = GgxD(wh, alpha, alpha);
		const float G = GgxG2(localLightDir, localEyeDir, alpha, alpha);
		const float specPdfD = GgxVNDFHalfPdf(localEyeDir, wh, alpha, alpha);
		const float specPdfR = GgxVNDFHalfPdf(localLightDir, wh, alpha, alpha);
		const luxrays::Spectrum F = DispersiveFresnelR(nt, nc, disp, cosThetaOH);

		if (directPdfW)
			*directPdfW = threshold * specPdfD *
					(hitPoint.fromLight ? fabsf(cosThetaIH) :
							(fabsf(cosThetaOH) * eta * eta)) / lengthSquared;

		if (reversePdfW)
			*reversePdfW = threshold * specPdfR *
					(hitPoint.fromLight ? (fabsf(cosThetaOH) * eta * eta) :
							fabsf(cosThetaIH)) / lengthSquared;

		// Radiance (eye path) scaling of the delta GlassMaterial this lobe
		// stands in for: the delta glass multiplies by eta_fixed^2 per
		// crossing, so a path mixing an exact entry (mindepth) with a
		// regularized exit must see the same factor here. eta is taken on
		// the light side in Evaluate (= 1 / the Sample-side eta), and the
		// GGX BTDF below already carries 1/eta^2: together eta^2.
		// Verified in a white furnace (IOR 1.5 sphere, sigma 0.01-0.1):
		// 1.00/0.995/0.97 vs 0.54/0.97/1.47 before.
		const float radianceScale = hitPoint.fromLight ? 1.f : (eta * eta);
		const luxrays::Spectrum result = (radianceScale * fabsf(cosThetaOH) * cosThetaIH * D *
				G / (cosThetaI * lengthSquared)) *
				kt * (luxrays::Spectrum(1.f) - F);

		*event = GLOSSY | TRANSMIT;

		return result;
	} else {
		// Reflect
		const float cosThetaO = fabsf(luxrays::CosTheta(localLightDir));
		const float cosThetaI = fabsf(luxrays::CosTheta(localEyeDir));
		if (cosThetaO == 0.f || cosThetaI == 0.f)
			return luxrays::Spectrum();
		luxrays::Vector wh = localLightDir + localEyeDir;
		if (wh == luxrays::Vector(0.f))
			return luxrays::Spectrum();
		wh = luxrays::Normalize(wh);
		if (wh.z < 0.f)
			wh = -wh;

		const float cosThetaH = luxrays::Dot(localEyeDir, wh);
		const float D = GgxD(wh, alpha, alpha);
		const float G = GgxG2(localLightDir, localEyeDir, alpha, alpha);
		const float specPdfD = GgxVNDFReflectionPdf(localEyeDir, wh, alpha, alpha);
		const float specPdfR = GgxVNDFReflectionPdf(localLightDir, wh, alpha, alpha);
		const luxrays::Spectrum F = allowTransmit ?
				DispersiveFresnelR(nt, nc, disp, cosThetaH) :
				luxrays::Spectrum(1.f);

		if (directPdfW)
			*directPdfW = (1.f - threshold) * specPdfD;

		if (reversePdfW)
			*reversePdfW = (1.f - threshold) * specPdfR;

		// Regularized delta lobe is single-scatter: no multibounce
		// compensation (the lobe lives only while sigma > 0).
		const float msFactor = D * G / (4.f * cosThetaI);
		const luxrays::Spectrum result = msFactor * kr * F;

		*event = GLOSSY | REFLECT;

		return result;
	}
}

// Sample a GGX microfacet normal with isotropic alpha == the vertex's
// regularization sigma, then refract/reflect exactly like
// RoughGlassMaterial::Sample (VNDF importance sampling, threshold
// split between transmit and reflect lobes, G2/G1 weighting).
inline luxrays::Spectrum GlassMicrofacet_Sample(
		const HitPoint &hitPoint,
		const luxrays::Vector &localFixedDir,
		luxrays::Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event,
		const luxrays::Spectrum &kr,
		const luxrays::Spectrum &kt,
		const float nc, const float nt, const Dispersion &disp,
		const bool allowTransmit) {
	if (fabsf(localFixedDir.z) < DEFAULT_COS_EPSILON_STATIC)
		return luxrays::Spectrum();

	const bool isKtBlack = !allowTransmit || kt.Black();
	const bool isKrBlack = kr.Black();
	if (isKtBlack && isKrBlack)
		return luxrays::Spectrum();

	const float alpha = hitPoint.regularization;
	if (alpha <= 0.f)
		return luxrays::Spectrum();

	// Dispersion (S2): the direction-defining wavelength is the hero bin
	const float ntEff = DispersiveIOR(nt, disp);
	const float ntc = ntEff / nc;

	// Index-matched interface (nt == nc): the boundary is invisible. The
	// microfacet refraction Jacobian is singular at eta = 1 (every half
	// vector maps to -wo, pdf -> inf, NaN MIS) and killed every path
	// through it - SSS/volume containers at IOR 1 rendered black. Pass
	// straight through as a delta event (F = 0, no reflection).
	if (!isKtBlack && fabsf(ntc - 1.f) < 1e-5f) {
		*localSampledDir = -localFixedDir;
		*pdfW = 1.f;
		*event = SPECULAR | TRANSMIT;
		return kt;
	}

	luxrays::Vector wh = GgxSampleVNDF(localFixedDir, alpha, alpha, u0, u1);
	const float specPdf = GgxVNDFHalfPdf(localFixedDir, wh, alpha, alpha);
	if (specPdf <= 0.f)
		return luxrays::Spectrum();
	if (wh.z < 0.f)
		wh = -wh;
	const float cosThetaOH = luxrays::Dot(localFixedDir, wh);

	const float coso = fabsf(localFixedDir.z);

	// Decide to transmit or reflect (same .5 threshold as roughglass)
	float threshold;
	if (!isKrBlack) {
		if (!isKtBlack)
			threshold = .5f;
		else
			threshold = 0.f;
	} else {
		if (!isKtBlack)
			threshold = 1.f;
		else
			return luxrays::Spectrum();
	}

	luxrays::Spectrum result;
	if (passThroughEvent < threshold) {
		// Transmit
		const bool entering = (luxrays::CosTheta(localFixedDir) > 0.f);
		const float eta = entering ? (nc / ntEff) : ntc;
		const float eta2 = eta * eta;
		const float sinThetaIH2 = eta2 * luxrays::Max(0.f, 1.f - cosThetaOH * cosThetaOH);
		if (sinThetaIH2 >= 1.f)
			return luxrays::Spectrum();
		float cosThetaIH = sqrtf(1.f - sinThetaIH2);
		if (entering)
			cosThetaIH = -cosThetaIH;
		const float length = eta * cosThetaOH + cosThetaIH;
		*localSampledDir = length * wh - eta * localFixedDir;

		const float lengthSquared = length * length;
		*pdfW = specPdf * fabsf(cosThetaIH) / lengthSquared;
		if (*pdfW <= 0.f)
			return luxrays::Spectrum();

		// (f*cos)/pdf = kt*(1-F)*G2/G1(wo) with VNDF sampling
		const float g1 = GgxG1(localFixedDir, alpha, alpha);
		if (g1 <= 0.f)
			return luxrays::Spectrum();
		const float g2 = GgxG2(*localSampledDir, localFixedDir, alpha, alpha);
		const luxrays::Spectrum F = DispersiveFresnelR(nt, nc, disp,
				hitPoint.fromLight ? cosThetaOH : cosThetaIH);
		result = kt * (luxrays::Spectrum(1.f) - F) * (g2 / (g1 * threshold));
		// Radiance scaling of the delta glass (see Evaluate)
		if (!hitPoint.fromLight)
			result *= eta2;

		*pdfW *= threshold;
		*event = GLOSSY | TRANSMIT;
		// Dispersive refraction terminates the secondary wavelengths
		if (disp.Active())
			result *= luxrays::Spectral::CollapseToHero();
	} else {
		// Reflect
		*pdfW = specPdf / (4.f * fabsf(cosThetaOH));
		if (*pdfW <= 0.f)
			return luxrays::Spectrum();

		*localSampledDir = 2.f * cosThetaOH * wh - localFixedDir;

		const float cosi = fabsf(localSampledDir->z);
		if ((cosi < DEFAULT_COS_EPSILON_STATIC) ||
				(localFixedDir.z * localSampledDir->z < 0.f))
			return luxrays::Spectrum();

		// Fresnel: dielectric materials (allowTransmit) use the
		// dispersive Cauchy reflectance; conductors (mirror) fold the
		// reflectance into kr, so F stays 1 (matches the delta mirror
		// contract where Sample returns kr un-attenuated).
		const luxrays::Spectrum F = allowTransmit ?
				DispersiveFresnelR(nt, nc, disp, cosThetaOH) :
				luxrays::Spectrum(1.f);
		const float g1 = GgxG1(localFixedDir, alpha, alpha);
		if (g1 <= 0.f)
			return luxrays::Spectrum();
		result = kr * F *
				(GgxG2(*localSampledDir, localFixedDir, alpha, alpha) /
				(g1 * (1.f - threshold)));

		*pdfW *= (1.f - threshold);
		*event = GLOSSY | REFLECT;
	}

	return result;
}

// Pdf: forward/reverse transport pdf of the regularized lobe at the
// given directions (half-vector transform of
// RoughGlassMaterial::Pdf, isotropic alpha = reg).
inline void GlassMicrofacet_Pdf(
		const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir,
		const luxrays::Vector &localEyeDir,
		float *directPdfW, float *reversePdfW,
		const luxrays::Spectrum &kr,
		const luxrays::Spectrum &kt,
		const float nc, const float nt, const Dispersion &disp,
		const bool allowTransmit) {
	if (directPdfW)
		*directPdfW = 0.f;
	if (reversePdfW)
		*reversePdfW = 0.f;

	const bool isKtBlack = !allowTransmit || kt.Black();
	const bool isKrBlack = kr.Black();
	if (isKtBlack && isKrBlack)
		return;

	const float alpha = hitPoint.regularization;
	if (alpha <= 0.f)
		return;

	const float ntEff = DispersiveIOR(nt, disp);
	const float ntc = ntEff / nc;

	const float threshold = isKrBlack ? 1.f : (isKtBlack ? 0.f : .5f);
	if (localLightDir.z * localEyeDir.z < 0.f) {
		// Transmit
		const bool entering = (luxrays::CosTheta(localLightDir) > 0.f);
		const float eta = entering ? (nc / ntEff) : ntc;

		luxrays::Vector wh = eta * localLightDir + localEyeDir;
		if (wh.z < 0.f)
			wh = -wh;

		const float lengthSquared = wh.LengthSquared();
		if (!(lengthSquared > 0.f))
			return;

		wh /= sqrtf(lengthSquared);
		const float cosThetaIH = luxrays::AbsDot(localEyeDir, wh);
		const float cosThetaOH = luxrays::AbsDot(localLightDir, wh);

		const float specPdfD = GgxVNDFHalfPdf(localEyeDir, wh, alpha, alpha);
		const float specPdfR = GgxVNDFHalfPdf(localLightDir, wh, alpha, alpha);

		if (directPdfW)
			*directPdfW = threshold * specPdfD * cosThetaIH / lengthSquared;

		if (reversePdfW)
			*reversePdfW = threshold * specPdfR * cosThetaOH * eta * eta / lengthSquared;
	} else {
		// Reflect
		const float cosThetaO = fabsf(luxrays::CosTheta(localLightDir));
		const float cosThetaI = fabsf(luxrays::CosTheta(localEyeDir));
		if (cosThetaO == 0.f || cosThetaI == 0.f)
			return;

		luxrays::Vector wh = localLightDir + localEyeDir;
		if (wh == luxrays::Vector(0.f))
			return;
		wh = luxrays::Normalize(wh);
		if (wh.z < 0.f)
			wh = -wh;

		const float specPdfD = GgxVNDFReflectionPdf(localEyeDir, wh, alpha, alpha);
		const float specPdfR = GgxVNDFReflectionPdf(localLightDir, wh, alpha, alpha);

		if (directPdfW)
			*directPdfW = (1.f - threshold) * specPdfD;

		if (reversePdfW)
			*reversePdfW = (1.f - threshold) * specPdfR;
	}
}

} // namespace slg
