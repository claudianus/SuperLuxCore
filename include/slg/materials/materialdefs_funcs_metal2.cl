#line 2 "materialdefs_funcs_metal2.cl"

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
// Metal2 material
//
// LuxRender Metal2 material porting.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void Metal2Material_GetNK(__global const Material* restrict material, __global const HitPoint *hitPoint,
		float3 *n, float3 *k
		MATERIALS_PARAM_DECL) {
	const uint fresnelTexIndex = material->metal2.fresnelTexIndex;
	if (fresnelTexIndex != NULL_INDEX) {
		__global const Texture* restrict fresnelTex = &texs[fresnelTexIndex];

		if (fresnelTex->type == FRESNELCOLOR_TEX) {
			const float3 f = Texture_GetSpectrumValue(fresnelTex->fresnelColor.krIndex, hitPoint TEXTURES_PARAM);
			*n = FresnelApproxN3(f);
			*k = FresnelApproxK3(f);
		} else {
			*n = VLOAD3F(&fresnelTex->fresnelConst.n.c[0]);
			*k = VLOAD3F(&fresnelTex->fresnelConst.k.c[0]);
		}
	} else {
		*n = clamp(Texture_GetSpectrumValue(material->metal2.nTexIndex, hitPoint TEXTURES_PARAM), .001f, INFINITY);
		*k = clamp(Texture_GetSpectrumValue(material->metal2.kTexIndex, hitPoint TEXTURES_PARAM), .001f, INFINITY);
	}
}

OPENCL_FORCE_INLINE void Metal2Material_Albedo(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 n, k;
	Metal2Material_GetNK(material, hitPoint,
			&n, &k
			MATERIALS_PARAM);

	const float3 albedo = Spectrum_Clamp(FresnelGeneral_Evaluate(n, k, 1.f));

	EvalStack_PushFloat3(albedo);
}

OPENCL_FORCE_INLINE void Metal2Material_GetInteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void Metal2Material_GetExteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void Metal2Material_GetPassThroughTransparency(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void Metal2Material_GetEmittedRadiance(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void Metal2Material_Evaluate(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 lightDir, eyeDir;
	EvalStack_PopFloat3(eyeDir);
	EvalStack_PopFloat3(lightDir);

	const float uVal = Texture_GetFloatValue(material->metal2.nuTexIndex, hitPoint TEXTURES_PARAM);
	const float vVal = Texture_GetFloatValue(material->metal2.nvTexIndex, hitPoint TEXTURES_PARAM);

	const float u = clamp(uVal, 1e-9f, 1.f);
	const float v = clamp(vVal, 1e-9f, 1.f);
	const float u2 = u * u;
	const float v2 = v * v;
	const float anisotropy = (u2 < v2) ? (1.f - u2 / v2) : u2 > 0.f ? (v2 / u2 - 1.f) : 0.f;
	const float roughness = u * v;
	const bool useGgx = material->metal2.useGgx;
	const float alphaT = fmax(u2, 1e-4f);
	const float alphaB = fmax(v2, 1e-4f);

	const float3 wh = normalize(lightDir + eyeDir);
	const float cosWH = dot(lightDir, wh);

	const float directPdfW = useGgx ?
		Microfacet_GgxVNDFReflectionPdf(eyeDir, wh, alphaT, alphaB) :
		SchlickDistribution_Pdf(roughness, wh, anisotropy) / (4.f * cosWH);

	float3 nVal, kVal;
	Metal2Material_GetNK(material, hitPoint,
			&nVal, &kVal
			MATERIALS_PARAM);

	const BSDFEvent event = GLOSSY | REFLECT;
	// f*|cos(lightDir)| = D * G2 * F / (4 * |eyeDir.z|)
	float3 result;
	if (useGgx && material->metal2.multibounce) {
		// Heitz'16 height-tracking multi-bounce evaluation; the estimator
		// already includes the single-scatter term.
		result = Microfacet_GgxMSConductorEval(eyeDir, lightDir,
				MAKE_FLOAT3(hitPoint->p.x, hitPoint->p.y, hitPoint->p.z),
				alphaT, alphaB, nVal, kVal);
	} else {
		const float3 F = Spectrum_Clamp(FresnelGeneral_Evaluate(nVal, kVal, cosWH));
		result = useGgx ?
			(Microfacet_GgxD(wh, alphaT, alphaB) *
					Microfacet_GgxG2(lightDir, eyeDir, alphaT, alphaB) /
					(4.f * fabs(eyeDir.z))) * F :
			(SchlickDistribution_D(roughness, wh, anisotropy) *
					SchlickDistribution_G(roughness, lightDir, eyeDir) /
					(4.f * fabs(eyeDir.z))) * F;
	}
	
	EvalStack_PushFloat3(result);
	EvalStack_PushBSDFEvent(event);
	EvalStack_PushFloat(directPdfW);
}

OPENCL_FORCE_INLINE void Metal2Material_Sample(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float u0, u1, passThroughEvent;
	EvalStack_PopFloat(passThroughEvent);
	EvalStack_PopFloat(u1);
	EvalStack_PopFloat(u0);
	float3 fixedDir;
	EvalStack_PopFloat3(fixedDir);

	if (fabs(fixedDir.z) < DEFAULT_COS_EPSILON_STATIC) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	const float uVal = Texture_GetFloatValue(material->metal2.nuTexIndex, hitPoint TEXTURES_PARAM);
	const float vVal = Texture_GetFloatValue(material->metal2.nvTexIndex, hitPoint TEXTURES_PARAM);

	const float u = clamp(uVal, 1e-9f, 1.f);
	const float v = clamp(vVal, 1e-9f, 1.f);
	const float u2 = u * u;
	const float v2 = v * v;
	const float anisotropy = (u2 < v2) ? (1.f - u2 / v2) : u2 > 0.f ? (v2 / u2 - 1.f) : 0.f;
	const float roughness = u * v;
	const bool useGgx = material->metal2.useGgx;
	const float alphaT = fmax(u2, 1e-4f);
	const float alphaB = fmax(v2, 1e-4f);

	float3 wh;
	float d = 0.f, specPdf = 0.f;
	if (useGgx)
		wh = Microfacet_GgxSampleVNDF(fixedDir, alphaT, alphaB, u0, u1);
	else
		SchlickDistribution_SampleH(roughness, anisotropy, u0, u1, &wh, &d, &specPdf);
	const float cosWH = dot(fixedDir, wh);
	const float3 sampledDir = 2.f * cosWH * wh - fixedDir;

	const float coso = fabs(fixedDir.z);
	const float cosi = fabs(sampledDir.z);
	if ((cosi < DEFAULT_COS_EPSILON_STATIC) || (fixedDir.z * sampledDir.z < 0.f)) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	const float pdfW = useGgx ?
		Microfacet_GgxVNDFReflectionPdf(fixedDir, wh, alphaT, alphaB) :
		specPdf / (4.f * fabs(cosWH));
	if (pdfW <= 0.f) {
		MATERIAL_SAMPLE_RETURN_BLACK;
	}

	float3 nVal, kVal;
	Metal2Material_GetNK(material, hitPoint,
			&nVal, &kVal
			MATERIALS_PARAM);

	const float3 F = FresnelGeneral_Evaluate(nVal, kVal, cosWH);
	Spectrum_Clamp(F);

	const BSDFEvent event = GLOSSY | REFLECT;

	float3 result;
	if (useGgx && material->metal2.multibounce) {
		// VNDF single-scatter sampling covers the multi-bounce support;
		// weight = (f_ss+ms)*cos / pdf_ss stays unbiased.
		result = Microfacet_GgxMSConductorEval(fixedDir, sampledDir,
				MAKE_FLOAT3(hitPoint->p.x, hitPoint->p.y, hitPoint->p.z),
				alphaT, alphaB, nVal, kVal) / pdfW;
	} else if (useGgx) {
		// (f*cos_i)/pdf = F * G2/G1(wo) for VNDF sampling
		const float g1 = Microfacet_GgxG1(fixedDir, alphaT, alphaB);
		if (g1 <= 0.f) {
			MATERIAL_SAMPLE_RETURN_BLACK;
		}
		result = F * (Microfacet_GgxG2(sampledDir, fixedDir, alphaT, alphaB) / g1);
	} else {
		const float G = SchlickDistribution_G(roughness, fixedDir, sampledDir);
		float factor = (d / specPdf) * G * fabs(cosWH);
		//if (!fromLight)
			factor /= coso;
		//else
		//	factor /= cosi;
		result = factor * F;
	}

	EvalStack_PushFloat3(result);
	EvalStack_PushFloat3(sampledDir);
	EvalStack_PushFloat(pdfW);
	EvalStack_PushBSDFEvent(event);
}

//------------------------------------------------------------------------------
// Material specific EvalOp
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE void Metal2Material_EvalOp(
		__global const Material* restrict material,
		const MaterialEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint
		MATERIALS_PARAM_DECL) {
	switch (evalType) {
		case EVAL_ALBEDO:
			Metal2Material_Albedo(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_INTERIOR_VOLUME:
			Metal2Material_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EXTERIOR_VOLUME:
			Metal2Material_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EMITTED_RADIANCE:
			Metal2Material_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_PASS_TROUGH_TRANSPARENCY:
			Metal2Material_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_EVALUATE:
			Metal2Material_Evaluate(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_SAMPLE:
			Metal2Material_Sample(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
