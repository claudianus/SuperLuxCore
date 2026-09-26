#line 2 "materialdefs_funcs_microfacet.cl"

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
// Shared microfacet / diffuse-lobe library (GPU twin of microfacet.h)
//
// References:
//   - GGX anisotropic NDF / Smith masking: Walter 2007, Heitz 2014
//   - VNDF sampling: Dupuy & Benyoub 2023 (spherical caps)
//   - Directional-albedo fits + multi-scatter compensation:
//     Kulla-Conty 2017 / Turquin 2019 (fits from the MaterialX implementation)
//   - EON rough diffuse: Portsmouth, Kutz, Hill 2024 (arXiv 2410.18026)
//   - F82 metal Fresnel: Kutz 2021 / OpenPBR spec
//   - Zeltner SGGX-LTC fuzz/sheen: Zeltner 2022 (MaterialX pbrlib impl.)
//------------------------------------------------------------------------------

// Anisotropic GGX NDF.
OPENCL_FORCE_INLINE float Microfacet_GgxD(const float3 wh,
		const float alphaX, const float alphaY) {
	const float hx = wh.x / alphaX, hy = wh.y / alphaY;
	const float d = hx * hx + hy * hy + wh.z * wh.z;
	return 1.f / (M_PI_F * alphaX * alphaY * d * d);
}

OPENCL_FORCE_INLINE float Microfacet_GgxLambda(const float3 w,
		const float alphaX, const float alphaY) {
	const float a2 = alphaX * alphaX * w.x * w.x + alphaY * alphaY * w.y * w.y;
	const float cos2 = w.z * w.z;
	if (cos2 <= 0.f)
		return 0.f;
	return (-1.f + sqrt(1.f + a2 / cos2)) * .5f;
}

OPENCL_FORCE_INLINE float Microfacet_GgxG1(const float3 w,
		const float alphaX, const float alphaY) {
	return 1.f / (1.f + Microfacet_GgxLambda(w, alphaX, alphaY));
}

// Height-correlated Smith masking-shadowing (Heitz 2014, eq. 99).
OPENCL_FORCE_INLINE float Microfacet_GgxG2(const float3 wi, const float3 wo,
		const float alphaX, const float alphaY) {
	return 1.f / (1.f + Microfacet_GgxLambda(wi, alphaX, alphaY) +
			Microfacet_GgxLambda(wo, alphaX, alphaY));
}

// VNDF sampling of the GGX distribution, Dupuy & Benyoub 2023 spherical caps.
// wo must be in the +Z hemisphere; returns wh.
OPENCL_FORCE_INLINE float3 Microfacet_GgxSampleVNDF(const float3 wo,
		const float alphaX, const float alphaY,
		const float u0, const float u1) {
	const float3 v = normalize(MAKE_FLOAT3(wo.x * alphaX, wo.y * alphaY, wo.z));

	const float phi = 2.f * M_PI_F * u0;
	const float z = (1.f - u1) * (1.f + v.z) - v.z;
	const float sinTheta = sqrt(clamp(1.f - z * z, 0.f, 1.f));
	const float3 c = MAKE_FLOAT3(sinTheta * cos(phi), sinTheta * sin(phi), z);

	const float3 h = c + v;
	return normalize(MAKE_FLOAT3(h.x * alphaX, h.y * alphaY, fmax(h.z, 0.f)));
}

// Solid-angle PDF of a half-vector sampled via the VNDF:
// p(wh) = D(wh) * G1(wo) * |wo.wh| / |wo.z|
OPENCL_FORCE_INLINE float Microfacet_GgxVNDFHalfPdf(const float3 wo,
		const float3 wh, const float alphaX, const float alphaY) {
	const float ndotV = fabs(wo.z);
	if (ndotV < 1e-6f)
		return 0.f;
	return Microfacet_GgxD(wh, alphaX, alphaY) * Microfacet_GgxG1(wo, alphaX, alphaY) *
			fabs(dot(wo, wh)) / ndotV;
}

// Solid-angle PDF of a reflection direction sampled via the VNDF.
OPENCL_FORCE_INLINE float Microfacet_GgxVNDFReflectionPdf(const float3 wo,
		const float3 wh, const float alphaX, const float alphaY) {
	const float ndotV = fabs(wo.z);
	if (ndotV < 1e-6f)
		return 0.f;
	return Microfacet_GgxD(wh, alphaX, alphaY) * Microfacet_GgxG1(wo, alphaX, alphaY) /
			(4.f * ndotV);
}

// OpenPBR anisotropy mapping: perceptual roughness + anisotropy -> alphas.
OPENCL_FORCE_INLINE void Microfacet_OpenPBRAnisoAlphas(const float roughness,
		const float anisotropy, __private float *alphaT, __private float *alphaB) {
	const float r2 = Sqr(clamp(roughness, 0.f, 1.f));
	const float a = clamp(anisotropy, 0.f, 1.f);
	const float at = r2 * sqrt(2.f / (1.f + Sqr(1.f - a)));
	*alphaT = fmax(at, 1e-4f);
	*alphaB = fmax((1.f - a) * at, 1e-4f);
}

// Rotate a direction in the local tangent plane by (cosA, sinA).
OPENCL_FORCE_INLINE float3 Microfacet_RotateXY(const float3 w,
		const float cosA, const float sinA) {
	return MAKE_FLOAT3(w.x * cosA - w.y * sinA, w.x * sinA + w.y * cosA, w.z);
}

// GGX directional-albedo fit pair (A, B): E = F0*A + F90*B.
OPENCL_FORCE_INLINE void Microfacet_GgxDirAlbedoAB(const float mu,
		const float alpha, __private float *A, __private float *B) {
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
	*A = clamp(r0 / r2, 0.f, 1.f);
	*B = clamp(r1 / r3, 0.f, 1.f);
}

OPENCL_FORCE_INLINE float Microfacet_GgxEss(const float mu, const float alpha) {
	float A, B;
	Microfacet_GgxDirAlbedoAB(mu, alpha, &A, &B);
	return A + B;
}

OPENCL_FORCE_INLINE float3 Microfacet_GgxDirAlbedo(const float mu,
		const float alpha, const float3 F0, const float3 F90) {
	float A, B;
	Microfacet_GgxDirAlbedoAB(mu, alpha, &A, &B);
	return F0 * A + F90 * B;
}

// Cosine-weighted hemisphere average of a Schlick Fresnel (exact).
OPENCL_FORCE_INLINE float3 Microfacet_GgxFresnelAverage(const float3 F0,
		const float3 F90) {
	return F0 + (F90 - F0) * (1.f / 21.f);
}

// Turquin multiple-scattering compensation: f_total ~= f_ss*(1+Favg*(1-Ess)/Ess)
OPENCL_FORCE_INLINE float3 Microfacet_GgxMSCompensation(const float mu,
		const float alpha, const float3 Favg) {
	const float Ess = Microfacet_GgxEss(mu, alpha);
	if (Ess <= 1e-4f)
		return WHITE;
	return WHITE + Favg * ((1.f - Ess) / Ess);
}

//------------------------------------------------------------------------------
// Height-tracking multi-bounce evaluation for Smith GGX conductors
// (Heitz et al. 2016, "Multiple-Scattering Microfacet BSDFs with the Smith
// Model"). GPU twin of GgxMSConductorEval in microfacet.h — see the CPU
// side for the algorithm description. Deterministic hash-seeded walks keep
// Evaluate a fixed function of (wStart, wTarget).
//------------------------------------------------------------------------------

#define GGX_MS_DEPTH 8
#define GGX_MS_WALKS 4

// Microflake cross-section for a direction v of any sign:
// |v.z|*(1+Lambda(v)) up, |v.z|*Lambda(v) down.
OPENCL_FORCE_INLINE float Microfacet_GgxMSSigma(const float3 v,
		const float alphaX, const float alphaY) {
	const float l = Microfacet_GgxLambda(v, alphaX, alphaY);
	return fabs(v.z) * ((v.z > 0.f) ? 1.f + l : l);
}

// Deterministic per-evaluation RNG seeded from the raw float bits of the
// input directions plus the shading point: Evaluate stays a fixed function of
// its inputs while different directions/points see independent walks.
OPENCL_FORCE_INLINE uint Microfacet_GgxMSSeed(const float3 a, const float3 b,
		const float3 p, const int walk) {
	uint s = as_uint(a.x) * 0x8da6b343u + as_uint(a.y) * 0xd8163841u +
			as_uint(a.z) * 0xcb1ab31fu + as_uint(b.x) * 0x9e3779b9u +
			as_uint(b.y) * 0x85ebca6bu + as_uint(b.z) * 0xc2b2ae35u +
			as_uint(p.x) * 0x165667b1u + as_uint(p.y) * 0xd3a2646cu +
			as_uint(p.z) * 0xfd7046c5u + (uint)walk * 0x27d4eb2fu;
	s ^= s >> 15; s *= 0x2c1b3c6du; s ^= s >> 12; s *= 0x297a2d39u; s ^= s >> 15;
	return s;
}

OPENCL_FORCE_INLINE float Microfacet_GgxMSRand(__private uint *s) {
	*s = (*s) * 1664525u + 1013904223u;
	return (float)((*s) >> 8) * (1.f / 16777216.f);
}

// Visible-normal sampler for an approach direction wi of any sign
// (GGX P22 slope-space form). GPU twin of GgxMSSampleVisibleNormal.
OPENCL_FORCE_INLINE float3 Microfacet_GgxMSSampleVisibleNormal(const float3 wi,
		const float alphaX, const float alphaY,
		const float r0, const float r1, const float r2) {
	const float3 v = normalize(MAKE_FLOAT3(wi.x * alphaX, wi.y * alphaY, wi.z));
	const float th = acos(clamp(v.z, -1.f, 1.f));
	float sx, sy;
	if (th < 1e-4f) {
		const float r = sqrt(r0 / fmax(1.f - r0, 1e-7f));
		const float ph = 2.f * M_PI_F * r1;
		sx = r * cos(ph);
		sy = r * sin(ph);
	} else {
		const float sn = sin(th), cs = cos(th), tn = sn / cs;
		const float sig = .5f * (cs + 1.f);
		if (sig < 1e-4f) {
			sx = 0.f;
			sy = 0.f;
		} else {
			const float c = 1.f / sig;
			const float A = 2.f * r0 / (cs * c) - 1.f;
			const float B = tn;
			const float tmp = 1.f / (A * A - 1.f);
			const float D = sqrt(fmax(0.f,
					B * B * tmp * tmp - (A * A - B * B) * tmp));
			const float s1 = B * tmp - D, s2 = B * tmp + D;
			sx = (A < 0.f || s2 > 1.f / tn) ? s1 : s2;
			sy = sqrt(fmax(0.f, -1.f - sx * sx +
					(1.f + sx * sx) / pow(fmax(1.f - r2, 1e-7f), 2.f / 3.f))) *
					sin(2.f * M_PI_F * r1);
		}
	}
	const float ph = atan2(v.y, v.x);
	const float cph = cos(ph), sph = sin(ph);
	float sxr = cph * sx - sph * sy, syr = sph * sx + cph * sy;
	sxr *= alphaX;
	syr *= alphaY;
	if (!isfinite(sxr) || !isfinite(syr))
		return (wi.z > 0.f) ? MAKE_FLOAT3(0.f, 0.f, 1.f) :
				normalize(MAKE_FLOAT3(wi.x, wi.y, 0.f));
	return normalize(MAKE_FLOAT3(-sxr, -syr, 1.f));
}

// Full Smith multi-bounce conductor BRDF (single + multiple scattering).
// Returns f * |cos(wTarget)|: pass lightDir as wTarget for the LuxCore
// f*|cos(lightDir)| convention.
OPENCL_FORCE_INLINE float3 Microfacet_GgxMSConductorEval(const float3 wStart,
		const float3 wTarget, const float3 p, const float alphaX,
		const float alphaY, const float3 eta, const float3 kappa) {
	if ((wStart.z <= 1e-5f) || (wTarget.z <= 1e-5f))
		return BLACK;

	const float lambdaOut = Microfacet_GgxLambda(wTarget, alphaX, alphaY);

	float3 result = BLACK;
	for (int k = 0; k < GGX_MS_WALKS; ++k) {
		uint seed = Microfacet_GgxMSSeed(wStart, wTarget, p, k);
		float3 wr = MAKE_FLOAT3(-wStart.x, -wStart.y, -wStart.z);
		float hr = 0.f;
		float3 weight = WHITE;

		for (int i = 0; i < GGX_MS_DEPTH; ++i) {
			const float st = Microfacet_GgxMSSigma(
					MAKE_FLOAT3(-wr.x, -wr.y, -wr.z), alphaX, alphaY);
			float h;
			if (st < 1e-5f) {
				if (wr.z >= 0.f)
					break;
				h = hr;
			} else {
				const float u = fmax(Microfacet_GgxMSRand(&seed), 1e-7f);
				h = fmin(0.f, hr) - log(u) * wr.z / st;
			}
			if (h >= 0.f)
				break;
			hr = h;

			const float3 m = Microfacet_GgxMSSampleVisibleNormal(
					MAKE_FLOAT3(-wr.x, -wr.y, -wr.z), alphaX, alphaY,
					Microfacet_GgxMSRand(&seed), Microfacet_GgxMSRand(&seed),
					Microfacet_GgxMSRand(&seed));

			const float3 v = MAKE_FLOAT3(-wr.x, -wr.y, -wr.z);
			const float3 hsum = v + wTarget;
			if (dot(hsum, hsum) > 1e-12f) {
				const float3 wh = normalize(hsum);
				// D(wh) is only defined on the upper hemisphere; the
				// wh.z <= 0 skip is required for energy conservation
				if ((wh.z > 0.f) && (dot(v, wh) > 1e-9f))
					result += weight *
							FresnelGeneral_Evaluate(eta, kappa, dot(v, wh)) *
							(Microfacet_GgxD(wh, alphaX, alphaY) *
									exp(h * lambdaOut) / (4.f * st));
			}

			const float wrm = dot(wr, m);
			weight *= FresnelGeneral_Evaluate(eta, kappa, fabs(wrm));
			wr = wr - m * (2.f * wrm);
		}
	}

	return result * (1.f / GGX_MS_WALKS);
}

//------------------------------------------------------------------------------
// GGX coating BSDF
//
// Drop-in GGX replacement for the SchlickBSDF_Coating* functions used by
// glossy2/glossycoating/glossytranslucent (opt-in "distribution=ggx"):
// anisotropic GGX NDF, height-correlated Smith G2, VNDF sampling.
// Same contract: *F returns f*|cos(sampledDir)|, *SampleF returns
// (f*cos)/pdf, *Pdf is the solid-angle pdf of the reflected direction.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 GgxBSDF_CoatingF(const float3 ks,
		const float alphaT, const float alphaB, const int multibounce,
		const float3 fixedDir, const float3 sampledDir) {
	const float coso = fabs(fixedDir.z);
	if (coso < 1e-6f)
		return BLACK;

	const float3 wh = normalize(fixedDir + sampledDir);
	const float3 S = FresnelSchlick_Evaluate(ks,
			clamp(fabs(dot(sampledDir, wh)), 0.f, 1.f));

	// f*cos(sampledDir) = D * G2 * F / (4 * |fixedDir.z|)
	float factor = Microfacet_GgxD(wh, alphaT, alphaB) *
			Microfacet_GgxG2(sampledDir, fixedDir, alphaT, alphaB) / (4.f * coso);
	if (multibounce) {
		const float alpha = .5f * (alphaT + alphaB);
		const float Favg = Spectrum_Filter(
				Microfacet_GgxFresnelAverage(ks, WHITE));
		const float Ess = Microfacet_GgxEss(coso, alpha);
		if (Ess > 1e-4f)
			factor *= 1.f + Favg * (1.f - Ess) / Ess;
	}
	return factor * S;
}

OPENCL_FORCE_INLINE float3 GgxBSDF_CoatingSampleF(const float3 ks,
		const float alphaT, const float alphaB, const int multibounce,
		const float3 fixedDir, float3 *sampledDir,
		const float u0, const float u1, float *pdf) {
	const float3 wh = Microfacet_GgxSampleVNDF(fixedDir, alphaT, alphaB, u0, u1);
	const float cosWH = dot(fixedDir, wh);
	*sampledDir = 2.f * cosWH * wh - fixedDir;

	if ((fabs((*sampledDir).z) < DEFAULT_COS_EPSILON_STATIC) ||
			(fixedDir.z * (*sampledDir).z < 0.f))
		return BLACK;

	*pdf = Microfacet_GgxVNDFReflectionPdf(fixedDir, wh, alphaT, alphaB);
	if (*pdf <= 0.f)
		return BLACK;

	return GgxBSDF_CoatingF(ks, alphaT, alphaB, multibounce,
			fixedDir, *sampledDir) / *pdf;
}

OPENCL_FORCE_INLINE float GgxBSDF_CoatingPdf(const float alphaT,
		const float alphaB, const float3 fixedDir, const float3 sampledDir) {
	const float3 wh = normalize(fixedDir + sampledDir);
	return Microfacet_GgxVNDFReflectionPdf(fixedDir, wh, alphaT, alphaB);
}

// Dielectric Fresnel at cosI for relative IOR eta = n_t/n_i (incl. TIR).
OPENCL_FORCE_INLINE float Microfacet_FresnelDielectric(const float cosI,
		const float eta) {
	const float c = fabs(cosI);
	const float mut2 = eta * eta + c * c - 1.f;
	if (mut2 <= 0.f)
		return 1.f; // TIR
	const float g = sqrt(mut2);
	const float gmc = g - c, gpc = g + c;
	return .5f * Sqr(gmc / gpc) *
			(1.f + Sqr((gpc * c - 1.f) / (gmc * c + 1.f)));
}

// OpenPBR specular-weight-modulated dielectric Fresnel (spec PR #247).
OPENCL_FORCE_INLINE float Microfacet_FresnelDielectricModulated(const float cosI,
		const float etaTI, const float weight) {
	const float etam1 = etaTI - 1.f;
	if (etam1 * etam1 < 1e-7f)
		return 0.f;
	// Total internal reflection is independent of specular_weight: beyond
	// the critical angle the interface reflects fully (sinI > n_t/n_i).
	if (etaTI < 1.f && (1.f - cosI * cosI) > etaTI * etaTI)
		return 1.f;
	if (weight != 1.f) {
		const float F0 = etam1 * etam1 / Sqr(1.f + etaTI);
		const float eps = ((etam1 >= 0.f) ? 1.f : -1.f) *
				sqrt(clamp(weight * F0, 0.f, 1.f));
		const float etaP = (1.f + eps) / fmax(1.f - eps, 1e-5f);
		if (etaP >= 1.f)
			return Microfacet_FresnelDielectric(cosI, etaP);
		const float mu2t = 1.f - (1.f - cosI * cosI) / (etaTI * etaTI);
		if (mu2t <= 0.f)
			return 1.f;
		return Microfacet_FresnelDielectric(sqrt(mu2t), 1.f / etaP);
	}
	return Microfacet_FresnelDielectric(cosI, etaTI);
}

// Average dielectric Fresnel over the hemisphere, d'Eon 2021 fit.
OPENCL_FORCE_INLINE float Microfacet_FresnelDielectricAverage(const float eta) {
	if (eta > 1.f) {
		const float e = clamp(eta, 1.0001f, 3.f);
		return log((10893.f * e - 1438.2f) / (-774.4f * e * e + 10212.f * e + 1.f));
	} else if (eta < 1.f)
		return 1.f - eta * eta * (1.f - Microfacet_FresnelDielectricAverage(1.f / eta));
	else
		return 0.f;
}

// OpenPBR F82-tint metallic Fresnel (clamped to [0,1], spec PR #256).
OPENCL_FORCE_INLINE float3 Microfacet_FresnelF82(const float mu,
		const float3 F0, const float3 edgeTint) {
	const float mubar = 1.f / 7.f;
	const float3 FSchlickBar = F0 + (WHITE - F0) * pow(1.f - mubar, 5.f);
	const float3 K = FSchlickBar * (WHITE - edgeTint) /
			(mubar * pow(1.f - mubar, 6.f));
	const float3 FSchlick = F0 + (WHITE - F0) * pow(1.f - mu, 5.f);
	return clamp(FSchlick - K * (mu * pow(1.f - mu, 6.f)), BLACK, WHITE);
}

// Exact complex-index conductor Fresnel (n + ik), PBRT formulation.
OPENCL_FORCE_INLINE float3 Microfacet_FresnelConductor(const float cosI,
		const float3 eta, const float3 k) {
	const float c = clamp(cosI, -1.f, 1.f);
	const float cos2 = c * c, sin2 = 1.f - cos2;
	float3 F;
	for (int i = 0; i < 3; ++i) {
		const float etai = (i == 0) ? eta.x : ((i == 1) ? eta.y : eta.z);
		const float ki = (i == 0) ? k.x : ((i == 1) ? k.y : k.z);
		const float eta2 = etai * etai, k2 = ki * ki;
		const float t0 = eta2 - k2 - sin2;
		const float a2b2 = sqrt(t0 * t0 + 4.f * eta2 * k2);
		const float t1 = a2b2 + cos2;
		const float a = sqrt(.5f * (a2b2 + t0));
		const float t2 = 2.f * c * a;
		const float rs = (t1 - t2) / (t1 + t2);
		const float t3 = cos2 * a2b2 + sin2 * sin2;
		const float t4 = t2 * sin2;
		const float rp = rs * (t3 - t4) / (t3 + t4);
		const float f = clamp(.5f * (rp + rs), 0.f, 1.f);
		if (i == 0) F.x = f; else if (i == 1) F.y = f; else F.z = f;
	}
	return F;
}

// Gulbrandsen 2014 (F0, edge-tint) -> (n, k) fit.
OPENCL_FORCE_INLINE void Microfacet_GulbrandsenNK(const float3 F0,
		const float3 edgeTint, __private float3 *n, __private float3 *k) {
	for (int i = 0; i < 3; ++i) {
		const float f0 = (i == 0) ? F0.x : ((i == 1) ? F0.y : F0.z);
		const float g = (i == 0) ? edgeTint.x : ((i == 1) ? edgeTint.y : edgeTint.z);
		const float r = clamp(f0, 0.f, .99f);
		const float rsqrt = sqrt(r);
		const float nMin = (1.f - r) / (1.f + r);
		const float nMax = (1.f + rsqrt) / (1.f - rsqrt);
		const float ni = Lerp(g, nMax, nMin);
		const float np1 = ni + 1.f, nm1 = ni - 1.f;
		const float k2 = fmax((np1 * np1 * r - nm1 * nm1) / (1.f - r), 0.f);
		if (i == 0) { n->x = ni; k->x = sqrt(k2); }
		else if (i == 1) { n->y = ni; k->y = sqrt(k2); }
		else { n->z = ni; k->z = sqrt(k2); }
	}
}

//------------------------------------------------------------------------------
// EON energy-preserving rough diffuse (Portsmouth, Kutz, Hill 2024)
//------------------------------------------------------------------------------

#define EON_C1 (0.5f - 2.f / (3.f * M_PI_F))
#define EON_C2 (2.f / 3.f - 28.f / (15.f * M_PI_F))

// FON directional albedo, fast polynomial approximation.
OPENCL_FORCE_INLINE float EON_EFonApprox(const float muIn, const float r) {
	const float mu = clamp(muIn, 0.f, 1.f);
	const float mucomp = 1.f - mu;
	const float GoverPi = mucomp * (0.0571085289f + mucomp * (0.491881867f +
			mucomp * (-0.332181442f + mucomp * 0.0714429953f)));
	return (1.f + r * GoverPi) / (1.f + EON_C1 * r);
}

// EON BRDF value (single + multi-scatter lobes), without the cosine factor.
OPENCL_FORCE_INLINE float3 EON_Eval(const float3 rho, const float r,
		const float3 wi, const float3 wo) {
	const float muI = wi.z, muO = wo.z;
	if (muI <= 1e-7f || muO <= 1e-7f)
		return BLACK;
	const float s = dot(wi, wo) - muI * muO;
	const float sovertF = (s > 0.f) ? s / fmax(muI, muO) : s;
	const float AF = 1.f / (1.f + EON_C1 * r);
	const float3 fSS = (rho * M_1_PI_F) * AF * (1.f + r * sovertF);
	const float EFo = EON_EFonApprox(muO, r);
	const float EFi = EON_EFonApprox(muI, r);
	const float avgEF = AF * (1.f + EON_C2 * r);
	const float3 rhoMS = (rho * rho) * avgEF / (WHITE - rho * (1.f - avgEF));
	const float eps = 1e-7f;
	const float3 fMS = (rhoMS * M_1_PI_F) *
			fmax(eps, 1.f - EFo) * fmax(eps, 1.f - EFi) /
			fmax(eps, 1.f - avgEF);
	return fSS + fMS;
}

// Directional albedo of the full EON BRDF.
OPENCL_FORCE_INLINE float3 EON_DirAlbedo(const float3 rho, const float r,
		const float mu) {
	const float AF = 1.f / (1.f + EON_C1 * r);
	const float EF = EON_EFonApprox(mu, r);
	const float avgEF = AF * (1.f + EON_C2 * r);
	const float3 rhoMS = (rho * rho) * avgEF / (WHITE - rho * (1.f - avgEF));
	return rho * EF + rhoMS * (1.f - EF);
}

// LTC coefficient fit for the CLTC sampling lobe.
OPENCL_FORCE_INLINE void EON_LtcCoeffs(const float mu, const float r,
		__private float *a, __private float *b, __private float *c, __private float *d) {
	*a = 1.f + r * (0.303392f + (-0.518982f + 0.111709f * mu) * mu + (-0.276266f + 0.335918f * mu) * r);
	*b = r * (-1.16407f + 1.15859f * mu + (0.150815f - 0.150105f * mu) * r) / (mu * mu * mu - 1.43545f);
	*c = 1.f + r * (0.20013f + (-0.506373f + 0.261777f * mu) * mu);
	*d = r * (0.540852f + (-1.01625f + 0.475392f * mu) * mu) / (-1.0743f + (0.0725628f + mu) * mu);
}

// PDF of the combined uniform+CLTC sampling strategy.
OPENCL_FORCE_INLINE float EON_Pdf(const float3 wo, const float3 wi,
		const float r) {
	const float mu = wo.z;
	const float Pu = pow(r, 0.1f) * (0.162925f + (-0.372058f + (0.538233f - 0.290822f * mu) * mu) * mu);
	const float Pc = 1.f - Pu;

	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	const float3 X = (lenSqr > 0.f) ?
			MAKE_FLOAT3(wo.x / sqrt(lenSqr), wo.y / sqrt(lenSqr), 0.f) :
			MAKE_FLOAT3(1.f, 0.f, 0.f);
	const float3 Y = MAKE_FLOAT3(-X.y, X.x, 0.f);
	const float3 wl = MAKE_FLOAT3(dot(wi, X), dot(wi, Y), wi.z);

	float a, b, c, d;
	EON_LtcCoeffs(mu, r, &a, &b, &c, &d);
	const float detM = c * (a - b * d);
	const float3 wh = MAKE_FLOAT3(c * (wl.x - b * wl.z), (a - b * d) * wl.y,
			-c * (d * wl.x - a * wl.z));
	const float lensq = dot(wh, wh);
	const float vz = 1.f / sqrt(d * d + 1.f);
	const float s = 0.5f * (1.f + vz);
	const float pdfC = detM * detM / (lensq * lensq) * fmax(wh.z, 0.f) / (M_PI_F * s);

	return Pu * (0.5f * M_1_PI_F) + Pc * pdfC;
}

// Sample EON: uniform + CLTC lobes mixed by fitted probability.
OPENCL_FORCE_INLINE float3 EON_Sample(const float3 wo, const float r,
		const float u0, const float u1, __private float *pdf) {
	const float mu = wo.z;
	const float Pu = pow(r, 0.1f) * (0.162925f + (-0.372058f + (0.538233f - 0.290822f * mu) * mu) * mu);
	const float Pc = 1.f - Pu;

	float3 wi;
	if (u0 <= Pu) {
		// Uniform hemisphere lobe
		const float u = u0 / Pu;
		const float sinTheta = sqrt(1.f - u * u);
		const float phi = 2.f * M_PI_F * u1;
		wi = MAKE_FLOAT3(sinTheta * cos(phi), sinTheta * sin(phi), u);
		*pdf = EON_Pdf(wo, wi, r);
		return wi;
	}

	// CLTC lobe
	const float u = (u0 - Pu) / Pc;
	float a, b, c, d;
	EON_LtcCoeffs(mu, r, &a, &b, &c, &d);
	const float R = sqrt(u);
	const float phi = 2.f * M_PI_F * u1;
	float x = R * cos(phi);
	const float y = R * sin(phi);
	const float vz = 1.f / sqrt(d * d + 1.f);
	const float s = 0.5f * (1.f + vz);
	x = -(s * x + (1.f - s) * sqrt(fmax(1.f - y * y, 0.f)));
	const float3 wh = MAKE_FLOAT3(x, y, sqrt(fmax(1.f - x * x - y * y, 0.f)));
	const float pdfWh = wh.z / (M_PI_F * s);
	const float3 wl = MAKE_FLOAT3(a * wh.x + b * wh.z, c * wh.y, d * wh.x + wh.z);
	const float len = length(wl);
	const float detM = c * (a - b * d);
	const float pdfC = pdfWh * len * len * len / detM;
	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	const float3 X = (lenSqr > 0.f) ?
			MAKE_FLOAT3(wo.x / sqrt(lenSqr), wo.y / sqrt(lenSqr), 0.f) :
			MAKE_FLOAT3(1.f, 0.f, 0.f);
	const float3 Y = MAKE_FLOAT3(-X.y, X.x, 0.f);
	wi = normalize(X * wl.x + Y * wl.y + MAKE_FLOAT3(0.f, 0.f, wl.z));
	*pdf = Pu * (0.5f * M_1_PI_F) + Pc * pdfC;
	return wi;
}

//------------------------------------------------------------------------------
// Zeltner 2022 SGGX-LTC fuzz/sheen lobe (OpenPBR fuzz model)
//------------------------------------------------------------------------------

// Gaussian fit to the directional albedo E(mu, roughness).
OPENCL_FORCE_INLINE float Zeltner_DirAlbedo(const float muIn, const float roughIn) {
	const float x = clamp(muIn, 0.f, 1.f);
	const float y = clamp(roughIn, 0.f, 1.f);
	const float s = y * (0.0206607f + 1.58491f * y) / (0.0379424f + y * (1.32227f + y));
	const float m = y * (-0.193854f + y * (-1.14885f + y * (1.7932f - 0.95943f * y * y))) / (0.046391f + y);
	const float o = y * (0.000654023f + (-0.0207818f + 0.119681f * y) * y) / (1.26264f + y * (-1.92021f + y));
	const float t = (x - m) / s;
	return clamp(exp(-0.5f * t * t) / (s * sqrt(2.f * M_PI_F)) + o, 0.f, 1.f);
}

OPENCL_FORCE_INLINE float Zeltner_LtcAInv(const float mu, const float rough) {
	return (2.58126f * mu + 0.813703f * rough) * rough /
			(1.f + 0.310327f * mu * mu + 2.60994f * mu * rough);
}

OPENCL_FORCE_INLINE float Zeltner_LtcBInv(const float mu, const float rough) {
	return sqrt(1.f - mu) * (rough - 1.f) * rough * rough * rough /
			(0.0000254053f + 1.71228f * mu - 1.71506f * mu * rough + 1.34174f * rough * rough);
}

// Solid-angle PDF of wi under the fuzz LTC lobe given wo (local frame).
OPENCL_FORCE_INLINE float Zeltner_Pdf(const float3 wo, const float3 wi,
		const float rough) {
	const float mu = clamp(wo.z, 0.f, 1.f);
	const float r = clamp(rough, 0.01f, 1.f);

	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	const float3 X = (lenSqr > 0.f) ?
			MAKE_FLOAT3(wo.x / sqrt(lenSqr), wo.y / sqrt(lenSqr), 0.f) :
			MAKE_FLOAT3(1.f, 0.f, 0.f);
	const float3 Y = MAKE_FLOAT3(-X.y, X.x, 0.f);
	const float3 w = MAKE_FLOAT3(dot(wi, X), dot(wi, Y), wi.z);

	const float aInv = Zeltner_LtcAInv(mu, r), bInv = Zeltner_LtcBInv(mu, r);
	const float3 wl = MAKE_FLOAT3(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
	const float lsq = dot(wl, wl);
	if (lsq <= 0.f || wl.z <= 0.f)
		return 0.f;
	return wl.z * M_1_PI_F * Sqr(aInv / lsq);
}

// Sample the fuzz LTC lobe; returns wi, fills pdf.
OPENCL_FORCE_INLINE float3 Zeltner_Sample(const float3 wo, const float rough,
		const float u0, const float u1, __private float *pdf) {
	const float mu = clamp(wo.z, 0.f, 1.f);
	const float r = clamp(rough, 0.01f, 1.f);

	const float sinTheta = sqrt(u0);
	const float phi = 2.f * M_PI_F * u1;
	const float3 wl = MAKE_FLOAT3(sinTheta * cos(phi), sinTheta * sin(phi),
			sqrt(fmax(1.f - u0, 0.f)));

	const float aInv = Zeltner_LtcAInv(mu, r), bInv = Zeltner_LtcBInv(mu, r);
	float3 w = MAKE_FLOAT3(wl.x / aInv - wl.z * bInv / aInv, wl.y / aInv, wl.z);
	const float lsq = dot(w, w);
	w /= sqrt(lsq);
	*pdf = w.z * M_1_PI_F * Sqr(aInv * lsq);

	const float lenSqr = wo.x * wo.x + wo.y * wo.y;
	const float3 X = (lenSqr > 0.f) ?
			MAKE_FLOAT3(wo.x / sqrt(lenSqr), wo.y / sqrt(lenSqr), 0.f) :
			MAKE_FLOAT3(1.f, 0.f, 0.f);
	const float3 Y = MAKE_FLOAT3(-X.y, X.x, 0.f);
	return normalize(X * w.x + Y * w.y + MAKE_FLOAT3(0.f, 0.f, w.z));
}

// muI * f_fuzz = E(muO) * D(wi|wo); f_fuzz = E * D / muI.
OPENCL_FORCE_INLINE float Zeltner_EvalTimesCosI(const float3 wo, const float3 wi,
		const float rough) {
	if (wi.z <= 0.f)
		return 0.f;
	return Zeltner_DirAlbedo(wo.z, rough) * Zeltner_Pdf(wo, wi, rough);
}
