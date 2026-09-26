#line 2 "materialdefs_funcs_hair.cl"

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
// Hair material (Chiang et al. 2019, ported from pbrt-v3 HairBSDF)
// Mirror of src/slg/materials/hairmat.cpp for the device path.
//------------------------------------------------------------------------------

#define HAIR_PMAX 3
#define HAIR_SQRTPIOVER8 0.626657069f
// Vertex AOV layers carrying the object-space strand tangent (must match
// HAIR_TANGENT_*_DATA_INDEX in slg/shapes/strands.h)
#define HAIR_TANGENT_X_DATA_INDEX 4
#define HAIR_TANGENT_Y_DATA_INDEX 5
#define HAIR_TANGENT_Z_DATA_INDEX 6

OPENCL_FORCE_INLINE float Hair_SafeSqrt(const float x) { return sqrt(fmax(0.f, x)); }
OPENCL_FORCE_INLINE float Hair_SafeASin(const float x) { return asin(clamp(x, -1.f, 1.f)); }
OPENCL_FORCE_INLINE float Hair_Fract(const float x) { return x - floor(x); }

OPENCL_FORCE_INLINE float Hair_I0(const float x) {
	float val = 0.f;
	float x2i = 1.f;
	int ifact = 1;
	int i4 = 1;
	for (int i = 0; i < 10; ++i) {
		if (i > 1) ifact *= i;
		val += x2i / (i4 * ifact * ifact);
		x2i *= x * x;
		i4 *= 4;
	}
	return val;
}

OPENCL_FORCE_INLINE float Hair_LogI0(const float x) {
	if (x > 12.f)
		return x + 0.5f * (-log(2.f * M_PI_F) + log(1.f / x) + 1.f / (8.f * x));
	else
		return log(Hair_I0(x));
}

OPENCL_FORCE_INLINE float Hair_Mp(const float cosThetaI, const float cosThetaO,
		const float sinThetaI, const float sinThetaO, const float v) {
	const float a = cosThetaI * cosThetaO / v;
	const float b = sinThetaI * sinThetaO / v;
	return (v <= .1f)
		? exp(Hair_LogI0(a) - b - 1.f / v + 0.6931f + log(1.f / (2.f * v)))
		: (exp(-b) * Hair_I0(a)) / (sinh(1.f / v) * 2.f * v);
}

OPENCL_FORCE_INLINE float Hair_Phi(const int p, const float gammaO, const float gammaT) {
	return 2.f * p * gammaT - 2.f * gammaO + p * M_PI_F;
}

OPENCL_FORCE_INLINE float Hair_Logistic(float x, const float s) {
	x = fabs(x);
	return exp(-x / s) / (s * (1.f + exp(-x / s)) * (1.f + exp(-x / s)));
}

OPENCL_FORCE_INLINE float Hair_LogisticCDF(const float x, const float s) {
	return 1.f / (1.f + exp(-x / s));
}

OPENCL_FORCE_INLINE float Hair_TrimmedLogistic(const float x, const float s,
		const float a, const float b) {
	return Hair_Logistic(x, s) / (Hair_LogisticCDF(b, s) - Hair_LogisticCDF(a, s));
}

OPENCL_FORCE_INLINE float Hair_Np(float phi, const int p, const float s,
		const float gammaO, const float gammaT) {
	float dphi = phi - Hair_Phi(p, gammaO, gammaT);
	// Remap dphi to [-pi, pi]
	while (dphi > M_PI_F) dphi -= 2.f * M_PI_F;
	while (dphi < -M_PI_F) dphi += 2.f * M_PI_F;
	return Hair_TrimmedLogistic(dphi, s, -M_PI_F, M_PI_F);
}

OPENCL_FORCE_INLINE float Hair_SampleTrimmedLogistic(const float u, const float s,
		const float a, const float b) {
	const float k = Hair_LogisticCDF(b, s) - Hair_LogisticCDF(a, s);
	const float x = -s * log(1.f / (u * k + Hair_LogisticCDF(a, s)) - 1.f);
	return clamp(x, a, b);
}

OPENCL_FORCE_INLINE void Hair_Ap(const float cosThetaO, const float eta, const float h,
		const float3 T, __private float3 *ap) {
	const float cosGammaO = Hair_SafeSqrt(1.f - h * h);
	const float cosTheta = cosThetaO * cosGammaO;
	const float f = FresnelCauchy_Evaluate(eta, cosTheta);
	ap[0] = f;

	ap[1] = (1.f - f) * (1.f - f) * T;

	for (int p = 2; p < HAIR_PMAX; ++p)
		ap[p] = ap[p - 1] * T * f;

	ap[HAIR_PMAX] = ap[HAIR_PMAX - 1] * f * T /
			(MAKE_FLOAT3(1.f, 1.f, 1.f) - T * f);
}

OPENCL_FORCE_INLINE void Hair_TiltThetaO(const int p,
		__private const float *sin2k, __private const float *cos2k,
		const float sinThetaO, const float cosThetaO,
		__private float *sinThetaOp, __private float *cosThetaOp) {
	if (p == 0) {
		*sinThetaOp = sinThetaO * cos2k[1] - cosThetaO * sin2k[1];
		*cosThetaOp = cosThetaO * cos2k[1] + sinThetaO * sin2k[1];
	} else if (p == 1) {
		*sinThetaOp = sinThetaO * cos2k[0] + cosThetaO * sin2k[0];
		*cosThetaOp = cosThetaO * cos2k[0] - sinThetaO * sin2k[0];
	} else if (p == 2) {
		*sinThetaOp = sinThetaO * cos2k[2] + cosThetaO * sin2k[2];
		*cosThetaOp = cosThetaO * cos2k[2] - sinThetaO * sin2k[2];
	} else {
		*sinThetaOp = sinThetaO;
		*cosThetaOp = cosThetaO;
	}
	*cosThetaOp = fabs(*cosThetaOp);
}

OPENCL_FORCE_INLINE void Hair_DemuxFloat(const float u, __private float *a, __private float *b) {
	*a = Hair_Fract(u * 65536.f);
	*b = Hair_Fract(u * 4294967296.f);
}

// Per-hit hair state
typedef struct {
	float3 tangent;   // strand tangent in the local shading frame
	float gammaO;
	float eta;
	float3 sigma_a;
	float v[4];
	float s;
	float sin2kAlpha[3];
	float cos2kAlpha[3];

	// Huang model state (only filled when material->hair.model == 1)
	float3 frameX, frameY, frameZ; // Huang local frame (Y = tangent)
	float3 wi;        // fixed dir in the Huang frame
	float hDivR;      // hit offset over projected radius [-1,1]
	float radius;     // projected radius from the view dir
	float rough;      // GGX alpha (= artist roughness)
	float tilt;       // cuticle tilt, radians
	float aspect;     // minor/major axis ratio b
} HairContext;

// Hair frame basis: e1 = radial (shading normal projected perp. to the
// tangent), e2 = cross(tangent, e1). Local shading normal is +Z.
OPENCL_FORCE_INLINE void Hair_Frame(const float3 T, __private float3 *e1, __private float3 *e2) {
	float3 nPerp = MAKE_FLOAT3(0.f, 0.f, 1.f) - T * T.z;
	if (dot(nPerp, nPerp) < 1e-12f)
		nPerp = MAKE_FLOAT3(1.f, 0.f, 0.f) - T * T.x;
	*e1 = normalize(nPerp);
	*e2 = cross(T, *e1);
}

OPENCL_FORCE_INLINE float3 Hair_EvalSigmaA(__global const Material* restrict material,
		__global const HitPoint *hitPoint, const float betaN
		MATERIALS_PARAM_DECL) {
	if (material->hair.sigmaATexIndex != NULL_INDEX)
		return fmax(Texture_GetSpectrumValue(material->hair.sigmaATexIndex,
				hitPoint TEXTURES_PARAM), BLACK);

	if (material->hair.colorTexIndex != NULL_INDEX) {
		const float3 c = Spectrum_Clamp(Texture_GetSpectrumValue(material->hair.colorTexIndex,
				hitPoint TEXTURES_PARAM));
		// SigmaAFromReflectance (Chiang 2019)
		const float denom = 5.969f - 0.215f * betaN + 2.532f * betaN * betaN -
				10.73f * betaN * betaN * betaN + 5.574f * pow(betaN, 4.f) +
				0.245f * pow(betaN, 5.f);
		float3 sa;
		sa.x = log(fmax(c.x, 1e-5f)) / denom; sa.x *= sa.x;
		sa.y = log(fmax(c.y, 1e-5f)) / denom; sa.y *= sa.y;
		sa.z = log(fmax(c.z, 1e-5f)) / denom; sa.z *= sa.z;
		return sa;
	}

	// Melanin concentration model (default: brown hair 1.3 / 0)
	const float ce = (material->hair.eumelaninTexIndex != NULL_INDEX) ?
			fmax(0.f, Texture_GetFloatValue(material->hair.eumelaninTexIndex,
			hitPoint TEXTURES_PARAM)) : 1.3f;
	const float cp = (material->hair.pheomelaninTexIndex != NULL_INDEX) ?
			fmax(0.f, Texture_GetFloatValue(material->hair.pheomelaninTexIndex,
			hitPoint TEXTURES_PARAM)) : 0.f;
	return MAKE_FLOAT3(
		ce * 0.419f + cp * 0.187f,
		ce * 0.697f + cp * 0.4f,
		ce * 1.37f + cp * 1.05f);
}

OPENCL_FORCE_INLINE bool Hair_SetupContext(__global const Material* restrict material,
		__global const HitPoint *hitPoint, const float3 localEyeDir,
		__private HairContext *ctx
		MATERIALS_PARAM_DECL) {
	// Strand tangent: vertex AOV (object space) -> world -> local frame
	float3 tObj = MAKE_FLOAT3(
			HitPoint_GetVertexAOV(hitPoint, HAIR_TANGENT_X_DATA_INDEX EXTMESH_PARAM),
			HitPoint_GetVertexAOV(hitPoint, HAIR_TANGENT_Y_DATA_INDEX EXTMESH_PARAM),
			HitPoint_GetVertexAOV(hitPoint, HAIR_TANGENT_Z_DATA_INDEX EXTMESH_PARAM));
	if (dot(tObj, tObj) < 1e-12f) {
		// No strand data: fall back to dpdv (fiber direction for
		// strand-like UV layouts)
		tObj = VLOAD3F(&hitPoint->dpdv.x);
	}
	const float3 tWorld = normalize(Transform_ApplyVector(&hitPoint->localToWorld, tObj));
	Frame frame;
	Frame_Set_Private(&frame, VLOAD3F(&hitPoint->dpdu.x),
			VLOAD3F(&hitPoint->dpdv.x), VLOAD3F(&hitPoint->shadeN.x));
	ctx->tangent = normalize(Frame_ToLocal_Private(&frame, tWorld));

	const float bm = clamp(Texture_GetFloatValue(material->hair.betaMTexIndex,
			hitPoint TEXTURES_PARAM), 1e-2f, 1.f);
	const float bn = clamp(Texture_GetFloatValue(material->hair.betaNTexIndex,
			hitPoint TEXTURES_PARAM), 1e-2f, 1.f);
	const float a = Texture_GetFloatValue(material->hair.alphaTexIndex,
			hitPoint TEXTURES_PARAM);
	ctx->eta = Texture_GetFloatValue(material->hair.etaTexIndex,
			hitPoint TEXTURES_PARAM);
	ctx->sigma_a = Hair_EvalSigmaA(material, hitPoint, bn MATERIALS_PARAM);

	if (material->hair.model == 1) {
		// Huang'22: cuticle tilt in radians (negated, Principled-Hair
		// convention), GGX alpha = artist roughness, elliptical axis b.
		ctx->tilt = -a * M_PI_F / 180.f;
		ctx->rough = clamp(Texture_GetFloatValue(material->hair.roughnessTexIndex,
				hitPoint TEXTURES_PARAM), 0.001f, 1.f);
		ctx->aspect = clamp(Texture_GetFloatValue(material->hair.aspectRatioTexIndex,
				hitPoint TEXTURES_PARAM), 0.1f, 1.f);
		const float b = ctx->aspect;

		// Azimuthal hit offset: cosine between the shading normal (+Z) and
		// the direction perpendicular to both tangent and ray.
		const float3 xRay = cross(ctx->tangent, localEyeDir);
		const float xRayLen = length(xRay);

		float3 X;
		if (b == 1.f) {
			X = (xRayLen >= 1e-8f) ? xRay / xRayLen : MAKE_FLOAT3(1.f, 0.f, 0.f);
		} else {
			float3 nPerp = MAKE_FLOAT3(0.f, 0.f, 1.f) - ctx->tangent * ctx->tangent.z;
			X = (dot(nPerp, nPerp) < 1e-12f) ?
					MAKE_FLOAT3(1.f, 0.f, 0.f) : normalize(nPerp);
		}

		if (xRayLen < 1e-8f) {
			ctx->hDivR = 0.f;
			ctx->radius = 1.f;
		} else {
			const float h = -xRay.z / xRayLen;
			const float e2 = 1.f - b * b;
			// Projected radius in the *ellipse* frame (X = major axis)
			const float wiX = dot(localEyeDir, X);
			const float wiZ = dot(localEyeDir, cross(X, ctx->tangent));
			ctx->radius = (e2 == 0.f) ? 1.f :
					sqrt(1.f - e2 * wiX * wiX / (wiX * wiX + wiZ * wiZ));
			ctx->hDivR = h / ctx->radius;
		}
		if (fabs(ctx->hDivR) >= 1.f)
			return false;

		ctx->frameX = X;
		ctx->frameY = ctx->tangent;
		ctx->frameZ = cross(ctx->frameX, ctx->frameY);
		ctx->wi = MAKE_FLOAT3(dot(localEyeDir, ctx->frameX),
				dot(localEyeDir, ctx->frameY), dot(localEyeDir, ctx->frameZ));
		return true;
	}

	// Longitudinal variance from beta_m
	ctx->v[0] = (0.726f * bm + 0.812f * bm * bm + 3.7f * pow(bm, 20.f));
	ctx->v[0] *= ctx->v[0];
	ctx->v[1] = .25f * ctx->v[0];
	ctx->v[2] = 4.f * ctx->v[0];
	ctx->v[3] = ctx->v[2];

	// Azimuthal logistic scale from beta_n
	ctx->s = HAIR_SQRTPIOVER8 *
			(0.265f * bn + 1.194f * bn * bn + 5.372f * pow(bn, 22.f));

	// Scale tilt (alpha) terms
	ctx->sin2kAlpha[0] = sin(a * M_PI_F / 180.f);
	ctx->cos2kAlpha[0] = Hair_SafeSqrt(1.f - ctx->sin2kAlpha[0] * ctx->sin2kAlpha[0]);
	for (int i = 1; i < 3; ++i) {
		ctx->sin2kAlpha[i] = 2.f * ctx->cos2kAlpha[i - 1] * ctx->sin2kAlpha[i - 1];
		ctx->cos2kAlpha[i] = ctx->cos2kAlpha[i - 1] * ctx->cos2kAlpha[i - 1] -
				ctx->sin2kAlpha[i - 1] * ctx->sin2kAlpha[i - 1];
	}

	// Azimuthal offset h from the hit radial direction vs. the outgoing
	// ray azimuth plane
	float3 e1, e2;
	Hair_Frame(ctx->tangent, &e1, &e2);
	float3 e1wo = localEyeDir - ctx->tangent * dot(localEyeDir, ctx->tangent);
	if (dot(e1wo, e1wo) < 1e-12f)
		e1wo = e1; // outgoing ray parallel to the fiber: h = 0
	else
		e1wo = normalize(e1wo);
	const float3 e2wo = cross(ctx->tangent, e1wo);
	const float h = clamp(dot(e1, e2wo), -1.f, 1.f);
	ctx->gammaO = Hair_SafeASin(h);

	return true;
}

OPENCL_FORCE_INLINE float3 Hair_f(__global const HitPoint *hitPoint,
		__private const HairContext *ctx, const float3 localLightDir, const float3 localEyeDir) {
	const float3 T = ctx->tangent;
	float3 e1, e2;
	Hair_Frame(T, &e1, &e2);

	const float sinThetaO = dot(localEyeDir, T);
	const float cosThetaO = Hair_SafeSqrt(1.f - sinThetaO * sinThetaO);
	const float phiO = atan2(dot(localEyeDir, e2), dot(localEyeDir, e1));

	const float sinThetaI = dot(localLightDir, T);
	const float cosThetaI = Hair_SafeSqrt(1.f - sinThetaI * sinThetaI);
	const float phiI = atan2(dot(localLightDir, e2), dot(localLightDir, e1));

	const float sinThetaT = sinThetaO / ctx->eta;
	const float cosThetaT = Hair_SafeSqrt(1.f - sinThetaT * sinThetaT);

	const float h = sin(ctx->gammaO);
	const float etap = sqrt(ctx->eta * ctx->eta - sinThetaO * sinThetaO) / cosThetaO;
	const float sinGammaT = h / etap;
	const float cosGammaT = Hair_SafeSqrt(1.f - sinGammaT * sinGammaT);
	const float gammaT = Hair_SafeASin(sinGammaT);

	const float3 Tt = Spectrum_Exp(ctx->sigma_a * (-2.f * cosGammaT / cosThetaT));

	const float phi = phiI - phiO;
	float3 ap[HAIR_PMAX + 1];
	Hair_Ap(cosThetaO, ctx->eta, h, Tt, ap);

	float3 fsum = BLACK;
	for (int p = 0; p < HAIR_PMAX; ++p) {
		float sinThetaOp, cosThetaOp;
		Hair_TiltThetaO(p, ctx->sin2kAlpha, ctx->cos2kAlpha,
				sinThetaO, cosThetaO, &sinThetaOp, &cosThetaOp);
		fsum += Hair_Mp(cosThetaI, cosThetaOp, sinThetaI, sinThetaOp, ctx->v[p]) *
				ap[p] * Hair_Np(phi, p, ctx->s, ctx->gammaO, gammaT);
	}
	fsum += Hair_Mp(cosThetaI, cosThetaO, sinThetaI, sinThetaO, ctx->v[HAIR_PMAX]) *
			ap[HAIR_PMAX] * (1.f / (2.f * M_PI_F));
	// LuxCore wants f * cos: pbrt's BSDF value is fsum/|cosThetaI| so the
	// cosine-multiplied value is exactly fsum.
	return fsum;
}

OPENCL_FORCE_INLINE void Hair_ComputeApPdf(__private const HairContext *ctx,
		const float cosThetaO, __private float *apPdf) {
	const float sinThetaO = Hair_SafeSqrt(1.f - cosThetaO * cosThetaO);
	const float sinThetaT = sinThetaO / ctx->eta;
	const float cosThetaT = Hair_SafeSqrt(1.f - sinThetaT * sinThetaT);

	const float h = sin(ctx->gammaO);
	const float etap = sqrt(ctx->eta * ctx->eta - sinThetaO * sinThetaO) / cosThetaO;
	const float sinGammaT = h / etap;
	const float cosGammaT = Hair_SafeSqrt(1.f - sinGammaT * sinGammaT);

	const float3 T = Spectrum_Exp(ctx->sigma_a * (-2.f * cosGammaT / cosThetaT));
	float3 ap[HAIR_PMAX + 1];
	Hair_Ap(cosThetaO, ctx->eta, h, T, ap);

	float sumY = 0.f;
	for (int i = 0; i <= HAIR_PMAX; ++i)
		sumY += Spectrum_Y(ap[i]);
	for (int i = 0; i <= HAIR_PMAX; ++i)
		apPdf[i] = (sumY > 0.f) ? (Spectrum_Y(ap[i]) / sumY) : 0.f;
}

OPENCL_FORCE_INLINE float Hair_EvalPdf(__global const HitPoint *hitPoint,
		__private const HairContext *ctx, const float3 localLightDir, const float3 localEyeDir) {
	const float3 T = ctx->tangent;
	float3 e1, e2;
	Hair_Frame(T, &e1, &e2);

	const float sinThetaO = dot(localEyeDir, T);
	const float cosThetaO = Hair_SafeSqrt(1.f - sinThetaO * sinThetaO);
	const float sinThetaI = dot(localLightDir, T);
	const float cosThetaI = Hair_SafeSqrt(1.f - sinThetaI * sinThetaI);
	const float phiO = atan2(dot(localEyeDir, e2), dot(localEyeDir, e1));
	const float phiI = atan2(dot(localLightDir, e2), dot(localLightDir, e1));
	const float phi = phiI - phiO;

	const float h = sin(ctx->gammaO);
	const float etap = sqrt(ctx->eta * ctx->eta - sinThetaO * sinThetaO) / cosThetaO;
	const float sinGammaT = h / etap;
	const float gammaT = Hair_SafeASin(sinGammaT);

	float apPdf[HAIR_PMAX + 1];
	Hair_ComputeApPdf(ctx, cosThetaO, apPdf);

	float pdf = 0.f;
	for (int p = 0; p < HAIR_PMAX; ++p) {
		float sinThetaOp, cosThetaOp;
		Hair_TiltThetaO(p, ctx->sin2kAlpha, ctx->cos2kAlpha,
				sinThetaO, cosThetaO, &sinThetaOp, &cosThetaOp);
		pdf += Hair_Mp(cosThetaI, cosThetaOp, sinThetaI, sinThetaOp, ctx->v[p]) *
				apPdf[p] * Hair_Np(phi, p, ctx->s, ctx->gammaO, gammaT);
	}
	pdf += Hair_Mp(cosThetaI, cosThetaO, sinThetaI, sinThetaO, ctx->v[HAIR_PMAX]) *
			apPdf[HAIR_PMAX] * (1.f / (2.f * M_PI_F));
	return pdf;
}

//------------------------------------------------------------------------------
// Huang'22 helpers — mirror of the huang namespace in hairmat.cpp.
// Frame convention: y = strand tangent, x/z = azimuthal plane.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float HairH_SinTheta(const float3 w) { return w.y; }
OPENCL_FORCE_INLINE float HairH_CosTheta(const float3 w) {
	return sqrt(fmax(0.f, w.x * w.x + w.z * w.z));
}
OPENCL_FORCE_INLINE float HairH_DirPhi(const float3 w) { return atan2(w.x, w.z); }
OPENCL_FORCE_INLINE void HairH_SincosPhi(const float3 w,
		__private float *s, __private float *c) {
	const float ct = HairH_CosTheta(w);
	*s = w.x / ct;
	*c = w.z / ct;
}
OPENCL_FORCE_INLINE bool HairH_IsCircular(const float b) { return b == 1.f; }

OPENCL_FORCE_INLINE float HairH_ToPhi(const float gamma, const float b) {
	if (HairH_IsCircular(b)) return gamma;
	return atan2(b * sin(gamma), cos(gamma));
}
OPENCL_FORCE_INLINE float HairH_ToGamma(const float phi, const float b) {
	if (HairH_IsCircular(b)) return phi;
	return atan2(sin(phi), b * cos(phi));
}
OPENCL_FORCE_INLINE float HairH_HToGamma(const float hDivR, const float b,
		const float3 wi) {
	return HairH_IsCircular(b) ? -Hair_SafeASin(hDivR) :
			atan2(wi.z, -b * wi.x) - acos(clamp(-hDivR, -1.f, 1.f));
}
OPENCL_FORCE_INLINE float3 HairH_ToPoint(const float gamma, const float b) {
	return MAKE_FLOAT3(sin(gamma), b * cos(gamma), 0.f);
}
OPENCL_FORCE_INLINE float3 HairH_SphgDir(const float theta, const float gamma,
		const float b) {
	const float st = sin(theta), ct = cos(theta);
	const float sg = sin(gamma), cg = cos(gamma);
	float sp, cp;
	if (HairH_IsCircular(b) || fabs(cg) < 1e-6f) {
		sp = sg;
		cp = cg;
	} else {
		const float tanP = b * sg / cg;
		cp = ((cg > 0.f) ? 1.f : -1.f) / sqrt(tanP * tanP + 1.f);
		sp = cp * tanP;
	}
	return MAKE_FLOAT3(sp * ct, st, cp * ct);
}
OPENCL_FORCE_INLINE float HairH_ArcLength(const float e2, const float gamma) {
	return (e2 == 0.f) ? 1.f : sqrt(1.f - e2 * sin(gamma) * sin(gamma));
}

OPENCL_FORCE_INLINE float3 HairH_RefractAngle(const float3 i, const float3 n,
		const float cosThetaT, const float invEta) {
	return (invEta * dot(n, i) + cosThetaT) * n - invEta * i;
}
OPENCL_FORCE_INLINE float3 HairH_ReflectDir(const float3 i, const float3 n) {
	return 2.f * dot(i, n) * n - i;
}
OPENCL_FORCE_INLINE bool HairH_MicrofacetVisible(const float3 v, const float3 m,
		const float3 h) {
	return dot(v, h) > 0.f && dot(v, m) > 0.f;
}
OPENCL_FORCE_INLINE bool HairH_MicrofacetVisible2(const float3 wi, const float3 wo,
		const float3 m, const float3 h) {
	return HairH_MicrofacetVisible(wi, m, h) && HairH_MicrofacetVisible(wo, m, h);
}

OPENCL_FORCE_INLINE float HairH_Lambda(const float alpha2, const float cosN) {
	const float c = fabs(cosN);
	if (c < 1e-7f) return 1e30f;
	return .5f * (sqrt(1.f + alpha2 * (1.f / (c * c) - 1.f)) - 1.f);
}
OPENCL_FORCE_INLINE float HairH_G(const float alpha2, const float cosI,
		const float cosO) {
	return 1.f / (1.f + HairH_Lambda(alpha2, cosI) + HairH_Lambda(alpha2, cosO));
}
OPENCL_FORCE_INLINE float HairH_Go(const float alpha2, const float cosNI,
		const float cosNO) {
	return (1.f + HairH_Lambda(alpha2, cosNI)) /
			(1.f + HairH_Lambda(alpha2, cosNI) + HairH_Lambda(alpha2, cosNO));
}
OPENCL_FORCE_INLINE float HairH_D(const float alpha2, const float cosNH) {
	const float c2 = fmin(cosNH * cosNH, 1.f);
	return alpha2 / (M_PI_F * (1.f - c2 + alpha2 * c2) * (1.f - c2 + alpha2 * c2));
}
OPENCL_FORCE_INLINE float HairH_FresnelT(const float cosI, const float eta,
		__private float *cosT) {
	const float c = fabs(cosI);
	const float sin2T = (1.f - c * c) / (eta * eta);
	if (sin2T >= 1.f) {
		*cosT = 0.f;
		return 1.f;
	}
	const float t = sqrt(1.f - sin2T);
	*cosT = t;
	const float rp = (eta * c - t) / (eta * c + t);
	const float rs = (c - eta * t) / (c + eta * t);
	return .5f * (rp * rp + rs * rs);
}
OPENCL_FORCE_INLINE float HairH_FresnelC(const float cosI, const float eta) {
	const float c = fabs(cosI);
	const float g = eta * eta - 1.f + c * c;
	if (g <= 0.f) return 1.f;
	const float gs = sqrt(g);
	return .5f * ((gs - c) / (gs + c)) * ((gs - c) / (gs + c)) *
			(1.f + (((gs + c) * c - 1.f) / ((gs - c) * c + 1.f)) *
			(((gs + c) * c - 1.f) / ((gs - c) * c + 1.f)));
}
OPENCL_FORCE_INLINE float HairH_EnergyScale(const float mu, const float sqrtAlpha,
		const float eta) {
	return 1.f / fmax(HairHuang_GlassE(mu, sqrtAlpha, eta), 1e-4f);
}
OPENCL_FORCE_INLINE float3 HairH_SampleWh(const float alpha, const float3 wi,
		const float3 wm, const float u0, const float u1) {
	float3 s, t;
	CoordinateSystem(wm, &s, &t);
	const float3 wiWm = MAKE_FLOAT3(dot(wi, s), dot(wi, t), dot(wi, wm));
	const float3 whWm = Microfacet_GgxSampleVNDF(wiWm, alpha, alpha, u0, u1);
	return whWm.x * s + whWm.y * t + whWm.z * wm;
}
OPENCL_FORCE_INLINE float3 HairH_EvalTRRT(const float T, const float R,
		const float3 A) {
	const float tAvg = fmax(1.f - R, 1e-5f);
	const float3 trrt = T * R * R * tAvg * A * A * A;
	return trrt / (MAKE_FLOAT3(1.f, 1.f, 1.f) - A * (1.f - tAvg));
}

// Deterministic VNDF quadrature points (Hammersley 8 / 4) replacing the
// Monte-Carlo quadrature Cycles runs inside eval_residual: Evaluate() must
// stay a pure function of (wi, wo).
#define HAIRH_QWH1 8
#define HAIRH_QWH2 4

OPENCL_FORCE_INLINE float2 HairH_QuadWh1(const int q) {
	const float2 pts[HAIRH_QWH1] = {
		(float2)(0.125f, 0.0625f), (float2)(0.625f, 0.1875f),
		(float2)(0.375f, 0.3125f), (float2)(0.875f, 0.4375f),
		(float2)(0.250f, 0.5625f), (float2)(0.750f, 0.6875f),
		(float2)(0.500f, 0.8125f), (float2)(0.0625f, 0.9375f)
	};
	return pts[q];
}
OPENCL_FORCE_INLINE float2 HairH_QuadWh2(const int q) {
	const float2 pts[HAIRH_QWH2] = {
		(float2)(0.19f, 0.41f), (float2)(0.69f, 0.83f),
		(float2)(0.44f, 0.09f), (float2)(0.94f, 0.61f)
	};
	return pts[q];
}

// Huang'22 eval at the hit azimuth. woLight is the evaluated direction in the
// Huang frame; returns f * |cos(wo . shadingNormal)| (LuxCore Evaluate value).
OPENCL_FORCE_INLINE float3 Hair_HuangEval(
		__global const Material* restrict material,
		__private const HairContext *ctx, const float3 woLight) {
	const float b = ctx->aspect;
	const float e2 = 1.f - b * b;
	const float alpha = ctx->rough;
	const float alpha2 = alpha * alpha;
	const float sqrtAlpha = sqrt(alpha);
	const float eta = ctx->eta;
	const float invEta = 1.f / eta;
	const float3 wi = ctx->wi;
	const float3 wo = woLight;

	const float gammaMi = HairH_HToGamma(ctx->hDivR, b, wi);
	const float3 wmi_ = HairH_SphgDir(0.f, gammaMi, b);
	const float3 wmi = HairH_SphgDir(ctx->tilt, gammaMi, b);
	const float cosMi = dot(wi, wmi);

	if (cosMi <= 0.f || dot(wo, wmi_) < 0.f || dot(wi, wmi_) < 0.f)
		return BLACK;

	const float arcI = HairH_ArcLength(e2, gammaMi);
	const float kSurf = 1.f / fmax(arcI * cosMi, 1e-4f);

	float3 result = BLACK;

	if (material->hair.scaleR > 0.f) {
		const float3 wh = normalize(wi + wo);
		if (HairH_MicrofacetVisible2(wi, wo, wmi_, wh)) {
			const float cosMo = dot(wo, wmi);
			const float F = HairH_FresnelC(dot(wi, wh), eta);
			const float D = HairH_D(alpha2, dot(wmi, wh));
			const float G = HairH_G(alpha2, cosMi, cosMo);
			const float scale = HairH_EnergyScale(cosMi, sqrtAlpha, eta);
			result += material->hair.scaleR * 0.25f * F * D * G * scale / cosMi;
		}
	}

	if (material->hair.scaleTT > 0.f || material->hair.scaleTRT > 0.f) {
		const float3 muA = ctx->sigma_a;
		float3 sTT = BLACK, sTRT = BLACK, sTRRT = BLACK;

		for (int q = 0; q < HAIRH_QWH1; ++q) {
			const float2 uv1 = HairH_QuadWh1(q);
			const float3 wh1 = HairH_SampleWh(alpha, wi, wmi, uv1.x, uv1.y);
			const float cosHi1 = dot(wi, wh1);
			if (cosHi1 <= 0.f)
				continue;
			float cosThetaT1;
			const float F1 = HairH_FresnelT(cosHi1, eta, &cosThetaT1);
			const float T1 = 1.f - F1;
			const float scale1 = HairH_EnergyScale(cosMi, sqrtAlpha, eta);
			const float3 wt = HairH_RefractAngle(wi, wh1, -cosThetaT1, invEta);
			const float phiT = HairH_DirPhi(wt);
			const float gammaMt = 2.f * HairH_ToPhi(phiT, b) - gammaMi;
			const float3 wmt = HairH_SphgDir(-ctx->tilt, gammaMt, b);
			const float3 wmt_ = HairH_SphgDir(0.f, gammaMt, b);
			const float cosMo1 = dot(-wt, wmi);
			const float cosMi2 = dot(-wt, wmt);
			const float G1o = HairH_Go(alpha2, cosMi, cosMo1);
			if (!HairH_MicrofacetVisible2(wi, -wt, wmi, wh1) ||
					!HairH_MicrofacetVisible2(wi, -wt, wmi_, wh1))
				continue;

			const float chord = HairH_IsCircular(b) ?
					2.f * cos(gammaMi - phiT) :
					-length(HairH_ToPoint(gammaMi, b) - HairH_ToPoint(gammaMt + M_PI_F, b));
			const float3 At = Spectrum_Exp(muA * (chord / HairH_CosTheta(wt)));
			const float scale2 = HairH_EnergyScale(cosMi2, sqrtAlpha, invEta);

			if (material->hair.scaleTT > 0.f && dot(wo, wt) >= invEta - 1e-5f) {
				float3 wh2 = invEta * wo - wt;
				const float rcpWh2 = 1.f / length(wh2);
				wh2 *= rcpWh2;
				const float cosMh2 = dot(wmt, wh2);
				if (cosMh2 >= 0.f) {
					const float cosHi2 = dot(-wt, wh2);
					const float cosHo2 = dot(-wo, wh2);
					const float cosMo2 = dot(-wo, wmt);
					const float T2 = (1.f - HairH_FresnelC(cosHi2, invEta)) * scale2;
					const float D2 = HairH_D(alpha2, cosMh2);
					const float G2 = HairH_G(alpha2, cosMi2, cosMo2);
					const float3 r = T1 * scale1 * T2 * D2 * G1o * G2 * At *
							cosHi2 * cosHo2 * rcpWh2 * rcpWh2 / cosMo1 *
							(HairH_ArcLength(e2, gammaMt) / arcI);
					if (!Spectrum_IsBlack(r) && !isnan(Spectrum_Y(r)))
						sTT += r;
				}
			}

			if (material->hair.scaleTRT > 0.f) {
				for (int q2 = 0; q2 < HAIRH_QWH2; ++q2) {
					const float2 uv2 = HairH_QuadWh2(q2);
					const float3 wh2 = HairH_SampleWh(alpha, -wt, wmt, uv2.x, uv2.y);
					const float cosHi2 = dot(-wt, wh2);
					if (cosHi2 <= 0.f)
						continue;
					const float R2 = HairH_FresnelC(cosHi2, invEta);
					const float3 wtr = HairH_ReflectDir(wt, wh2);

					if (dot(-wtr, wo) < invEta - 1e-5f) {
						sTRRT += HairH_EvalTRRT(T1, R2, At);
						continue;
					}
					if (!HairH_MicrofacetVisible2(-wt, -wtr, wmt, wh2) ||
							!HairH_MicrofacetVisible2(-wt, -wtr, wmt_, wh2))
						continue;

					const float phiTr = HairH_DirPhi(wtr);
					const float gammaMtr = gammaMi -
							2.f * (HairH_ToPhi(phiT, b) - HairH_ToPhi(phiTr, b)) + M_PI_F;
					const float3 wmtr = HairH_SphgDir(-ctx->tilt, gammaMtr, b);
					const float3 wmtr_ = HairH_SphgDir(0.f, gammaMtr, b);
					float3 wh3 = wtr + invEta * wo;
					const float rcpWh3 = 1.f / length(wh3);
					wh3 *= rcpWh3;
					const float cosMh3 = dot(wmtr, wh3);
					if (cosMh3 < 0.f ||
							!HairH_MicrofacetVisible2(wtr, -wo, wmtr, wh3) ||
							!HairH_MicrofacetVisible2(wtr, -wo, wmtr_, wh3)) {
						sTRRT += HairH_EvalTRRT(T1, R2, At);
						continue;
					}
					const float cosHi3 = dot(wh3, wtr);
					const float cosHo3 = dot(wh3, -wo);
					const float cosMi3 = dot(wmtr, wtr);
					const float T3 = (1.f - HairH_FresnelC(cosHi3, invEta)) *
							HairH_EnergyScale(cosMi3, sqrtAlpha, invEta);
					const float D3 = HairH_D(alpha2, cosMh3);
					const float3 Atr = Spectrum_Exp(muA *
							(HairH_IsCircular(b) ?
								-2.f * fabs(cos(phiTr - gammaMt)) :
								-length(HairH_ToPoint(gammaMtr, b) - HairH_ToPoint(gammaMt, b)))
							/ HairH_CosTheta(wtr));
					const float cosMo2 = dot(wmt, -wtr);
					const float G2o = HairH_Go(alpha2, cosMi2, cosMo2);
					const float G3 = HairH_G(alpha2, cosMi3, dot(wmtr, -wo));
					const float3 r = T1 * scale1 * R2 * scale2 * T3 * D3 *
							G1o * G2o * G3 * At * Atr *
							cosMi2 * cosHi3 * cosHo3 * rcpWh3 * rcpWh3 /
							(cosMo1 * cosMo2) *
							(HairH_ArcLength(e2, gammaMtr) / arcI);
					if (!Spectrum_IsBlack(r) && !isnan(Spectrum_Y(r)))
						sTRT += r;
					sTRRT += HairH_EvalTRRT(T1, R2, At);
				}
			}
		}
		sTT /= HAIRH_QWH1;
		sTRT /= (HAIRH_QWH1 * HAIRH_QWH2);
		sTRRT /= (HAIRH_QWH1 * HAIRH_QWH2);

		const float sinTi = HairH_SinTheta(wi), cosTi = HairH_CosTheta(wi);
		const float sinTo = HairH_SinTheta(wo), cosTo = HairH_CosTheta(wo);
		const float M = Hair_Mp(cosTi, cosTo, sinTi, sinTo, 4.f * alpha);
		const float N = 1.f / (2.f * M_PI_F);
		result += ((material->hair.scaleTT * sTT + material->hair.scaleTRT * sTRT) *
				invEta * invEta + sTRRT * M * N * (2.f / M_PI_F)) * kSurf;
	}

	return result;
}

// Huang'22 forward sampling: energy-proportional lobe selection, self-
// normalized eval, unit directional pdf (same convention as Cycles).
OPENCL_FORCE_INLINE float3 Hair_HuangSample(
		__global const Material* restrict material,
		__private const HairContext *ctx, __private float3 *woLight,
		__private const float *u) {
	const float b = ctx->aspect;
	const float alpha = ctx->rough;
	const float alpha2 = alpha * alpha;
	const float sqrtAlpha = sqrt(alpha);
	const float eta = ctx->eta;
	const float invEta = 1.f / eta;
	const float3 wi = ctx->wi;

	const float gammaMi = HairH_HToGamma(ctx->hDivR, b, wi);
	const float3 wmi_ = HairH_SphgDir(0.f, gammaMi, b);
	const float3 wmi = HairH_SphgDir(ctx->tilt, gammaMi, b);
	const float cosMi1 = dot(wmi, wi);
	if (cosMi1 < 0.f || dot(wmi_, wi) < 0.f)
		return BLACK;

	const float3 wh1 = HairH_SampleWh(alpha, wi, wmi, u[1], u[2]);
	const float3 wr = HairH_ReflectDir(wi, wh1);
	if (!HairH_MicrofacetVisible(wi, wmi_, wh1))
		return BLACK;
	float cosThetaT1;
	const float R1 = HairH_FresnelT(dot(wi, wh1), eta, &cosThetaT1);
	const float scale1 = HairH_EnergyScale(cosMi1, sqrtAlpha, eta);
	const float R = material->hair.scaleR * R1 * scale1 *
			(HairH_MicrofacetVisible(wr, wmi_, wh1) ? 1.f : 0.f) *
			HairH_Go(alpha2, cosMi1, dot(wmi, wr));

	const float3 wt = HairH_RefractAngle(wi, wh1, -cosThetaT1, invEta);
	const float phiT = HairH_DirPhi(wt);
	const float gammaMt = 2.f * HairH_ToPhi(phiT, b) - gammaMi;
	const float3 wmt = HairH_SphgDir(-ctx->tilt, gammaMt, b);
	const float3 wmt_ = HairH_SphgDir(0.f, gammaMt, b);
	const float3 wh2 = HairH_SampleWh(alpha, -wt, wmt, u[3], u[4]);
	const float3 wtr = HairH_ReflectDir(wt, wh2);
	const float cosMi2 = dot(-wt, wmt);

	float3 TT = BLACK, TRT = BLACK, TRRT = BLACK;
	float3 wtt = MAKE_FLOAT3(0.f, 0.f, 1.f);
	float3 wtrt = wtt, wtrrt = wtt;
	if (cosMi2 > 0.f && HairH_MicrofacetVisible(-wt, wmi_, wh1) &&
			HairH_MicrofacetVisible(-wt, wmt_, wh2)) {
		const float3 muA = ctx->sigma_a;
		const float chord = HairH_IsCircular(b) ?
				2.f * cos(phiT - gammaMi) :
				-length(HairH_ToPoint(gammaMi, b) - HairH_ToPoint(gammaMt + M_PI_F, b));
		const float3 At = Spectrum_Exp(muA * chord / HairH_CosTheta(wt));
		float cosThetaT2;
		const float R2 = HairH_FresnelT(dot(-wt, wh2), invEta, &cosThetaT2);
		const float T1 = (1.f - R1) * scale1 *
				HairH_Go(alpha2, cosMi1, dot(wmi, -wt));
		const float T2 = 1.f - R2;
		const float scale2 = HairH_EnergyScale(cosMi2, sqrtAlpha, invEta);
		wtt = HairH_RefractAngle(-wt, wh2, -cosThetaT2, eta);
		if (dot(wmt, -wtt) > 0.f && T2 > 0.f &&
				HairH_MicrofacetVisible(-wtt, wmt_, wh2)) {
			TT = material->hair.scaleTT * T1 * At * T2 * scale2 *
					HairH_Go(alpha2, cosMi2, dot(wmt, -wtt));
		}

		const float phiTr = HairH_DirPhi(wtr);
		const float gammaMtr = gammaMi -
				2.f * (HairH_ToPhi(phiT, b) - HairH_ToPhi(phiTr, b)) + M_PI_F;
		const float3 wmtr = HairH_SphgDir(-ctx->tilt, gammaMtr, b);
		const float3 wh3 = HairH_SampleWh(alpha, wtr, wmtr, u[5], u[6]);
		float cosThetaT3;
		const float R3 = HairH_FresnelT(dot(wtr, wh3), invEta, &cosThetaT3);
		wtrt = HairH_RefractAngle(wtr, wh3, -cosThetaT3, eta);
		const float cosMi3 = dot(wmtr, wtr);
		if (cosMi3 > 0.f) {
			const float chord2 = HairH_IsCircular(b) ?
					-2.f * fabs(cos(phiTr - gammaMt)) :
					-length(HairH_ToPoint(gammaMt, b) - HairH_ToPoint(gammaMtr, b));
			const float3 Atr = Spectrum_Exp(muA * chord2 / HairH_CosTheta(wtr));
			const float3 TR = T1 * R2 * scale2 * At * Atr *
					HairH_EnergyScale(cosMi3, sqrtAlpha, invEta) *
					HairH_Go(alpha2, cosMi2, dot(wmt, -wtr));
			const float T3 = 1.f - R3;
			const float3 wmtr_ = HairH_SphgDir(0.f, gammaMtr, b);
			if (T3 > 0.f && HairH_MicrofacetVisible2(wtr, -wtrt, wmtr_, wh3)) {
				TRT = material->hair.scaleTRT * TR * T3 *
						HairH_Go(alpha2, cosMi3, dot(wmtr, -wtrt));
			}

			const float randT = fmax(u[7], 1e-5f);
			const float fac = 1.f + 4.f * alpha *
					log(randT + (1.f - randT) * exp(-0.5f / alpha));
			const float uT = Hair_Fract(u[7] * 7919.f);
			const float sinTo = -fac * HairH_SinTheta(wi) +
					Hair_SafeSqrt(1.f - fac * fac) *
					cos(2.f * M_PI_F * uT) * HairH_CosTheta(wi);
			const float cosTo = Hair_SafeSqrt(1.f - sinTo * sinTo);
			const float phiO = 2.f * M_PI_F * Hair_Fract(u[0] * 104729.f + 0.31f);
			wtrrt = MAKE_FLOAT3(sin(phiO) * cosTo, sinTo, cos(phiO) * cosTo);

			const float3 Aavg = sqrt(At * Atr);
			const float tAvg = fmax(0.5f * (T2 + T3), 1e-5f);
			const float3 Ares = Aavg * tAvg /
					(MAKE_FLOAT3(1.f, 1.f, 1.f) - Aavg * (1.f - tAvg));
			TRRT = TR * R3 * Ares *
					HairH_Go(alpha2, cosMi3, dot(wmtr, HairH_ReflectDir(wtr, wh3)));
		}
	}

	const float eR = R;
	const float eTT = Spectrum_Y(TT);
	const float eTRT = Spectrum_Y(TRT);
	const float eTRRT = Spectrum_Y(TRRT);
	const float total = eR + eTT + eTRT + eTRRT;
	if (total <= 0.f)
		return BLACK;

	float sel = u[0] * total;
	float3 localO;
	float3 eval;
	if (sel < eR) {
		localO = wr;
		eval = MAKE_FLOAT3(total, total, total);
	} else if (sel < eR + eTT) {
		localO = wtt;
		eval = TT * (total / eTT);
	} else if (sel < eR + eTT + eTRT) {
		localO = wtrt;
		eval = TRT * (total / eTRT);
	} else {
		localO = wtrrt;
		eval = TRRT * (total / eTRRT);
	}
	*woLight = localO;
	return eval;
}

OPENCL_FORCE_INLINE void HairMaterial_Albedo(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	HairContext ctx;
	if (!Hair_SetupContext(material, hitPoint, MAKE_FLOAT3(0.f, 0.f, 1.f),
			&ctx MATERIALS_PARAM)) {
		EvalStack_PushFloat3(BLACK);
		return;
	}
	if (material->hair.model == 1) {
		const float a = clamp(material->hair.scaleR *
				HairH_EnergyScale(1.f, sqrt(ctx.rough), ctx.eta) +
				material->hair.scaleTT + material->hair.scaleTRT, 0.f, 1.f);
		EvalStack_PushFloat3(MAKE_FLOAT3(a, a, a));
		return;
	}
	float apPdf[HAIR_PMAX + 1];
	Hair_ComputeApPdf(&ctx, 1.f, apPdf);
	float sum = 0.f;
	for (int i = 0; i <= HAIR_PMAX; ++i)
		sum += apPdf[i];
	EvalStack_PushFloat3(MAKE_FLOAT3(clamp(sum * 0.5f, 0.f, 1.f),
			clamp(sum * 0.5f, 0.f, 1.f), clamp(sum * 0.5f, 0.f, 1.f)));
}

OPENCL_FORCE_INLINE void HairMaterial_GetInteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HairMaterial_GetExteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HairMaterial_GetPassThroughTransparency(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HairMaterial_GetEmittedRadiance(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HairMaterial_Evaluate(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 lightDir, eyeDir;
	EvalStack_PopFloat3(eyeDir);
	EvalStack_PopFloat3(lightDir);

	HairContext ctx;
	if (!Hair_SetupContext(material, hitPoint, eyeDir, &ctx MATERIALS_PARAM)) {
		MATERIAL_EVALUATE_RETURN_BLACK;
	}

	float3 result;
	if (material->hair.model == 1) {
		const float3 woH = MAKE_FLOAT3(dot(lightDir, ctx.frameX),
				dot(lightDir, ctx.frameY), dot(lightDir, ctx.frameZ));
		result = Hair_HuangEval(material, &ctx, woH);
	} else {
		result = Hair_f(hitPoint, &ctx, lightDir, eyeDir);
	}
	if (Spectrum_IsBlack(result)) {
		MATERIAL_EVALUATE_RETURN_BLACK;
	}

	const BSDFEvent event = GLOSSY | REFLECT | TRANSMIT;
	// Huang's sampler is self-normalized: unit directional pdf
	const float directPdfW = (material->hair.model == 1) ? 1.f :
			Hair_EvalPdf(hitPoint, &ctx, lightDir, eyeDir);

	EvalStack_PushFloat3(result);
	EvalStack_PushBSDFEvent(event);
	EvalStack_PushFloat(directPdfW);
}

OPENCL_FORCE_INLINE void HairMaterial_Sample(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float u0, u1, passThroughEvent;
	EvalStack_PopFloat(passThroughEvent);
	EvalStack_PopFloat(u1);
	EvalStack_PopFloat(u0);
	float3 fixedDir;
	EvalStack_PopFloat3(fixedDir);

	HairContext ctx;
	if (!Hair_SetupContext(material, hitPoint, fixedDir, &ctx MATERIALS_PARAM)) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	if (material->hair.model == 1) {
		float u[8];
		Hair_DemuxFloat(u0, &u[0], &u[1]);
		Hair_DemuxFloat(u1, &u[2], &u[3]);
		Hair_DemuxFloat(passThroughEvent, &u[4], &u[5]);
		Hair_DemuxFloat(Hair_Fract(u0 * 7919.f + u1 * 104729.f + 0.31f),
				&u[6], &u[7]);

		float3 woH;
		const float3 eval = Hair_HuangSample(material, &ctx, &woH, u);
		if (Spectrum_IsBlack(eval)) {
			MATERIAL_SAMPLE_RETURN_BLACK;
		}
		const float3 sampledDirH = woH.x * ctx.frameX + woH.y * ctx.frameY +
				woH.z * ctx.frameZ;

		EvalStack_PushFloat3(eval);
		EvalStack_PushFloat3(sampledDirH);
		EvalStack_PushFloat(1.f);
		EvalStack_PushBSDFEvent(GLOSSY | REFLECT | TRANSMIT);
		return;
	}

	const float3 T = ctx.tangent;
	float3 e1, e2;
	Hair_Frame(T, &e1, &e2);

	const float sinThetaO = dot(fixedDir, T);
	const float cosThetaO = Hair_SafeSqrt(1.f - sinThetaO * sinThetaO);
	const float phiO = atan2(dot(fixedDir, e2), dot(fixedDir, e1));

	// Four sample dimensions from the three supplied values
	float u[2][2];
	Hair_DemuxFloat(u0, &u[0][0], &u[0][1]);
	Hair_DemuxFloat(u1, &u[1][0], &u[1][1]);

	// Choose the lobe p from the Ap pdf
	float apPdf[HAIR_PMAX + 1];
	Hair_ComputeApPdf(&ctx, cosThetaO, apPdf);
	int p;
	float sel = u[0][0];
	for (p = 0; p < HAIR_PMAX; ++p) {
		if (sel < apPdf[p]) break;
		sel -= apPdf[p];
	}

	float sinThetaOp, cosThetaOp;
	Hair_TiltThetaO(p, ctx.sin2kAlpha, ctx.cos2kAlpha,
			sinThetaO, cosThetaO, &sinThetaOp, &cosThetaOp);

	// Sample M_p -> thetaI
	const float u10 = fmax(u[1][0], 1e-5f);
	const float cosTheta =
			1.f + ctx.v[p] * log(u10 + (1.f - u10) * exp(-2.f / ctx.v[p]));
	const float sinTheta = Hair_SafeSqrt(1.f - cosTheta * cosTheta);
	const float cosPhi = cos(2.f * M_PI_F * u[1][1]);
	const float sinThetaI = -cosTheta * sinThetaOp + sinTheta * cosPhi * cosThetaOp;
	const float cosThetaI = Hair_SafeSqrt(1.f - sinThetaI * sinThetaI);

	// Sample N_p -> dphi
	const float h = sin(ctx.gammaO);
	const float etap = sqrt(ctx.eta * ctx.eta - sinThetaO * sinThetaO) / cosThetaO;
	const float sinGammaT = h / etap;
	const float gammaT = Hair_SafeASin(sinGammaT);
	float dphi;
	if (p < HAIR_PMAX)
		dphi = Hair_Phi(p, ctx.gammaO, gammaT) +
				Hair_SampleTrimmedLogistic(u[0][1], ctx.s, -M_PI_F, M_PI_F);
	else
		dphi = 2.f * M_PI_F * u[0][1];

	const float phiI = phiO + dphi;
	const float3 sampledDir = sinThetaI * T +
			cosThetaI * cos(phiI) * e1 + cosThetaI * sin(phiI) * e2;

	// Pdf for the sampled direction
	float pdf = 0.f;
	for (int pp = 0; pp < HAIR_PMAX; ++pp) {
		float stp, ctp;
		Hair_TiltThetaO(pp, ctx.sin2kAlpha, ctx.cos2kAlpha,
				sinThetaO, cosThetaO, &stp, &ctp);
		pdf += Hair_Mp(cosThetaI, ctp, sinThetaI, stp, ctx.v[pp]) *
				apPdf[pp] * Hair_Np(dphi, pp, ctx.s, ctx.gammaO, gammaT);
	}
	pdf += Hair_Mp(cosThetaI, cosThetaO, sinThetaI, sinThetaO, ctx.v[HAIR_PMAX]) *
			apPdf[HAIR_PMAX] * (1.f / (2.f * M_PI_F));

	if (pdf <= 0.f) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	const BSDFEvent event = (p == 0) ? (GLOSSY | REFLECT) : (GLOSSY | TRANSMIT);

	const float3 result = Hair_f(hitPoint, &ctx, sampledDir, fixedDir) / pdf;
	if (Spectrum_IsBlack(result)) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	EvalStack_PushFloat3(result);
	EvalStack_PushFloat3(sampledDir);
	EvalStack_PushFloat(pdf);
	EvalStack_PushBSDFEvent(event);
}

//------------------------------------------------------------------------------
// Material specific EvalOp
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE void HairMaterial_EvalOp(
		__global const Material* restrict material,
		const MaterialEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint
		MATERIALS_PARAM_DECL) {
	switch (evalType) {
		case EVAL_ALBEDO:
			HairMaterial_Albedo(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_INTERIOR_VOLUME:
			HairMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EXTERIOR_VOLUME:
			HairMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EMITTED_RADIANCE:
			HairMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_PASS_TROUGH_TRANSPARENCY:
			HairMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_EVALUATE:
			HairMaterial_Evaluate(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_SAMPLE:
			HairMaterial_Sample(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
