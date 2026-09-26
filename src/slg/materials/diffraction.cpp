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

#include <limits>

#include "slg/materials/diffraction.h"
#include "luxrays/core/color/spectral.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Diffraction grating material (1D reflective grating)
//
// Physics (Stam'99 "Diffraction Shaders", GPU Gems ch.8): grooves along the
// local tangent direction t form a 1D grating with period d. For each
// diffraction order m the outgoing direction is the discrete delta lobe
//
//   a_m = -a_f + m * lambda / d,   b_m = -b_f,   c_m = +sqrt(1 - a_m^2 - b_m^2)
//
// where (a_f, b_f, c_f) is the fixed (incident) direction expressed in the
// (s, t, n) frame (s = grating direction in the tangent plane). m = 0 gives
// the ordinary mirror direction. Orders are importance sampled with pdf
// proportional to the groove envelope (lamellar profile with fill factor and
// optional blaze angle) so the returned weight collapses to the constant kr.
//
// Spectral mode: diffract at the path hero wavelength and collapse the
// secondary bins (same mechanism as dispersive glass). RGB mode: jitter one
// wavelength per sample and convert it with WaveLength2RGB so white light
// still integrates to kr.
//------------------------------------------------------------------------------

// Same piecewise-linear approximation used by glass.cpp dispersion (keep in
// sync with GlassMaterial_WaveLength2RGB in materialdefs_funcs_glass.cl)
static Spectrum DfrWaveLength2RGB(const float waveLength) {
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
		return Spectrum(0.f);

	float factor;
	if ((waveLength >= 380.f) && (waveLength < 420.f))
		factor = .3f + .7f * (waveLength - 380.f) / (420.f - 380.f);
	else if ((waveLength >= 420) && (waveLength < 700))
		factor = 1.f;
	else
		factor = .3f + .7f * (780.f - waveLength) / (780.f - 700.f);

	return Spectrum(r * factor, g * factor, b * factor) *
			Spectrum(1.f / .5652729f, 1.f / .36875f, 1.f / .265375f);
}

// Bit-exact Wang hash -> [0,1). Used to derive the second gaussian uniform
// so CPU and GPU jitter identically given the same u0/u1/passThroughEvent.
static inline float DfrHash01(const float a, const float b, const float c) {
	u_int h = ((const u_int &)a) ^ (((const u_int &)b) << 1) ^ ((const u_int &)c);
	h = (h ^ 61u) ^ (h >> 16u);
	h += h << 3u;
	h ^= h >> 4u;
	h *= 0x27d4eb2du;
	h ^= h >> 15u;
	return (h >> 8u) * (1.f / 16777216.f);
}

static inline float DfrSinc2(const float x) {
	const float s = (fabsf(x) < 1e-6f) ? 1.f : sinf(x) / x;
	return s * s;
}

DiffractionMaterial::DiffractionMaterial(TexRef frontTransp, TexRef backTransp,
		TexRef emitted, TexRef bump,
		TexRef refl, TexRef spacingTex, TexRef roughTex, TexRef fillTex,
		const DiffractionOrientation orient,
		const Point &c, const float cu, const float cv,
		const float bl, const u_int mo) :
		Material(frontTransp, backTransp, emitted, bump), Kr(refl),
		spacing(spacingTex), roughness(roughTex), fillFactor(fillTex),
		orientation(orient), center(c), centerU(cu), centerV(cv),
		blaze(bl), maxOrder(mo) {
	glossiness = 1.f;
}

Spectrum DiffractionMaterial::Evaluate(const HitPoint &hitPoint,
	const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
	float *directPdfW, float *reversePdfW) const {
	return Spectrum();
}

Spectrum DiffractionMaterial::Sample(const HitPoint &hitPoint,
	const Vector &localFixedDir, Vector *localSampledDir,
	const float u0, const float u1, const float passThroughEvent,
	float *pdfW, BSDFEvent *event) const {
	const Spectrum krSpec = Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	if (krSpec.Black())
		return Spectrum();

	const float d = Max(1e-3f, spacing->GetFloatValue(hitPoint)); // nm
	const float fill = Clamp(fillFactor->GetFloatValue(hitPoint), 0.f, 1.f);

	//----------------------------------------------------------------------
	// Groove frame: s (grating direction) and t (groove direction) in local
	// shading coordinates (z = shade normal)
	//----------------------------------------------------------------------
	float sx, sy;
	switch (orientation) {
		case DIFFRACTION_U:
			// Grooves along U: grating direction is V
			sx = 0.f; sy = 1.f;
			break;
		case DIFFRACTION_V:
			// Grooves along V: grating direction is U
			sx = 1.f; sy = 0.f;
			break;
		case DIFFRACTION_RADIAL_UV:
		case DIFFRACTION_RADIAL:
		default: {
			// World-space radial direction projected onto the tangent plane
			Vector radial;
			if (orientation == DIFFRACTION_RADIAL_UV) {
				const float du = hitPoint.defaultUV.u - centerU;
				const float dv = hitPoint.defaultUV.v - centerV;
				radial = du * hitPoint.dpdu + dv * hitPoint.dpdv;
			} else {
				const Point cWorld = hitPoint.localToWorld * center;
				radial = hitPoint.p - cWorld;
			}
			const Vector n(hitPoint.shadeN);
			radial -= Dot(radial, n) * n;

			if (radial.LengthSquared() < 1e-12f) {
				sx = 1.f; sy = 0.f;
			} else {
				const Vector r = Normalize(radial);
				// Frame(dpdu, dpdv, shadeN): X ~ dpdu dir, Y = Z x X
				const Frame f(hitPoint.GetFrame());
				const Vector rl = f.ToLocal(r);
				const float len = sqrtf(rl.x * rl.x + rl.y * rl.y);
				if (len < 1e-6f) {
					sx = 1.f; sy = 0.f;
				} else {
					sx = rl.x / len; sy = rl.y / len;
				}
			}
			break;
		}
	}
	// t = n x s (in-plane perpendicular to s)
	const float tx = -sy, ty = sx;

	// Fixed direction in the (s, t) rotated basis
	const float aF = localFixedDir.x * sx + localFixedDir.y * sy;
	const float bF = -localFixedDir.x * tx - localFixedDir.y * ty; // = -(f . t)
	const float cF = localFixedDir.z;

	//----------------------------------------------------------------------
	// Wavelength selection
	//----------------------------------------------------------------------
	const PathWavelengths *sw = Spectral::Current();
	float lambda, heroWeight = 1.f;
	Spectrum waveColor(1.f);
	if (sw) {
		lambda = sw->w[sw->hero];
		heroWeight = Spectral::CollapseToHero();
	} else {
		lambda = Lerp(passThroughEvent, 380.f, 780.f);
		waveColor = DfrWaveLength2RGB(lambda);
	}

	const float lOverD = lambda / d;

	//----------------------------------------------------------------------
	// Valid order range: |a_m| <= sqrt(1 - b_m^2), b_m = -b_f
	//----------------------------------------------------------------------
	const float sb2 = Max(0.f, 1.f - bF * bF);
	const float sb = sqrtf(sb2);
	const int maxO = (int)Min(maxOrder, (u_int)32);
	const int mMin = Max(-maxO, (int)ceilf((aF - sb) / lOverD - 1e-4f));
	const int mMax = Min(maxO, (int)floorf((aF + sb) / lOverD + 1e-4f));

	if (mMin > mMax) {
		// No propagating order besides (possibly) none at all: behave as a
		// mirror (order 0 is the only diffracted lobe that always exists
		// geometrically when the surface is smooth enough)
		*localSampledDir = Vector(-localFixedDir.x, -localFixedDir.y, localFixedDir.z);
		*pdfW = 1.f;
		*event = SPECULAR | REFLECT;
		return krSpec * (sw ? heroWeight : 1.f) * waveColor;
	}

	//----------------------------------------------------------------------
	// Order weights: lamellar groove envelope, sinc^2 around the facet
	// specular direction a_spec (blaze angle tilts it away from a_m = -a_f)
	//----------------------------------------------------------------------
	const float aSpec = -aF * cosf(2.f * blaze) + cF * sinf(2.f * blaze);
	const float envScale = M_PI * fill / lOverD;
	float w[65];
	float wSum = 0.f;
	for (int m = mMin; m <= mMax; ++m) {
		const float am = -aF + m * lOverD;
		w[m - mMin] = DfrSinc2(envScale * (am - aSpec));
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
		// Degenerate envelope (all orders at sinc zeros): pick the order
		// closest to the facet specular
		float best = std::numeric_limits<float>::infinity();
		for (int m = mMin; m <= mMax; ++m) {
			const float am = -aF + m * lOverD;
			const float e = fabsf(am - aSpec);
			if (e < best) {
				best = e;
				mPick = m;
			}
		}
	}

	float aM = -aF + mPick * lOverD;
	float bM = -bF;

	//----------------------------------------------------------------------
	// Groove roughness: gaussian jitter of the cone direction (keeps the
	// delta convention; blurs the rainbow bands like a real CD)
	//----------------------------------------------------------------------
	const float rough = Clamp(roughness->GetFloatValue(hitPoint), 0.f, 1.f);
	if (rough > 0.f) {
		const float sigma = rough * .25f;
		const float uj2 = DfrHash01(u0, u1, passThroughEvent);
		const float r = sqrtf(-2.f * logf(Max(1e-30f, 1.f - u1)));
		const float ga = r * cosf(2.f * M_PI * uj2) * sigma;
		const float gb = r * sinf(2.f * M_PI * uj2) * sigma;
		const float aj = aM + ga, bj = bM + gb;
		if (aj * aj + bj * bj < 1.f) {
			aM = aj; bM = bj;
		}
	}

	const float cM = sqrtf(Max(0.f, 1.f - aM * aM - bM * bM));

	// Back to local shading coords: dir = aM * s + bM * t + cM * n
	*localSampledDir = Vector(
			aM * sx + bM * tx,
			aM * sy + bM * ty,
			cM);

	*pdfW = 1.f;
	*event = SPECULAR | REFLECT;

	return krSpec * (sw ? heroWeight : 1.f) * waveColor;
}

void DiffractionMaterial::AddReferencedTextures(std::unordered_set<const Texture *> &referencedTexs) const {
	Material::AddReferencedTextures(referencedTexs);

	Kr->AddReferencedTextures(referencedTexs);
	spacing->AddReferencedTextures(referencedTexs);
	roughness->AddReferencedTextures(referencedTexs);
	fillFactor->AddReferencedTextures(referencedTexs);
}

void DiffractionMaterial::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	Material::UpdateTextureReferences(oldTex, newTex);

	if (Kr == &oldTex)
		Kr = &newTex;
	if (spacing == &oldTex)
		spacing = &newTex;
	if (roughness == &oldTex)
		roughness = &newTex;
	if (fillFactor == &oldTex)
		fillFactor = &newTex;
}

PropertiesUPtr DiffractionMaterial::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.materials." + name + ".type")("diffraction"));
	props->Set(Property("scene.materials." + name + ".kr")(Kr->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".spacing")(spacing->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".roughness")(roughness->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".fillfactor")(fillFactor->GetSDLValue()));
	string orientName = "radial";
	switch (orientation) {
		case DIFFRACTION_U: orientName = "u"; break;
		case DIFFRACTION_V: orientName = "v"; break;
		case DIFFRACTION_RADIAL_UV: orientName = "radialuv"; break;
		default: break;
	}
	props->Set(Property("scene.materials." + name + ".orientation")(orientName));
	props->Set(Property("scene.materials." + name + ".center")(center.x, center.y, center.z));
	props->Set(Property("scene.materials." + name + ".centeru")(centerU));
	props->Set(Property("scene.materials." + name + ".centerv")(centerV));
	props->Set(Property("scene.materials." + name + ".blaze")(blaze * (180.f / M_PI)));
	props->Set(Property("scene.materials." + name + ".orders")((int)maxOrder));
	props->Set(Material::ToProperties(imgMapCache, useRealFileName));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
