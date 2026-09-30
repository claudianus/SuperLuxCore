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

#include <cstddef>

#include "slg/volumes/homogenous.h"
#include "luxrays/usings.h"
#include "slg/bsdf/bsdf.h"
#include "slg/textures/texture.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// HomogeneousVolume
//------------------------------------------------------------------------------

HomogeneousVolume::HomogeneousVolume(
	TextureConstRef iorTex,
	TextureConstPtr emiTex,
	TextureConstRef a, TextureConstRef s, TextureConstRef g,
	const bool multiScat, const bool useHG, const bool equiang,
	TextureConstPtr sssAlbedo, TextureConstPtr sssMfp,
	const int sssProfile
) :
	Volume(iorTex, emiTex),
	schlickScatter(*this, g, useHG),
	multiScattering(multiScat),
	equiangular(equiang),
	sigmaA(a),
	sigmaS(s),
	sssAlbedoTex(sssAlbedo),
	sssMfpTex(sssMfp),
	sssProfile(sssProfile)
{}

// SSS albedo parametrization: maps the diffuse surface albedo A to the
// physical single-scatter albedo alpha of the medium. Closed-form
// inversion of d'Eon, "A Hitchhiker's Guide to Multiple Scattering"
// v0.3.2, Eq. 53.7 (Cycles' "van de Hulst" random-walk SSS remap), which
// folds the phase anisotropy g into alpha. Calibrated against exact
// Monte-Carlo transport at a refractive-index-matched boundary.
static float SSSAlphaVanDeHulst(const float A, const float g) {
	const float x = 4.20863f * A + 4.09712f -
			sqrtf(9.59217f + 41.6808f * A + 17.7126f * A * A);
	const float s2 = x * x;
	return Clamp((1.f - s2) / (1.f - g * s2), 0.f, 0.999999f);
}

// van de Hulst remap: d is the extinction mean free path (1/sigma_t).
static void SSSRemapVanDeHulst(const float A, const float d, const float g,
		float &sigmaT, float &alpha) {
	alpha = SSSAlphaVanDeHulst(A, g);
	sigmaT = 1.f / Max(d, 1e-6f);
}

// Jensen-Buhler/Christensen-Burley diffusion-theory diffuse reflectance
// of a semi-infinite medium at interior IOR eta (Groenhuis-Ferry
// boundary reflectance fit). NOTE: verified against the engine's own
// random walk (dev-tools/e48) that this expression overshoots the true
// transport reflectance by ~10% at matched boundaries, so it is only
// used as the ratio K(a,eta) = Rd(a,eta)/Rd(a,1), which captures the
// physically correct Fresnel-trapping correction direction/magnitude.
static float SSSRdDiffusion(const float a, const float eta) {
	const float e = Max(eta, 1.0001f);
	const float rhoE = -1.4399f / (e * e) + .7099f / e + .6681f + .0636f * e;
	const float Ab = (1.f + rhoE) / Max(1.f - rhoE, 1e-4f);
	return .5f * a * (1.f + expf(-4.f / 3.f * Ab * sqrtf(3.f * (1.f - a))));
}

// "cb15" SSS parametrization: requested sssalbedo A is the *measured*
// diffuse reflectance including the interior dielectric boundary, i.e.
//   A = R_match(a) * K(a, eta)
// solved by fixed-point iteration a <- vdHinv(A / K(a,eta)). For
// eta <= 1 it collapses exactly to the van de Hulst remap. For eta > 1
// TIR trapping lowers external reflectance (K < 1) and the solve raises
// the physical single-scatter albedo to compensate, so the same
// sssalbedo yields the same apparent reflectance regardless of volume
// IOR. d reads as the transport mfp (1/sigma'_t), the Principled-radius
// convention.
static void SSSRemapCB15(const float A, const float d, const float g,
		const float eta, float &sigmaT, float &alpha) {
	const float Ad = Clamp(A, 1e-4f, 0.999f);
	const float e = Max(eta, 1.0001f);
	float al = SSSAlphaVanDeHulst(Ad, g);
	for (int i = 0; i < 6; ++i) {
		const float k = Clamp(SSSRdDiffusion(al, e) /
				Max(SSSRdDiffusion(al, 1.f), 1e-4f), 1e-2f, 1.f);
		al = SSSAlphaVanDeHulst(Clamp(A / k, 1e-4f, 0.999f), g);
	}
	alpha = al;
	sigmaT = (1.f / Max(d, 1e-6f)) / Max(1.f - g * alpha, 1e-6f);
}

Spectrum HomogeneousVolume::SSSCoeffs(const HitPoint &hitPoint,
		Spectrum &alpha) const {
	const Spectrum A = sssAlbedoTex->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	const Spectrum mfp = sssMfpTex->GetSpectrumValue(hitPoint).Clamp();
	const Spectrum g = GetG().GetSpectrumValue(hitPoint).Clamp(-0.99f, 0.99f);
	const float eta = GetIOR(hitPoint);
	Spectrum sigmaT;
	for (u_int i = 0; i < COLOR_SAMPLES; ++i) {
		if (sssProfile == 1)
			SSSRemapCB15(A.c[i], mfp.c[i], g.c[i], eta, sigmaT.c[i], alpha.c[i]);
		else
			SSSRemapVanDeHulst(A.c[i], mfp.c[i], g.c[i], sigmaT.c[i], alpha.c[i]);
	}
	return sigmaT;
}

float HomogeneousVolume::Scatter(const float u,
		const bool scatterAllowed, const float segmentLength,
		const Spectrum &sigmaA, const Spectrum &sigmaS, const Spectrum &emission,
		Spectrum &segmentTransmittance, Spectrum &segmentEmission) {
	// This code must work also with segmentLength = INFINITY

	bool scatter = false;
	segmentTransmittance = Spectrum(1.f);
	segmentEmission = Spectrum(0.f);

	//--------------------------------------------------------------------------
	// Check if there is a scattering event
	//--------------------------------------------------------------------------

	float scatterDistance = segmentLength;
	const float sigmaSValue = sigmaS.Filter();
	if (scatterAllowed && (sigmaSValue > 0.f)) {
		// Determine scattering distance
		const float proposedScatterDistance = -logf(1.f - u) / sigmaSValue;

		scatter = (proposedScatterDistance < segmentLength);
		scatterDistance = scatter ? proposedScatterDistance : segmentLength;

		// Note: scatterDistance can not be infinity because otherwise there would
		// have been a scatter event before.
		const float tau = scatterDistance * sigmaSValue;
		const float pdf = expf(-tau) * (scatter ? sigmaSValue : 1.f);
		segmentTransmittance /= pdf;
	}

	//--------------------------------------------------------------------------
	// Volume transmittance
	//--------------------------------------------------------------------------
	
	const Spectrum sigmaT = sigmaA + sigmaS;
	if (!sigmaT.Black()) {
		if (isinf(scatterDistance)) {
			// This avoid NaN in case scatterDistance is inf
			segmentTransmittance = Spectrum(0.f);
		} else {
			const Spectrum tau = scatterDistance * sigmaT;
			segmentTransmittance *= Exp(-tau) * (scatter ? sigmaT : Spectrum(1.f));
		}
	}

	//--------------------------------------------------------------------------
	// Volume emission
	//--------------------------------------------------------------------------

	segmentEmission += segmentTransmittance * scatterDistance * emission;

	return scatter ? scatterDistance : -1.f;
}

Spectrum HomogeneousVolume::SigmaA(const HitPoint &hitPoint) const {
	if (sssAlbedoTex) {
		Spectrum alpha;
		const Spectrum sigmaT = SSSCoeffs(hitPoint, alpha);
		return (Spectrum(1.f) - alpha) * sigmaT;
	}
	return GetSigmaA().GetSpectrumValue(hitPoint).Clamp();
}

Spectrum HomogeneousVolume::SigmaS(const HitPoint &hitPoint) const {
	if (sssAlbedoTex) {
		Spectrum alpha;
		const Spectrum sigmaT = SSSCoeffs(hitPoint, alpha);
		return alpha * sigmaT;
	}
	return GetSigmaS().GetSpectrumValue(hitPoint).Clamp();
}

float HomogeneousVolume::Scatter(const Ray &ray, const float u,
		const bool scatteredStart, Spectrum *connectionThroughput,
		Spectrum *connectionEmission) const {
	const float segmentLength = ray.maxt - ray.mint;

	// Check if I have to support multi-scattering
	const bool scatterAllowed = (!scatteredStart || multiScattering);

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	const Spectrum sigmaA = SigmaA(hitPoint);
	const Spectrum sigmaS = SigmaS(hitPoint);
	const Spectrum emission = Emission(hitPoint);

	Spectrum segmentTransmittance, segmentEmission;
	const float scatterDistance = HomogeneousVolume::Scatter(u, scatterAllowed,
			segmentLength, sigmaA, sigmaS, emission,
			segmentTransmittance, segmentEmission);

	// I need to update first connectionEmission and than connectionThroughput
	*connectionEmission += *connectionThroughput * emission;
	*connectionThroughput *= segmentTransmittance;

	return (scatterDistance == -1.f) ? -1.f : (ray.mint + scatterDistance);
}

float HomogeneousVolume::ScatterEquiangular(const Ray &ray, const float u,
		const bool scatteredStart, const std::vector<Point> &eqLightPoints,
		const std::vector<float> &eqLightLuminances,
		Spectrum *connectionThroughput,
		Spectrum *connectionEmission) const {
	const float segmentLength = ray.maxt - ray.mint;
	const bool scatterAllowed = (!scatteredStart || multiScattering);

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	const Spectrum sigmaA = SigmaA(hitPoint);
	const Spectrum sigmaS = SigmaS(hitPoint);
	const Spectrum emission = Emission(hitPoint);
	const float sigmaSValue = sigmaS.Filter();
	const Spectrum sigmaT = sigmaA + sigmaS;

	// Equiangular geometry requires a finite segment and at least one
	// eligible light
	const u_int lightCount = (u_int)eqLightPoints.size();
	if (!scatterAllowed || (sigmaSValue <= 0.f) || !isfinite(segmentLength) ||
			(segmentLength <= 0.f) || (lightCount == 0u))
		return Scatter(ray, u, scatteredStart, connectionThroughput, connectionEmission);

	TauswortheRandomGenerator rng(u);

	// Contribution-aware light selection: the glow mass of light i along
	// the segment is ~ lum_i * int 1/(D_i^2 + x^2) dx =
	// lum_i * (thetaB_i - thetaA_i) / D_i. Sampling proportionally to it
	// concentrates equiangular vertices where the illumination gradient is
	// steepest; the selection probability cancels in the conditioned MIS,
	// so any weight keeps the estimator unbiased.
	const float u1 = rng.floatValue();
	const auto weightOf = [&](const u_int i) -> float {
		const Vector toL(eqLightPoints[i] - ray.o);
		const float dlt = Dot(toL, ray.d);
		const float D = sqrtf(Max(1e-10f, toL.LengthSquared() - dlt * dlt));
		const float thA = atan2f(ray.mint - dlt, D);
		const float thB = atan2f(ray.maxt - dlt, D);
		return eqLightLuminances[i] * (thB - thA) / D;
	};
	// Single-pass evaluation: the light count is typically small and
	// weightOf(i) is pure - reusing the values saves a second sqrt +
	// 2*atan2 per light (bit-identical, they were evaluated twice).
	float weights[64];
	u_int pick = lightCount - 1u;
	if (lightCount <= 64u) {
		float weightsSum = 0.f;
		for (u_int i = 0; i < lightCount; ++i) {
			weights[i] = weightOf(i);
			weightsSum += weights[i];
		}
		if (weightsSum > 0.f) {
			float acc = 0.f;
			for (u_int i = 0; i < lightCount; ++i) {
				acc += weights[i];
				if (acc >= u1 * weightsSum) {
					pick = i;
					break;
				}
			}
		} else
			pick = Min((u_int)(u1 * lightCount), lightCount - 1u);
	} else {
		float weightsSum = 0.f;
		for (u_int i = 0; i < lightCount; ++i)
			weightsSum += weightOf(i);
		if (weightsSum > 0.f) {
			float acc = 0.f;
			for (u_int i = 0; i < lightCount; ++i) {
				acc += weightOf(i);
				if (acc >= u1 * weightsSum) {
					pick = i;
					break;
				}
			}
		} else
			pick = Min((u_int)(u1 * lightCount), lightCount - 1u);
	}
	const Point &eqLightPos = eqLightPoints[pick];

	// delta: projection of the light on the ray (absolute t domain);
	// D: perpendicular distance of the light from the ray
	const Vector toLight(eqLightPos - ray.o);
	const float delta = Dot(toLight, ray.d);
	const float D = sqrtf(Max(1e-10f, toLight.LengthSquared() - delta * delta));
	const float thetaA = atan2f(ray.mint - delta, D);
	const float thetaB = atan2f(ray.maxt - delta, D);
	const float invThetaRange = 1.f / (thetaB - thetaA);

	// One-sample MIS between the equiangular (E) and transmittance (T)
	// strategies, conditioned on the already-picked light: the light
	// selection probability is shared by both strategies and cancels in
	// the balance heuristic, so it must not appear in the pdfs.
	float dist, pdf;
	bool collision;
	if (rng.floatValue() < .5f) {
		// Equiangular strategy: always produces a vertex in [mint, maxt]
		const float tAbs = delta + D * tanf(thetaA +
				(thetaB - thetaA) * rng.floatValue());
		// Guard against fp error pushing the vertex outside the segment
		dist = Clamp(tAbs - ray.mint, 0.f, segmentLength);
		const float pdfT = sigmaSValue * expf(-sigmaSValue * dist);
		const float pdfE = D * invThetaRange /
				(D * D + (tAbs - delta) * (tAbs - delta));
		pdf = .5f * (pdfT + pdfE);
		collision = true;
	} else {
		// Transmittance strategy (same sampler as Scatter())
		dist = -logf(1.f - rng.floatValue()) / sigmaSValue;
		collision = (dist < segmentLength);
		if (collision) {
			const float pdfT = sigmaSValue * expf(-sigmaSValue * dist);
			const float tAbs = ray.mint + dist;
			const float pdfE = D * invThetaRange /
					(D * D + (tAbs - delta) * (tAbs - delta));
			pdf = .5f * (pdfT + pdfE);
		} else {
			// The pass-through event belongs to the T strategy only:
			// the mixture probability is .5 * exp(-sigmaS * L)
			dist = segmentLength;
			pdf = .5f * expf(-sigmaSValue * segmentLength);
		}
	}

	// Same weighting convention as Scatter(): sigma_t * T(t) / pdf at a
	// collision, T(L) / pdf on pass-through
	Spectrum segmentTransmittance = Spectrum(1.f) / pdf;
	if (collision) {
		const Spectrum tau = dist * sigmaT;
		segmentTransmittance *= Exp(-tau) * sigmaT;
	} else {
		const Spectrum tau = segmentLength * sigmaT;
		segmentTransmittance *= Exp(-tau);
	}

	// I need to update first connectionEmission and than connectionThroughput
	*connectionEmission += *connectionThroughput * emission;
	*connectionThroughput *= segmentTransmittance;

	return collision ? (ray.mint + dist) : -1.f;
}

Spectrum HomogeneousVolume::TransmittanceEstimate(const Ray &ray, const float u) const {
	const float segmentLength = ray.maxt - ray.mint;

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	const Spectrum sigmaT = SigmaT(hitPoint);
	if (sigmaT.Black() || (segmentLength <= 0.f))
		return Spectrum(1.f);

	const Spectrum tau = (segmentLength * sigmaT).Clamp();
	return Exp(-tau);
}

Spectrum HomogeneousVolume::Albedo(const HitPoint &hitPoint) const {
	return schlickScatter.Albedo(hitPoint);
}

Spectrum HomogeneousVolume::Evaluate(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
		float *directPdfW, float *reversePdfW) const {
	return schlickScatter.Evaluate(hitPoint, localLightDir, localEyeDir, event, directPdfW, reversePdfW);
}

Spectrum HomogeneousVolume::Sample(const HitPoint &hitPoint,
		const Vector &localFixedDir, Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const {
	return schlickScatter.Sample(hitPoint, localFixedDir, localSampledDir,
			u0, u1, passThroughEvent, pdfW, event);
}

void HomogeneousVolume::Pdf(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
	schlickScatter.Pdf(hitPoint, localLightDir, localEyeDir, directPdfW, reversePdfW);
}

void HomogeneousVolume::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Volume::AddReferencedTextures(referencedTexs);

	GetSigmaA().AddReferencedTextures(referencedTexs);
	GetSigmaS().AddReferencedTextures(referencedTexs);
	schlickScatter.GetG().AddReferencedTextures(referencedTexs);
	if (sssAlbedoTex)
		sssAlbedoTex->AddReferencedTextures(referencedTexs);
	if (sssMfpTex)
		sssMfpTex->AddReferencedTextures(referencedTexs);
}

void HomogeneousVolume::UpdateTextureReferences(
	TextureConstRef oldTex, TextureRef newTex
) {
	Volume::UpdateTextureReferences(oldTex, newTex);

	updtex(sigmaA, oldTex, newTex);
	updtex(sigmaS, oldTex, newTex);
	if (&schlickScatter.GetG() == &oldTex)
		schlickScatter.SetG(newTex);
	if (sssAlbedoTex == std::addressof(oldTex))
		sssAlbedoTex = std::addressof(newTex);
	if (sssMfpTex == std::addressof(oldTex))
		sssMfpTex = std::addressof(newTex);
}

PropertiesUPtr HomogeneousVolume::ToProperties() const {
	PropertiesUPtr props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.volumes." + name + ".type")("homogeneous"));
	props->Set(Property("scene.volumes." + name + ".absorption")(GetSigmaA().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".scattering")(GetSigmaS().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".asymmetry")(schlickScatter.GetG().GetSDLValue()));
	if (sssAlbedoTex) {
		props->Set(Property("scene.volumes." + name + ".sssalbedo")(sssAlbedoTex->GetSDLValue()));
		props->Set(Property("scene.volumes." + name + ".sssmfp")(sssMfpTex->GetSDLValue()));
		props->Set(Property("scene.volumes." + name + ".sssprofile")(sssProfile == 1 ? "cb15" : "vandehulst"));
	}
	props->Set(Property("scene.volumes." + name + ".multiscattering")(multiScattering));
	props->Set(Property("scene.volumes." + name + ".phase")(schlickScatter.IsHGPhase() ? "hg" : "schlick"));
	props->Set(Property("scene.volumes." + name + ".distancesampling")(equiangular ? "equiangular" : "transmittance"));
	props->Set(Volume::ToProperties());

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
