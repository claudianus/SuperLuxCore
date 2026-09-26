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
 *                                                                         *
 *   See the License for the specific language governing permissions and   *
 *   limitations under the License.                                        *
 ***************************************************************************/

#ifndef _SLG_MICROFACET_H
#define _SLG_MICROFACET_H

// Modern shared microfacet / diffuse-lobe library used by the OpenPBR
// uber-material and the modernized legacy materials. Everything is expressed
// in the material local frame (+Z = shading normal). References:
//   - GGX anisotropic NDF / Smith masking: Walter 2007, Heitz 2014
//   - VNDF sampling: Dupuy & Benyoub 2023 (spherical caps)
//   - Directional-albedo fits + multi-scatter compensation:
//     Kulla-Conty 2017 / Turquin 2019 (fits from the MaterialX implementation)
//   - EON rough diffuse: Portsmouth, Kutz, Hill 2024 (arXiv 2410.18026)
//   - F82 metal Fresnel: Kutz 2021 / OpenPBR spec
//   - Zeltner SGGX-LTC fuzz/sheen: Zeltner 2022 (MaterialX pbrlib impl.)

#include "luxrays/core/geometry/vector_normal.h"
#include "luxrays/core/color/color.h"
#include "slg/materials/material.h"
#include "slg/textures/fresnel/fresneltexture.h"

namespace slg {

// Anisotropic GGX (Trowbridge-Reitz) NDF. alphaX/alphaY are the squared
// perceptual roughnesses along the local X (tangent) and Y axes.
inline float GgxD(const luxrays::Vector &wh, const float alphaX, const float alphaY) {
	const float hx = wh.x / alphaX, hy = wh.y / alphaY;
	const float d = hx * hx + hy * hy + wh.z * wh.z;
	return 1.f / (M_PI * alphaX * alphaY * d * d);
}

// Smith Lambda for GGX (anisotropic form, per-direction).
inline float GgxLambda(const luxrays::Vector &w, const float alphaX, const float alphaY) {
	const float a2 = alphaX * alphaX * w.x * w.x + alphaY * alphaY * w.y * w.y;
	const float cos2 = w.z * w.z;
	if (cos2 <= 0.f)
		return 0.f;
	// Lambda = (-1 + sqrt(1 + alphaProjected^2 * tan^2))/2
	return (-1.f + sqrtf(1.f + a2 / cos2)) * .5f;
}

inline float GgxG1(const luxrays::Vector &w, const float alphaX, const float alphaY) {
	return 1.f / (1.f + GgxLambda(w, alphaX, alphaY));
}

// Height-correlated Smith masking-shadowing (Heitz 2014, eq. 99).
inline float GgxG2(const luxrays::Vector &wi, const luxrays::Vector &wo,
		const float alphaX, const float alphaY) {
	const float lI = GgxLambda(wi, alphaX, alphaY);
	const float lO = GgxLambda(wo, alphaX, alphaY);
	return 1.f / (1.f + lI + lO);
}

// Visible-normal (VNDF) sampling of the GGX distribution, Dupuy & Benyoub 2023
// "Sampling Visible GGX Normals with Spherical Caps". Returns wh in the local
// frame; the incident direction wi = 2*dot(wh,wo)*wh - wo.
inline luxrays::Vector GgxSampleVNDF(const luxrays::Vector &wo,
		const float alphaX, const float alphaY,
		const float u0, const float u1) {
	// Stretch the view direction to the hemisphere configuration
	luxrays::Vector v = luxrays::Normalize(luxrays::Vector(wo.x * alphaX, wo.y * alphaY, wo.z));

	// Sample a spherical cap in (-v.z, 1]
	const float phi = 2.f * M_PI * u0;
	const float z = (1.f - u1) * (1.f + v.z) - v.z;
	const float sinTheta = sqrtf(luxrays::Clamp(1.f - z * z, 0.f, 1.f));
	const float x = sinTheta * cosf(phi);
	const float y = sinTheta * sinf(phi);
	luxrays::Vector c(x, y, z);

	luxrays::Vector h = c + v;
	// Back to the ellipsoid configuration
	h = luxrays::Normalize(luxrays::Vector(h.x * alphaX, h.y * alphaY,
			luxrays::Max(h.z, 0.f)));

	return h;
}

// Solid-angle PDF of a half-vector wh sampled via the VNDF:
// p(wh) = D(wh) * G1(wo) * |wo.wh| / |wo.z|
inline float GgxVNDFHalfPdf(const luxrays::Vector &wo, const luxrays::Vector &wh,
		const float alphaX, const float alphaY) {
	const float ndotV = fabsf(wo.z);
	if (ndotV < 1e-6f)
		return 0.f;
	return GgxD(wh, alphaX, alphaY) * GgxG1(wo, alphaX, alphaY) *
			fabsf(luxrays::Dot(wo, wh)) / ndotV;
}

// Solid-angle PDF of a reflection direction wi sampled via the VNDF:
// p(wi) = D(wh) * G1(wo) / (4 * |wo.z|)
inline float GgxVNDFReflectionPdf(const luxrays::Vector &wo, const luxrays::Vector &wh,
		const float alphaX, const float alphaY) {
	const float ndotV = fabsf(wo.z);
	if (ndotV < 1e-6f)
		return 0.f;
	return GgxD(wh, alphaX, alphaY) * GgxG1(wo, alphaX, alphaY) / (4.f * ndotV);
}

// OpenPBR anisotropy mapping: perceptual roughness r and anisotropy a in
// [0,1] -> (alphaT, alphaB), preserving alpha_t^2 + alpha_b^2 = 2*alpha^2
// (OpenPBR spec eq. "openpbr-anisotropy-formula").
inline void OpenPBRAnisoAlphas(const float roughness, const float anisotropy,
		float &alphaT, float &alphaB) {
	const float r2 = luxrays::Sqr(luxrays::Clamp(roughness, 0.f, 1.f));
	const float a = luxrays::Clamp(anisotropy, 0.f, 1.f);
	alphaT = r2 * sqrtf(2.f / (1.f + luxrays::Sqr(1.f - a)));
	alphaB = (1.f - a) * alphaT;
	alphaT = luxrays::Max(alphaT, 1e-4f);
	alphaB = luxrays::Max(alphaB, 1e-4f);
}

// Rotate a direction in the local tangent plane (anisotropy orientation).
// rot in [0,1] maps to [0, 2pi). cosA/sinA are precomputed by the caller.
inline luxrays::Vector RotateXY(const luxrays::Vector &w,
		const float cosA, const float sinA) {
	return luxrays::Vector(w.x * cosA - w.y * sinA, w.x * sinA + w.y * cosA, w.z);
}

//------------------------------------------------------------------------------
// GGX directional albedo fits (Kulla-Conty/Turquin, MaterialX coefficients)
//------------------------------------------------------------------------------

// Returns the fitted (A, B) pair so that E = F0*A + F90*B is the single-scatter
// directional albedo of an isotropic GGX lobe with Schlick-like Fresnel.
inline void GgxDirAlbedoAB(const float mu, const float alpha, float &A, float &B) {
	const float x = mu, y = alpha;
	const float x2 = x * x, y2 = y * y;
	const float r0 = 0.1003f + (-0.6303f) * x + 9.748f * y + (-2.038f) * x * y +
		29.34f * x2 + (-8.245f) * y2 + (-26.44f) * x2 * y + 19.99f * x * y2 + (-5.448f) * x2 * y2;
	const float r1 = 0.9345f + (-2.323f) * x + 2.229f * y + (-3.748f) * x * y +
		1.424f * x2 + (-0.7684f) * y2 + 1.436f * x2 * y + 0.2913f * x * y2 + 0.6286f * x2 * y2;
	const float r2 = 1.f + (-1.765f) * x + 8.263f * y + 11.53f * x * y +
		28.96f * x2 + (-7.507f) * y2 + (-36.11f) * x2 * y + 15.86f * x * y2 + 33.37f * x2 * y2;
	const float r3 = 1.f + 0.2281f * x + 15.94f * y + (-55.83f) * x * y +
		13.08f * x2 + 41.26f * y2 + 54.9f * x2 * y + 300.2f * x * y2 + (-285.1f) * x2 * y2;
	A = luxrays::Clamp(r0 / r2, 0.f, 1.f);
	B = luxrays::Clamp(r1 / r3, 0.f, 1.f);
}

// Scalar directional albedo of the single-scatter lobe (F0=F90=1 -> Ess).
inline float GgxEss(const float mu, const float alpha) {
	float A, B;
	GgxDirAlbedoAB(mu, alpha, A, B);
	return A + B;
}

// Directional albedo of a GGX lobe with F0/F90 Fresnel endpoints.
inline luxrays::Spectrum GgxDirAlbedo(const float mu, const float alpha,
		const luxrays::Spectrum &F0, const luxrays::Spectrum &F90) {
	float A, B;
	GgxDirAlbedoAB(mu, alpha, A, B);
	return F0 * A + F90 * B;
}

// Cosine-weighted hemisphere average of a Schlick-exponent-5 Fresnel:
// exact for Schlick; used as Fresnel average for compensation.
inline luxrays::Spectrum GgxFresnelAverage(const luxrays::Spectrum &F0,
		const luxrays::Spectrum &F90) {
	return F0 + (F90 - F0) * (1.f / 21.f);
}

// Turquin multiple-scattering compensation multiplier (eq. 14/16):
// f_total ~= f_ss * (1 + Favg * (1 - Ess) / Ess)
inline float GgxMSCompensation(const float mu, const float alpha, const float Favg) {
	const float Ess = GgxEss(mu, alpha);
	if (Ess <= 1e-4f)
		return 1.f;
	return 1.f + Favg * (1.f - Ess) / Ess;
}

inline luxrays::Spectrum GgxMSCompensation(const float mu, const float alpha,
		const luxrays::Spectrum &Favg) {
	const float Ess = GgxEss(mu, alpha);
	if (Ess <= 1e-4f)
		return luxrays::Spectrum(1.f);
	return luxrays::Spectrum(1.f) + Favg * ((1.f - Ess) / Ess);
}

//------------------------------------------------------------------------------
// Height-tracking multi-bounce evaluation for Smith GGX conductors
// (Heitz et al. 2016, "Multiple-Scattering Microfacet BSDFs with the Smith
// Model", SIGGRAPH). A random walk through the microflake field: the free
// path in height is exponential with rate sigma(v)/|v.z| for approach
// direction v = -w, the hit normal is drawn from the visible normal
// distribution of -w (slope-space P22 sampling, valid for both sides of the
// surface), and each vertex contributes F * D(wh) * G1(wo, h) / (4*sigma)
// toward the outgoing direction, where G1(wo, h) = exp(h * Lambda(wo)) is
// the transmittance from depth h to the boundary. All correlations are
// captured by tracking the explicit height, so the estimator is unbiased
// (white-furnace albedo of an F=1 conductor is exactly 1).
//
// Material::Evaluate carries no RNG state, so the walks are driven by a
// deterministic hash of (wStart, wTarget): evaluation stays a fixed function
// of the two directions while different directions see different
// realizations. The result already includes the single-scatter term;
// sampling can keep using plain VNDF because its pdf covers the whole
// multi-bounce support, so the f/pdf pair remains unbiased.
//------------------------------------------------------------------------------

constexpr int GGX_MS_DEPTH = 8;  // max internal vertices per walk
constexpr int GGX_MS_WALKS = 4;  // independent walk restarts per evaluation

// Microflake cross-section for a direction v of any sign:
// |v.z|*(1+Lambda(v)) for upward v, |v.z|*Lambda(v) for downward v.
inline float GgxMSSigma(const luxrays::Vector &v,
		const float alphaX, const float alphaY) {
	const float l = GgxLambda(v, alphaX, alphaY); // sign-agnostic (z^2)
	return fabsf(v.z) * ((v.z > 0.f) ? 1.f + l : l);
}

// Deterministic per-evaluation RNG. Seeded from the raw float bits of the
// input directions plus the shading point so that Evaluate() stays a fixed
// function of its inputs, while different directions and surface points still
// see independent walk realizations (error becomes noise, not frozen bias).
inline u_int GgxMSBits(const float f) {
	union { float f; u_int u; } v;
	v.f = f;
	return v.u;
}

inline u_int GgxMSSeed(const luxrays::Vector &a, const luxrays::Vector &b,
		const luxrays::Point &p, const int walk) {
	u_int s = GgxMSBits(a.x) * 0x8da6b343u + GgxMSBits(a.y) * 0xd8163841u +
			GgxMSBits(a.z) * 0xcb1ab31fu + GgxMSBits(b.x) * 0x9e3779b9u +
			GgxMSBits(b.y) * 0x85ebca6bu + GgxMSBits(b.z) * 0xc2b2ae35u +
			GgxMSBits(p.x) * 0x165667b1u + GgxMSBits(p.y) * 0xd3a2646cu +
			GgxMSBits(p.z) * 0xfd7046c5u + (u_int)walk * 0x27d4eb2fu;
	s ^= s >> 15; s *= 0x2c1b3c6du; s ^= s >> 12; s *= 0x297a2d39u; s ^= s >> 15;
	return s;
}

inline float GgxMSRand(u_int &s) {
	s = s * 1664525u + 1013904223u;
	return (float)(s >> 8) * (1.f / 16777216.f);
}

// Visible-normal sampler for an approach direction wi of any sign
// (GGX P22 slope-space form, after Heitz'16 / facet-forge). For wi.z < 0
// it returns the underside-facing normals an ascending ray can hit.
inline luxrays::Vector GgxMSSampleVisibleNormal(const luxrays::Vector &wi,
		const float alphaX, const float alphaY,
		const float r0, const float r1, const float r2) {
	// stretch to the isotropic alpha=1 configuration
	const luxrays::Vector v = luxrays::Normalize(
			luxrays::Vector(wi.x * alphaX, wi.y * alphaY, wi.z));
	const float th = acosf(luxrays::Clamp(v.z, -1.f, 1.f));
	float sx, sy;
	if (th < 1e-4f) {
		const float r = sqrtf(r0 / luxrays::Max(1.f - r0, 1e-7f));
		const float ph = 2.f * M_PI * r1;
		sx = r * cosf(ph);
		sy = r * sinf(ph);
	} else {
		const float sn = sinf(th), cs = cosf(th), tn = sn / cs;
		const float sig = .5f * (cs + 1.f);
		if (sig < 1e-4f) {
			sx = 0.f;
			sy = 0.f;
		} else {
			const float c = 1.f / sig;
			const float A = 2.f * r0 / (cs * c) - 1.f;
			const float B = tn;
			const float tmp = 1.f / (A * A - 1.f);
			const float D = sqrtf(luxrays::Max(0.f,
					B * B * tmp * tmp - (A * A - B * B) * tmp));
			const float s1 = B * tmp - D, s2 = B * tmp + D;
			sx = (A < 0.f || s2 > 1.f / tn) ? s1 : s2;
			sy = sqrtf(luxrays::Max(0.f, -1.f - sx * sx +
					(1.f + sx * sx) / powf(luxrays::Max(1.f - r2, 1e-7f), 2.f / 3.f))) *
					sinf(2.f * M_PI * r1);
		}
	}
	// align with the view azimuth and stretch back
	const float ph = atan2f(v.y, v.x);
	const float cph = cosf(ph), sph = sinf(ph);
	float sxr = cph * sx - sph * sy, syr = sph * sx + cph * sy;
	sxr *= alphaX;
	syr *= alphaY;
	if (!std::isfinite(sxr) || !std::isfinite(syr))
		return (wi.z > 0.f) ? luxrays::Vector(0.f, 0.f, 1.f) :
				luxrays::Normalize(luxrays::Vector(wi.x, wi.y, 0.f));
	return luxrays::Normalize(luxrays::Vector(-sxr, -syr, 1.f));
}

// Full Smith multi-bounce conductor BRDF (single + multiple scattering).
// Returns f * |cos(wTarget)| following the LuxCore convention: to obtain the
// usual f*|cos(lightDir)|, pass the light/incident direction as wTarget and
// the outgoing/eye direction as wStart. Both directions must lie in the upper
// hemisphere of the material local frame.
inline luxrays::Spectrum GgxMSConductorEval(const luxrays::Vector &wStart,
		const luxrays::Vector &wTarget, const luxrays::Point &p,
		const float alphaX, const float alphaY,
		const luxrays::Spectrum &eta, const luxrays::Spectrum &kappa) {
	if ((wStart.z <= 1e-5f) || (wTarget.z <= 1e-5f))
		return luxrays::Spectrum(0.f);

	// Transmittance from depth h toward wTarget: G1 = exp(h * Lambda(wTarget))
	const float lambdaOut = GgxLambda(wTarget, alphaX, alphaY);

	luxrays::Spectrum result(0.f);
	for (int k = 0; k < GGX_MS_WALKS; ++k) {
		u_int seed = GgxMSSeed(wStart, wTarget, p, k);
		luxrays::Vector wr(-wStart.x, -wStart.y, -wStart.z); // ray travel dir
		float hr = 0.f; // height in extinction units (h <= 0 inside, >= 0 out)
		luxrays::Spectrum weight(1.f);

		for (int i = 0; i < GGX_MS_DEPTH; ++i) {
			const float st = GgxMSSigma(
					luxrays::Vector(-wr.x, -wr.y, -wr.z), alphaX, alphaY);
			float h;
			if (st < 1e-5f) {
				if (wr.z >= 0.f)
					break; // near-horizontal ascent: escapes unhit
				h = hr;    // grazing descent: hits again at same height
			} else {
				const float u = luxrays::Max(GgxMSRand(seed), 1e-7f);
				h = luxrays::Min(0.f, hr) - logf(u) * wr.z / st;
			}
			if (h >= 0.f)
				break; // left the surface
			hr = h;

			const luxrays::Vector m = GgxMSSampleVisibleNormal(
					luxrays::Vector(-wr.x, -wr.y, -wr.z), alphaX, alphaY,
					GgxMSRand(seed), GgxMSRand(seed), GgxMSRand(seed));

			// NEE vertex: F * D(wh) * G1(wo, h) / (4*sigma), wh = half(-wr, wo)
			const luxrays::Vector v(-wr.x, -wr.y, -wr.z);
			const luxrays::Vector hsum = v + wTarget;
			if (luxrays::Dot(hsum, hsum) > 1e-12f) {
				const luxrays::Vector wh = luxrays::Normalize(hsum);
				// D(wh) is only defined on the upper hemisphere; skipping
				// wh.z <= 0 is required for energy conservation
				if ((wh.z > 0.f) && (luxrays::Dot(v, wh) > 1e-9f))
					result += weight *
							FresnelTexture::GeneralEvaluate(eta, kappa,
									luxrays::Dot(v, wh)) *
							(GgxD(wh, alphaX, alphaY) * expf(h * lambdaOut) /
									(4.f * st));
			}

			// Mirror facet: throughput *= F(|wr.m|), new dir = reflect about m
			const float wrm = luxrays::Dot(wr, m);
			weight *= FresnelTexture::GeneralEvaluate(eta, kappa, fabsf(wrm));
			wr = wr - m * (2.f * wrm);
		}
	}

	return result * (1.f / GGX_MS_WALKS);
}

//------------------------------------------------------------------------------
// Fresnel helpers
//------------------------------------------------------------------------------

// OpenPBR "F82-tint" metallic Fresnel: Schlick curve corrected to hit the
// user-specified edge tint at muBar = cos(82deg) = 1/7.
inline luxrays::Spectrum FresnelF82(const float mu, const luxrays::Spectrum &F0,
		const luxrays::Spectrum &edgeTint) {
	const float mubar = 1.f / 7.f;
	const luxrays::Spectrum FSchlickBar = F0 + (luxrays::Spectrum(1.f) - F0) *
			powf(1.f - mubar, 5.f);
	// F(muBar) desired = edgeTint * FSchlick(muBar); correction coefficient:
	const luxrays::Spectrum K = FSchlickBar * (luxrays::Spectrum(1.f) - edgeTint) /
			(mubar * powf(1.f - mubar, 6.f));
	const luxrays::Spectrum FSchlick = F0 + (luxrays::Spectrum(1.f) - F0) *
			powf(1.f - mu, 5.f);
	return (FSchlick - K * (mu * powf(1.f - mu, 6.f))).Clamp(0.f, 1.f);
}

// Exact complex-index conductor Fresnel (n + ik), PBRT formulation,
// per Spectrum channel. eta/k are relative to the incident medium.
inline luxrays::Spectrum FresnelConductor(const float cosI,
		const luxrays::Spectrum &eta, const luxrays::Spectrum &k) {
	luxrays::Spectrum F;
	const float c = luxrays::Clamp(cosI, -1.f, 1.f);
	const float cos2 = c * c, sin2 = 1.f - cos2;
	for (u_int i = 0; i < 3; ++i) {
		const float eta2 = eta.c[i] * eta.c[i], k2 = k.c[i] * k.c[i];
		const float t0 = eta2 - k2 - sin2;
		const float a2b2 = sqrtf(t0 * t0 + 4.f * eta2 * k2);
		const float t1 = a2b2 + cos2;
		const float a = sqrtf(.5f * (a2b2 + t0));
		const float t2 = 2.f * c * a;
		const float rs = (t1 - t2) / (t1 + t2);
		const float t3 = cos2 * a2b2 + sin2 * sin2;
		const float t4 = t2 * sin2;
		const float rp = rs * (t3 - t4) / (t3 + t4);
		F.c[i] = luxrays::Clamp(.5f * (rp + rs), 0.f, 1.f);
	}
	return F;
}

// Gulbrandsen 2014 artist-friendly (F0, edge-tint) -> (n, k) fit, used when a
// conductor needs a physical complex IOR (e.g. thin film on metal).
// edgeTint g in [0,1] interpolates n between n_max (g=0) and n_min (g=1).
inline void GulbrandsenNK(const luxrays::Spectrum &F0, const luxrays::Spectrum &edgeTint,
		luxrays::Spectrum &n, luxrays::Spectrum &k) {
	for (u_int i = 0; i < 3; ++i) {
		const float r = luxrays::Clamp(F0.c[i], 0.f, .99f);
		const float rsqrt = sqrtf(r);
		const float nMin = (1.f - r) / (1.f + r);
		const float nMax = (1.f + rsqrt) / (1.f - rsqrt);
		const float ni = luxrays::Lerp(edgeTint.c[i], nMax, nMin);
		const float np1 = ni + 1.f, nm1 = ni - 1.f;
		const float k2 = luxrays::Max((np1 * np1 * r - nm1 * nm1) / (1.f - r), 0.f);
		n.c[i] = ni;
		k.c[i] = sqrtf(k2);
	}
}

// Average dielectric Fresnel over the hemisphere, d'Eon 2021 fit
// (E_F(eta) = 2*int F(mu) mu dmu). eta is the relative IOR n_t/n_i.
inline float FresnelDielectricAverage(const float eta) {
	if (eta > 1.f) {
		const float e = luxrays::Clamp(eta, 1.0001f, 3.f);
		return logf((10893.f * e - 1438.2f) / (-774.4f * e * e + 10212.f * e + 1.f));
	} else if (eta < 1.f)
		return 1.f - eta * eta * (1.f - FresnelDielectricAverage(1.f / eta));
	else
		return 0.f;
}

// Defined below; forward declaration for FresnelDielectricModulated.
inline float FresnelDielectric(const float cosI, const float eta);

// OpenPBR dielectric Fresnel modulated by specular_weight (spec PR #247):
// weight scales F0 via a modified IOR eta'; for internal reflections the
// "squeezed" curve at the refracted angle preserves energy (Stokes).
inline float FresnelDielectricModulated(const float cosI, const float etaTI,
		const float weight) {
	const float etam1 = etaTI - 1.f;
	if (etam1 * etam1 < 1e-7f)
		return 0.f;
	if (weight != 1.f) {
		const float F0 = etam1 * etam1 / luxrays::Sqr(1.f + etaTI);
		const float eps = ((etam1 >= 0.f) ? 1.f : -1.f) *
				sqrtf(luxrays::Clamp(weight * F0, 0.f, 1.f));
		const float etaP = (1.f + eps) / luxrays::Max(1.f - eps, 1e-5f);
		if (etaP >= 1.f)
			return FresnelDielectric(cosI, etaP);
		// TIR possible: test against the *unmodified* eta, then evaluate the
		// squeezed curve at the refracted angle.
		const float mu2t = 1.f - (1.f - cosI * cosI) / (etaTI * etaTI);
		if (mu2t <= 0.f)
			return 1.f;
		return FresnelDielectric(sqrtf(mu2t), 1.f / etaP);
	}
	return FresnelDielectric(cosI, etaTI);
}

//------------------------------------------------------------------------------
// EON energy-preserving rough diffuse (Portsmouth, Kutz, Hill 2024)
//------------------------------------------------------------------------------

namespace eon {

constexpr float kC1 = 0.5f - 2.f / (3.f * M_PI);     // FON constant 1
constexpr float kC2 = 2.f / 3.f - 28.f / (15.f * M_PI); // FON constant 2

// FON directional albedo, exact closed form.
inline float EFonExact(float mu, const float r) {
	const float AF = 1.f / (1.f + kC1 * r);
	const float BF = r * AF;
	mu = luxrays::Clamp(mu, -1.f, 1.f);
	const float Si = sqrtf(1.f - mu * mu);
	const float G = Si * (acosf(mu) - Si * mu) +
			(2.f / 3.f) * (Si * mu * (1.f + Si + Si * Si) / (1.f + Si) - Si);
	return AF + BF * (1.f / M_PI) * G;
}

// FON directional albedo, fast polynomial approximation.
inline float EFonApprox(float mu, const float r) {
	const float mucomp = 1.f - mu;
	const float g1 = 0.0571085289f, g2 = 0.491881867f,
			g3 = -0.332181442f, g4 = 0.0714429953f;
	const float GoverPi = mucomp * (g1 + mucomp * (g2 + mucomp * (g3 + mucomp * g4)));
	return (1.f + r * GoverPi) / (1.f + kC1 * r);
}

// EON BRDF value (single + multi-scatter lobes). wi, wo point away from the
// surface, local frame. Returns f (without the cosine factor).
inline luxrays::Spectrum Eval(const luxrays::Spectrum &rho, const float r,
		const luxrays::Vector &wi, const luxrays::Vector &wo) {
	const float muI = wi.z, muO = wo.z;
	if (muI <= 1e-7f || muO <= 1e-7f)
		return luxrays::Spectrum(0.f);
	const float s = luxrays::Dot(wi, wo) - muI * muO;
	const float sovertF = (s > 0.f) ? s / luxrays::Max(muI, muO) : s;
	const float AF = 1.f / (1.f + kC1 * r);
	const luxrays::Spectrum fSS = (rho * (1.f / M_PI)) * AF * (1.f + r * sovertF);
	const float EFo = EFonApprox(muO, r);
	const float EFi = EFonApprox(muI, r);
	const float avgEF = AF * (1.f + kC2 * r);
	const luxrays::Spectrum rhoMS = (rho * rho) * avgEF /
			(luxrays::Spectrum(1.f) - rho * (1.f - avgEF));
	const float eps = 1e-7f;
	const luxrays::Spectrum fMS = (rhoMS * (1.f / M_PI)) *
			luxrays::Max(eps, 1.f - EFo) * luxrays::Max(eps, 1.f - EFi) /
			luxrays::Max(eps, 1.f - avgEF);
	return fSS + fMS;
}

// Directional albedo of the full EON BRDF.
inline luxrays::Spectrum DirAlbedo(const luxrays::Spectrum &rho, const float r,
		const float mu) {
	const float AF = 1.f / (1.f + kC1 * r);
	const float EF = EFonApprox(mu, r);
	const float avgEF = AF * (1.f + kC2 * r);
	const luxrays::Spectrum rhoMS = (rho * rho) * avgEF /
			(luxrays::Spectrum(1.f) - rho * (1.f - avgEF));
	return rho * EF + rhoMS * (1.f - EF);
}

// LTC coefficient fit for the CLTC sampling lobe.
inline void LtcCoeffs(const float mu, const float r,
		float &a, float &b, float &c, float &d) {
	a = 1.f + r * (0.303392f + (-0.518982f + 0.111709f * mu) * mu + (-0.276266f + 0.335918f * mu) * r);
	b = r * (-1.16407f + 1.15859f * mu + (0.150815f - 0.150105f * mu) * r) / (mu * mu * mu - 1.43545f);
	c = 1.f + r * (0.20013f + (-0.506373f + 0.261777f * mu) * mu);
	d = r * (0.540852f + (-1.01625f + 0.475392f * mu) * mu) / (-1.0743f + (0.0725628f + mu) * mu);
}

// PDF of the combined uniform+CLTC sampling strategy.
inline float Pdf(const luxrays::Vector &wo, const luxrays::Vector &wi, const float r) {
	const float mu = wo.z;
	const float Pu = powf(r, 0.1f) * (0.162925f + (-0.372058f + (0.538233f - 0.290822f * mu) * mu) * mu);
	const float Pc = 1.f - Pu;

	// wi to LTC space (basis built on wo)
	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	luxrays::Vector X = (lenSqr > 0.f) ?
			luxrays::Vector(wo.x, wo.y, 0.f) * (1.f / sqrtf(lenSqr)) :
			luxrays::Vector(1.f, 0.f, 0.f);
	luxrays::Vector Y(-X.y, X.x, 0.f);
	const luxrays::Vector wl(luxrays::Dot(wi, X), luxrays::Dot(wi, Y), wi.z);

	float a, b, c, d;
	LtcCoeffs(mu, r, a, b, c, d);
	const float detM = c * (a - b * d);
	const luxrays::Vector wh(c * (wl.x - b * wl.z), (a - b * d) * wl.y, -c * (d * wl.x - a * wl.z));
	const float lensq = luxrays::Dot(wh, wh);
	const float vz = 1.f / sqrtf(d * d + 1.f);
	const float s = 0.5f * (1.f + vz);
	const float pdfC = detM * detM / (lensq * lensq) * luxrays::Max(wh.z, 0.f) / (M_PI * s);

	const float pdfU = 1.f / (2.f * M_PI);
	return Pu * pdfU + Pc * pdfC;
}

// Sample EON: uniform lobe + CLTC lobe mixed by fitted probability.
// Returns sampled wi; *pdf filled with the strategy pdf.
inline luxrays::Vector Sample(const luxrays::Vector &wo, const float r,
		const float u0, const float u1, float &pdf) {
	const float mu = wo.z;
	const float Pu = powf(r, 0.1f) * (0.162925f + (-0.372058f + (0.538233f - 0.290822f * mu) * mu) * mu);
	const float Pc = 1.f - Pu;

	luxrays::Vector wi;
	if (u0 <= Pu) {
		// Uniform hemisphere lobe
		const float u = u0 / Pu;
		const float sinTheta = sqrtf(1.f - u * u);
		const float phi = 2.f * M_PI * u1;
		wi = luxrays::Vector(sinTheta * cosf(phi), sinTheta * sinf(phi), u);
		pdf = Pdf(wo, wi, r);
		return wi;
	}

	// CLTC lobe
	const float u = (u0 - Pu) / Pc;
	float a, b, c, d;
	LtcCoeffs(mu, r, a, b, c, d);
	const float R = sqrtf(u);
	const float phi = 2.f * M_PI * u1;
	float x = R * cosf(phi);
	const float y = R * sinf(phi);
	const float vz = 1.f / sqrtf(d * d + 1.f);
	const float s = 0.5f * (1.f + vz);
	x = -(s * x + (1.f - s) * sqrtf(luxrays::Max(1.f - y * y, 0.f)));
	const luxrays::Vector wh(x, y, sqrtf(luxrays::Max(1.f - x * x - y * y, 0.f)));
	const float pdfWh = wh.z / (M_PI * s);
	const luxrays::Vector wl(a * wh.x + b * wh.z, c * wh.y, d * wh.x + wh.z);
	const float len = wl.Length();
	const float detM = c * (a - b * d);
	const float pdfC = pdfWh * len * len * len / detM;
	// LTC-space -> local frame
	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	luxrays::Vector X = (lenSqr > 0.f) ?
			luxrays::Vector(wo.x, wo.y, 0.f) * (1.f / sqrtf(lenSqr)) :
			luxrays::Vector(1.f, 0.f, 0.f);
	luxrays::Vector Y(-X.y, X.x, 0.f);
	wi = luxrays::Normalize(X * wl.x + Y * wl.y + luxrays::Vector(0.f, 0.f, wl.z));
	pdf = Pu * (1.f / (2.f * M_PI)) + Pc * pdfC;
	return wi;
}

} // namespace eon

//------------------------------------------------------------------------------
// Zeltner 2022 SGGX-LTC fuzz/sheen lobe (OpenPBR fuzz model)
//------------------------------------------------------------------------------

namespace zeltner {

// Gaussian fit to the directional albedo E(mu, roughness) of the fuzz layer.
inline float DirAlbedo(const float mu, const float rough) {
	const float x = luxrays::Clamp(mu, 0.f, 1.f);
	const float y = luxrays::Clamp(rough, 0.f, 1.f);
	const float s = y * (0.0206607f + 1.58491f * y) / (0.0379424f + y * (1.32227f + y));
	const float m = y * (-0.193854f + y * (-1.14885f + y * (1.7932f - 0.95943f * y * y))) / (0.046391f + y);
	const float o = y * (0.000654023f + (-0.0207818f + 0.119681f * y) * y) / (1.26264f + y * (-1.92021f + y));
	const float t = (x - m) / s;
	return luxrays::Clamp(expf(-0.5f * t * t) / (s * sqrtf(2.f * M_PI)) + o, 0.f, 1.f);
}

inline float LtcAInv(const float mu, const float rough) {
	return (2.58126f * mu + 0.813703f * rough) * rough /
			(1.f + 0.310327f * mu * mu + 2.60994f * mu * rough);
}

inline float LtcBInv(const float mu, const float rough) {
	return sqrtf(1.f - mu) * (rough - 1.f) * rough * rough * rough /
			(0.0000254053f + 1.71228f * mu - 1.71506f * mu * rough + 1.34174f * rough * rough);
}

// Solid-angle PDF of wi under the fuzz LTC lobe given wo (local frame).
inline float Pdf(const luxrays::Vector &wo, const luxrays::Vector &wi, const float rough) {
	const float mu = luxrays::Clamp(wo.z, 0.f, 1.f);
	const float r = luxrays::Clamp(rough, 0.01f, 1.f);

	// wi to LTC space
	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	luxrays::Vector X = (lenSqr > 0.f) ?
			luxrays::Vector(wo.x, wo.y, 0.f) * (1.f / sqrtf(lenSqr)) :
			luxrays::Vector(1.f, 0.f, 0.f);
	luxrays::Vector Y(-X.y, X.x, 0.f);
	const luxrays::Vector w(luxrays::Dot(wi, X), luxrays::Dot(wi, Y), wi.z);

	const float aInv = LtcAInv(mu, r), bInv = LtcBInv(mu, r);
	const luxrays::Vector wl(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
	const float lsq = luxrays::Dot(wl, wl);
	if (lsq <= 0.f || wl.z <= 0.f)
		return 0.f;
	// D = Do(wl) * (aInv/||wl||^2)^2, Do = cosine pdf on the unnormalized M^-1 w
	return wl.z * (1.f / M_PI) * luxrays::Sqr(aInv / lsq);
}

// Sample the fuzz LTC lobe; returns wi, fills pdf.
inline luxrays::Vector Sample(const luxrays::Vector &wo, const float rough,
		const float u0, const float u1, float &pdf) {
	const float mu = luxrays::Clamp(wo.z, 0.f, 1.f);
	const float r = luxrays::Clamp(rough, 0.01f, 1.f);

	// cosine sample
	const float sinTheta = sqrtf(u0);
	const float phi = 2.f * M_PI * u1;
	luxrays::Vector wl(sinTheta * cosf(phi), sinTheta * sinf(phi),
			sqrtf(luxrays::Max(1.f - u0, 0.f)));

	const float aInv = LtcAInv(mu, r), bInv = LtcBInv(mu, r);
	luxrays::Vector w(wl.x / aInv - wl.z * bInv / aInv, wl.y / aInv, wl.z);
	const float lsq = luxrays::Dot(w, w);
	w *= 1.f / sqrtf(lsq);
	pdf = w.z * (1.f / M_PI) * luxrays::Sqr(aInv * lsq);

	// LTC space -> local frame
	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	luxrays::Vector X = (lenSqr > 0.f) ?
			luxrays::Vector(wo.x, wo.y, 0.f) * (1.f / sqrtf(lenSqr)) :
			luxrays::Vector(1.f, 0.f, 0.f);
	luxrays::Vector Y(-X.y, X.x, 0.f);
	return luxrays::Normalize(X * w.x + Y * w.y + luxrays::Vector(0.f, 0.f, w.z));
}

// muI * f_fuzz = E(muO) * D(wi|wo). f_fuzz itself = E * D / muI.
inline float EvalTimesCosI(const luxrays::Vector &wo, const luxrays::Vector &wi,
		const float rough) {
	if (wi.z <= 0.f)
		return 0.f;
	return DirAlbedo(wo.z, rough) * Pdf(wo, wi, rough);
}

} // namespace zeltner

// Dielectric Fresnel at cosI for relative IOR eta = n_t/n_i (incl. TIR).
// Compact form from the OpenPBR reference (Portsmouth).
inline float FresnelDielectric(const float cosI, const float eta) {
	const float c = fabsf(cosI);
	const float mut2 = eta * eta + c * c - 1.f;
	if (mut2 <= 0.f)
		return 1.f; // TIR
	const float g = sqrtf(mut2);
	const float gmc = g - c, gpc = g + c;
	return .5f * luxrays::Sqr(gmc / gpc) *
			(1.f + luxrays::Sqr((gpc * c - 1.f) / (gmc * c + 1.f)));
}

} // namespace slg

#endif /* _SLG_MICROFACET_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
