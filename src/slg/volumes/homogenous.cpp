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
#include "slg/textures/math/scale.h"
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
{
	// Constant-texture fast path for Scatter()/ScatterEquiangular(): in
	// non-spectral renders the sigma/emission textures of a homogeneous
	// volume are almost always ConstFloat3, so evaluating them at each
	// volume event is pure overhead (virtual dispatch + a fully built
	// HitPoint). Under spectral rendering the value depends on the path
	// wavelengths, so the cache is gated at use sites on !Spectral::Current().
	HitPoint hp;
	hp.Init();
	const bool sigmaAConst = (sigmaA.get().GetType() == CONST_FLOAT3) ||
			(sigmaA.get().GetType() == CONST_FLOAT);
	const bool sigmaSConst = (sigmaS.get().GetType() == CONST_FLOAT3) ||
			(sigmaS.get().GetType() == CONST_FLOAT);
	const bool emissionConst = !volumeEmissionTex ||
			(volumeEmissionTex->GetType() == CONST_FLOAT3) ||
			(volumeEmissionTex->GetType() == CONST_FLOAT);
	constSigmaParams = !sssAlbedoTex && sigmaAConst && sigmaSConst && emissionConst;
	if (constSigmaParams) {
		constSigmaA = sigmaA.get().EvalSpectrumValue(hp).Clamp();
		constSigmaS = sigmaS.get().EvalSpectrumValue(hp).Clamp();
		constEmission = volumeEmissionTex ?
				volumeEmissionTex->EvalSpectrumValue(hp).Clamp() : Spectrum(0.f);
	}
}

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
// solved for A' = R_match(a) by bisection, a = vdHinv(A'). For
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
	// Bisection on the matched-boundary albedo A': A'*K(vdH(A'),eta) is
	// monotonic in A'. The former fixed-point a <- vdH(A/K(a)) oscillated
	// at high albedo (K -> 1 as a -> 1: A=.8, eta=1.5 flips between
	// a=.9909 and a=.99995) and stopped on the under-compensated branch,
	// losing ~20% of the requested reflectance.
	float lo = 1e-4f, hi = .999f;
	for (int i = 0; i < 16; ++i) {
		const float m = .5f * (lo + hi);
		const float al = SSSAlphaVanDeHulst(m, g);
		const float k = Clamp(SSSRdDiffusion(al, e) /
				Max(SSSRdDiffusion(al, 1.f), 1e-4f), 1e-2f, 1.f);
		if (m * k < Ad)
			lo = m;
		else
			hi = m;
	}
	const float al = SSSAlphaVanDeHulst(.5f * (lo + hi), g);
	alpha = al;
	sigmaT = (1.f / Max(d, 1e-6f)) / Max(1.f - g * alpha, 1e-6f);
}

namespace {
thread_local HomogeneousVolume::InteriorScope *sssInteriorScope = nullptr;

bool IsSpatiallyConstantSSS(const Texture &texture, const unsigned depth = 0) {
	if (texture.GetType() == CONST_FLOAT || texture.GetType() == CONST_FLOAT3) return true;
	if (depth >= 8 || texture.GetType() != SCALE_TEX) return false;
	const auto &scale = static_cast<const ScaleTexture &>(texture);
	return IsSpatiallyConstantSSS(scale.GetTexture1(), depth + 1) &&
			IsSpatiallyConstantSSS(scale.GetTexture2(), depth + 1);
}
}

HomogeneousVolume::InteriorScope::InteriorScope(const HomogeneousVolume &owner, const HitPoint &point) :
		previous(sssInteriorScope) {
	static const bool disabled = getenv("SUPERLUXCORE_SSS_NO_COEFFICIENT_SCOPE") != nullptr;
	if (!disabled && owner.IsSSSParametrized() && IsSpatiallyConstantSSS(*owner.GetSSSAlbedoTexture()) &&
			IsSpatiallyConstantSSS(*owner.GetSSSMfpTexture()) && IsSpatiallyConstantSSS(owner.GetG()) &&
			IsSpatiallyConstantSSS(owner.GetIORTexture())) {
		sigmaT = owner.SSSCoeffs(point, alpha);
		const auto *sw = Spectral::Current();
		spectral = sw != nullptr;
		if (sw) {
			for (unsigned i = 0; i < 3; ++i) wavelengths[i] = sw->w[i];
			hero = sw->hero; aliveMask = sw->aliveMask;
			upsampling = Spectral::GetUpsamplingModel();
		}
		volume = &owner;
	}
	sssInteriorScope = this;
}

HomogeneousVolume::InteriorScope::~InteriorScope() { sssInteriorScope = previous; }

bool HomogeneousVolume::InteriorScope::Matches() const {
	const auto *sw = Spectral::Current();
	if (spectral != (sw != nullptr)) return false;
	if (!sw) return true;
	return hero == sw->hero && aliveMask == sw->aliveMask &&
			upsampling == Spectral::GetUpsamplingModel() &&
			wavelengths[0] == sw->w[0] && wavelengths[1] == sw->w[1] && wavelengths[2] == sw->w[2];
}

Spectrum HomogeneousVolume::SSSCoeffs(const HitPoint &hitPoint,
		Spectrum &alpha) const {
	if (sssInteriorScope && sssInteriorScope->volume == this && sssInteriorScope->Matches()) {
		alpha = sssInteriorScope->alpha;
		return sssInteriorScope->sigmaT;
	}
	const Spectrum A = sssAlbedoTex->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	const Spectrum mfp = sssMfpTex->GetSpectrumValue(hitPoint).Clamp();
	const Spectrum g = GetG().GetSpectrumValue(hitPoint).Clamp(-0.99f, 0.99f);
	const float eta = GetIOR(hitPoint);
	// A dense walk repeatedly solves the same 16-step CB15 inversion,
	// including separate SigmaA/SigmaS/phase evaluations at each vertex.
	// Keep one exact last-input result per CPU thread. Evaluate textures
	// first: spatial changes, edits and the current spectral wavelengths
	// are already reflected in A/mfp/g/eta. No pointer identity, quantized
	// key, interpolation, shared mutable state or growing allocation.
	struct Coefficients {
		bool valid = false;
		Spectrum albedo, mfp, g, sigmaT, alpha;
		float eta = 0.f;
		int profile = 0;
	};
	static thread_local Coefficients last;
	if (last.valid && last.albedo == A && last.mfp == mfp &&
			last.g == g && last.eta == eta && last.profile == sssProfile) {
		alpha = last.alpha;
		return last.sigmaT;
	}
	Spectrum sigmaT;
	for (u_int i = 0; i < COLOR_SAMPLES; ++i) {
		if (sssProfile == 1)
			SSSRemapCB15(A.c[i], mfp.c[i], g.c[i], eta, sigmaT.c[i], alpha.c[i]);
		else
			SSSRemapVanDeHulst(A.c[i], mfp.c[i], g.c[i], sigmaT.c[i], alpha.c[i]);
	}
	last.albedo = A;
	last.mfp = mfp;
	last.g = g;
	last.eta = eta;
	last.profile = sssProfile;
	last.sigmaT = sigmaT;
	last.alpha = alpha;
	last.valid = true;
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

float HomogeneousVolume::ScatterChromatic(const float u,
		const bool scatterAllowed, const float segmentLength,
		const Spectrum &sigmaA, const Spectrum &sigmaS, const Spectrum &emission,
		const Spectrum &channelWeights,
		Spectrum &segmentTransmittance, Spectrum &segmentEmission) {
	// This code must work also with segmentLength = INFINITY

	bool scatter = false;
	segmentTransmittance = Spectrum(1.f);
	segmentEmission = Spectrum(0.f);

	float scatterDistance = segmentLength;
	if (scatterAllowed && (sigmaS.Max() > 0.f)) {
		// Channel selection probabilities ~ path throughput
		float w[COLOR_SAMPLES];
		float wSum = 0.f;
		for (u_int i = 0; i < COLOR_SAMPLES; ++i) {
			const float c = channelWeights.c[i];
			w[i] = (isfinite(c) && (c > 0.f)) ? c : 0.f;
			wSum += w[i];
		}
		if (!(wSum > 0.f)) {
			for (u_int i = 0; i < COLOR_SAMPLES; ++i)
				w[i] = 1.f;
			wSum = COLOR_SAMPLES;
		}

		// Pick the channel with u and reuse the remainder for the distance
		float uc = u * wSum;
		u_int ch = COLOR_SAMPLES - 1;
		for (u_int i = 0; i < COLOR_SAMPLES; ++i) {
			if ((uc < w[i]) && (w[i] > 0.f)) {
				ch = i;
				break;
			}
			uc -= w[i];
		}
		while ((w[ch] <= 0.f) && (ch > 0))
			--ch;
		const float u2 = Clamp(uc / w[ch], 0.f, 0.99999994f);

		const float sCh = sigmaS.c[ch];
		const float proposedScatterDistance = (sCh > 0.f) ? (-logf(1.f - u2) / sCh) : INFINITY;
		scatter = (proposedScatterDistance < segmentLength);
		scatterDistance = scatter ? proposedScatterDistance : segmentLength;

		// One-sample MIS, balance heuristic: the mixture pdf of the
		// scatter distance (or of flying past the segment end)
		float pdf = 0.f;
		for (u_int i = 0; i < COLOR_SAMPLES; ++i) {
			if (w[i] <= 0.f)
				continue;
			const float tr = isinf(scatterDistance) ? 0.f : expf(-sigmaS.c[i] * scatterDistance);
			pdf += (w[i] / wSum) * (scatter ? sigmaS.c[i] * tr : tr);
		}
		if (!(pdf > 0.f)) {
			segmentTransmittance = Spectrum(0.f);
			return -1.f;
		}
		segmentTransmittance /= pdf;
	}

	const Spectrum sigmaT = sigmaA + sigmaS;
	if (!sigmaT.Black()) {
		if (isinf(scatterDistance))
			segmentTransmittance = Spectrum(0.f);
		else {
			const Spectrum tau = scatterDistance * sigmaT;
			segmentTransmittance *= Exp(-tau) * (scatter ? sigmaT : Spectrum(1.f));
		}
	}

	segmentEmission += segmentTransmittance * scatterDistance * emission;

	return scatter ? scatterDistance : -1.f;
}

void HomogeneousVolume::SSSScatteringCoefficients(const HitPoint &hitPoint,
		Spectrum &absorption, Spectrum &scattering) const {
	Spectrum alpha;
	const Spectrum sigmaT = SSSCoeffs(hitPoint, alpha);
	absorption = (Spectrum(1.f) - alpha) * sigmaT;
	scattering = alpha * sigmaT;
}

bool HomogeneousVolume::DirectionProposal(const HitPoint &hitPoint,
		const Spectrum &throughput, const Vector &axis, const Point &proposalEntry, const float thickness,
		SSSDirectionProposal &proposal) const {
	if (!IsSSSParametrized() || HasVolumeEmissionTexture() || !(thickness > 0.f)) return false;
	const Spectrum phaseG = GetG().GetSpectrumValue(hitPoint);
	for (unsigned i = 0; i < 3; ++i)
		if (!isfinite(phaseG.c[i])) return false;
	const float anisotropy = Max(fabsf(phaseG.c[0]), Max(fabsf(phaseG.c[1]), fabsf(phaseG.c[2])));
	if (anisotropy > 1e-4f) return false;
	const float axisLength = Dot(axis, axis);
	if (!(axisLength > 0.f) || !isfinite(axisLength)) return false;
	proposal.axis = axis / sqrtf(axisLength);
	Spectrum alpha;
	const Spectrum sigmaT = SSSCoeffs(hitPoint, alpha);
	// A textured flight may land in a purely absorbing region. Its phase
	// sampler returns black without a world direction; keep that absorption
	// event on the original path instead of evaluating an unset direction.
	if (alpha.Black()) return false;
	// The isotropic diffusion guide is not a suitable proposal for strongly
	// anisotropic walks; retain their verified HG/Schlick sampler.
	static const bool legacyGuide = getenv("SUPERLUXCORE_SSS_LEGACY_GUIDE") != nullptr;
	static const bool sharedGuide = getenv("SUPERLUXCORE_SSS_GUIDE_SHARED") != nullptr;
	static const bool thickOnly = getenv("SUPERLUXCORE_SSS_GUIDE_THICK_ONLY") != nullptr;
	static const float classicShare = []() {
		const char *value = getenv("SUPERLUXCORE_SSS_DWIVEDI_CLASSIC");
		return value ? Clamp(float(atof(value)), .05f, .95f) : .1f;
	}();
	static const float forwardShare = []() {
		const char *value = getenv("SUPERLUXCORE_SSS_DWIVEDI_FORWARD");
		return value ? Clamp(float(atof(value)), .01f, .99f) : .99f;
	}();
	proposal.classic = classicShare;
	proposal.forward = forwardShare;
	struct GuideCoefficients {
		bool valid = false;
		Spectrum alpha;
		float v[3], phaseLog[3];
	};
	static thread_local GuideCoefficients lastGuide;
	if (!lastGuide.valid || lastGuide.alpha != alpha) {
		for (unsigned i = 0; i < 3; ++i) {
			const float a = Clamp((!legacyGuide && sharedGuide) ? alpha.Max() : alpha.c[i], 1e-4f, .999999f);
			lastGuide.v[i] = Max(1.00001f, 1.f / sqrtf(Max(1e-7f, 1.f - powf(a,
						2.44294f - .0215813f * a + .578637f / a))));
			lastGuide.phaseLog[i] = logf((lastGuide.v[i] + 1.f) / (lastGuide.v[i] - 1.f));
		}
		lastGuide.alpha = alpha;
		lastGuide.valid = true;
	}
	float sum = 0.f;
	for (unsigned i = 0; i < 3; ++i) {
		if (!(sigmaT.c[i] > 0.f) || !isfinite(sigmaT.c[i])) return false;
		proposal.sigmaT[i] = sigmaT.c[i];
		proposal.channel[i] = isfinite(throughput.c[i]) ?
				Max(throughput.c[i] * (legacyGuide ? 1.f : alpha.c[i]), 1e-10f) : 1e-10f;
		sum += proposal.channel[i];
		// The fit controls only the proposal, never physical coefficients.
		proposal.v[i] = lastGuide.v[i];
		proposal.phaseLog[i] = lastGuide.phaseLog[i];
		// An entry-directed half-space proposal is counterproductive
		// for thin transmission. Keep its original chromatic walk where
		// the opposite interface has appreciable diffusion importance.
		if ((legacyGuide || thickOnly) && thickness * sigmaT.c[i] / proposal.v[i] < 6.f) return false;
	}
	for (unsigned i = 0; i < 3; ++i) proposal.channel[i] /= sum;
	if (!legacyGuide) {
		// Proposal only: prefer the nearby tangent boundary and smoothly
		// reverse toward the opposite side of thin/curved domains.
		const float depth = Clamp(-Dot(hitPoint.p - proposalEntry, proposal.axis), 0.f, thickness);
		const float optical = (2.f * depth - thickness) * sigmaT.Min() / proposal.v[0];
		proposal.forward = Clamp(1.f / (1.f + expf(Clamp(optical, -20.f, 20.f))), .01f, .99f);
	}
	return true;
}

float HomogeneousVolume::ScatterSSS(const Ray &ray, const float u,
		const bool scatteredStart, const Spectrum &channelWeights,
		Spectrum *connectionThroughput, Spectrum *connectionEmission,
		const SSSFlightProposal *proposal) const {
	const float segmentLength = ray.maxt - ray.mint;
	const bool scatterAllowed = (!scatteredStart || multiScattering);

	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	Spectrum sigmaA, sigmaS;
	SSSScatteringCoefficients(hitPoint, sigmaA, sigmaS);
	const Spectrum emission = Emission(hitPoint);

	Spectrum segmentTransmittance, segmentEmission;
	float scatterDistance;
	if (proposal && proposal->active && scatterAllowed) {
		float pick = u;
		unsigned selected = 8;
		for (unsigned i = 0; i < 8; ++i) {
			if (pick < proposal->probabilities[i]) { selected = i; break; }
			pick -= proposal->probabilities[i];
		}
		const float remapped = Clamp(pick / proposal->probabilities[selected], 0.f, .99999994f);
		const float flight = -logf(1.f - remapped) / proposal->rates[selected];
		const bool scattered = flight < segmentLength;
		const float distance = scattered ? flight : segmentLength;
		const Spectrum sigmaT = sigmaA + sigmaS;
		double physicalTr[3];
		for (unsigned i = 0; i < 3; ++i)
			physicalTr[i] = exp(-double(distance) * sigmaT.c[i]);
		double pdf = 0.;
		for (unsigned i = 0; i < 9; ++i) {
			// The classic channel component normally has exactly the same
			// extinction as the physical flight. Reuse its double-precision
			// exponential, preserving the mixture summation order. Textured
			// flights with a different source coefficient keep their own rate.
			const double tr = i < 3 && proposal->rates[i] == sigmaT.c[i] ?
					physicalTr[i] : exp(-double(proposal->rates[i]) * distance);
			pdf += proposal->probabilities[i] * tr * (scattered ? proposal->rates[i] : 1.f);
		}
		for (unsigned i = 0; i < 3; ++i)
			segmentTransmittance.c[i] = pdf > 0. ?
					physicalTr[i] * (scattered ? sigmaT.c[i] : 1.f) / pdf : 0.f;
		scatterDistance = scattered ? distance : -1.f;
	} else
		scatterDistance = ScatterChromatic(u, scatterAllowed,
				segmentLength, sigmaA, sigmaS, emission, channelWeights,
				segmentTransmittance, segmentEmission);

	*connectionEmission += *connectionThroughput * emission;
	*connectionThroughput *= segmentTransmittance;

	return (scatterDistance == -1.f) ? -1.f : (ray.mint + scatterDistance);
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

	Spectrum sigmaA, sigmaS, emission;
	if (constSigmaParams && !Spectral::Current()) {
		// Constant params fast path: no HitPoint / virtual texture evals.
		sigmaA = constSigmaA;
		sigmaS = constSigmaS;
		emission = constEmission;
	} else {
		// Point where to evaluate the volume
		HitPoint hitPoint;
		hitPoint.Init();
		hitPoint.fixedDir = ray.d;
		hitPoint.p = ray.o;
		hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
		hitPoint.passThroughEvent = u;

		sigmaA = SigmaA(hitPoint);
		sigmaS = SigmaS(hitPoint);
		emission = Emission(hitPoint);
	}

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

	Spectrum sigmaA, sigmaS, emission;
	if (constSigmaParams && !Spectral::Current()) {
		sigmaA = constSigmaA;
		sigmaS = constSigmaS;
		emission = constEmission;
	} else {
		// Point where to evaluate the volume
		HitPoint hitPoint;
		hitPoint.Init();
		hitPoint.fixedDir = ray.d;
		hitPoint.p = ray.o;
		hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
		hitPoint.passThroughEvent = u;

		sigmaA = SigmaA(hitPoint);
		sigmaS = SigmaS(hitPoint);
		emission = Emission(hitPoint);
	}
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

	Spectrum sigmaT;
	if (constSigmaParams && !Spectral::Current())
		sigmaT = constSigmaA + constSigmaS;
	else {
		// Point where to evaluate the volume
		HitPoint hitPoint;
		hitPoint.Init();
		hitPoint.fixedDir = ray.d;
		hitPoint.p = ray.o;
		hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
		hitPoint.passThroughEvent = u;

		if (IsSSSParametrized()) {
			Spectrum absorption, scattering;
			SSSScatteringCoefficients(hitPoint, absorption, scattering);
			sigmaT = absorption + scattering;
		} else
			sigmaT = SigmaT(hitPoint);
	}
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
