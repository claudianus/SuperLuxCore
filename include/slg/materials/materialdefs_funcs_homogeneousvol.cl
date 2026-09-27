#line 2 "materialdefs_funcs_homogenousvol.cl"

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
// HomogeneousVol material
//------------------------------------------------------------------------------

// SSS albedo parametrization: maps the diffuse surface albedo A and the
// scattering mean free path d to the extinction sigma_t and the physical
// single-scatter albedo alpha (d'Eon, "A Hitchhiker's Guide to Multiple
// Scattering" v0.3.2, Eq. 53.7 — Cycles' "van de Hulst" random-walk
// remap). Mirrors volume_funcs.cl HomogeneousVolume_SSSCoeffs.
OPENCL_FORCE_INLINE void HomogeneousVolMaterial_SSSCoeffs(
		__global const Material* restrict material,
		__global const HitPoint *hitPoint, float3 *sigmaT, float3 *alpha
		MATERIALS_PARAM_DECL) {
	const float3 A = clamp(Texture_GetSpectrumValue(
			material->volume.homogenous.sssAlbedoTexIndex, hitPoint
			TEXTURES_PARAM), 0.f, 1.f);
	const float3 mfp = Texture_GetSpectrumValue(
			material->volume.homogenous.sssMfpTexIndex, hitPoint
			TEXTURES_PARAM);
	const float3 g = clamp(Texture_GetSpectrumValue(
			material->volume.homogenous.gTexIndex, hitPoint
			TEXTURES_PARAM), -0.99f, 0.99f);

	const float3 x = 4.20863f * A + 4.09712f -
			sqrt(9.59217f + 41.6808f * A + 17.7126f * A * A);
	const float3 s2 = x * x;
	*alpha = clamp((1.f - s2) / (1.f - g * s2), 0.f, 0.999999f);
	*sigmaT = 1.f / max(mfp, (float3)(1e-6f, 1e-6f, 1e-6f));
}

// "cb15" parametrization, mirror of HomogeneousVolume_SSSCoeffsCB15 in
// volume_funcs.cl / SSSRemapCB15 in homogenous.cpp: A is the measured
// external reflectance, inverted via the vdH fit times the CB15
// diffusion boundary ratio K(a,eta) (fixed-point iteration); eta comes
// from the volume IOR texture. d is the transport mfp.
OPENCL_FORCE_INLINE void HomogeneousVolMaterial_SSSCoeffsCB15(
		__global const Material* restrict material,
		__global const HitPoint *hitPoint, float3 *sigmaT, float3 *alpha
		MATERIALS_PARAM_DECL) {
	const float3 A = clamp(Texture_GetSpectrumValue(
			material->volume.homogenous.sssAlbedoTexIndex, hitPoint
			TEXTURES_PARAM), 1e-4f, 0.999f);
	const float3 mfp = Texture_GetSpectrumValue(
			material->volume.homogenous.sssMfpTexIndex, hitPoint
			TEXTURES_PARAM);
	const float3 g = clamp(Texture_GetSpectrumValue(
			material->volume.homogenous.gTexIndex, hitPoint
			TEXTURES_PARAM), -0.99f, 0.99f);
	const float eta = fmax(Texture_GetFloatValue(
			material->volume.iorTexIndex, hitPoint
			TEXTURES_PARAM), 1.0001f);

	for (int ch = 0; ch < 3; ++ch) {
		const float Ad = (ch == 0) ? A.x : ((ch == 1) ? A.y : A.z);
		const float gi = (ch == 0) ? g.x : ((ch == 1) ? g.y : g.z);
		const float di = (ch == 0) ? mfp.x : ((ch == 1) ? mfp.y : mfp.z);

		float al = SSSAlphaVanDeHulst(Ad, gi);
		for (int i = 0; i < 6; ++i) {
			const float k = clamp(SSSRdDiffusion(al, eta) /
					fmax(SSSRdDiffusion(al, 1.f), 1e-4f), 1e-2f, 1.f);
			al = SSSAlphaVanDeHulst(clamp(Ad / k, 1e-4f, 0.999f), gi);
		}
		const float st = (1.f / fmax(di, 1e-6f)) / fmax(1.f - gi * al, 1e-6f);
		if (ch == 0) { alpha->x = al; sigmaT->x = st; }
		else if (ch == 1) { alpha->y = al; sigmaT->y = st; }
		else { alpha->z = al; sigmaT->z = st; }
	}
}

// sigma_s/sigma_a, honoring the SSS albedo parametrization when present.
OPENCL_FORCE_INLINE void HomogeneousVolMaterial_Coeffs(
		__global const Material* restrict material,
		__global const HitPoint *hitPoint, float3 *sigmaS, float3 *sigmaA
		MATERIALS_PARAM_DECL) {
	if (material->volume.homogenous.sssAlbedoTexIndex != NULL_INDEX) {
		float3 sigmaT, alpha;
		if (material->volume.homogenous.sssProfile == 1)
			HomogeneousVolMaterial_SSSCoeffsCB15(material, hitPoint,
					&sigmaT, &alpha MATERIALS_PARAM);
		else
			HomogeneousVolMaterial_SSSCoeffs(material, hitPoint,
					&sigmaT, &alpha MATERIALS_PARAM);
		*sigmaS = alpha * sigmaT;
		*sigmaA = (WHITE - alpha) * sigmaT;
	} else {
		*sigmaS = clamp(Texture_GetSpectrumValue(
				material->volume.homogenous.sigmaSTexIndex, hitPoint
				TEXTURES_PARAM), 0.f, INFINITY);
		*sigmaA = clamp(Texture_GetSpectrumValue(
				material->volume.homogenous.sigmaATexIndex, hitPoint
				TEXTURES_PARAM), 0.f, INFINITY);
	}
}

OPENCL_FORCE_INLINE void HomogeneousVolMaterial_Albedo(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 sigmaS, sigmaA;
	HomogeneousVolMaterial_Coeffs(material, hitPoint, &sigmaS, &sigmaA
			MATERIALS_PARAM);

	const float3 albedo = SchlickScatter_Albedo(sigmaS, sigmaA);

	EvalStack_PushFloat3(albedo);
}

OPENCL_FORCE_INLINE void HomogeneousVolMaterial_GetInteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HomogeneousVolMaterial_GetExteriorVolume(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HomogeneousVolMaterial_GetPassThroughTransparency(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HomogeneousVolMaterial_GetEmittedRadiance(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	DefaultMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
}

OPENCL_FORCE_INLINE void HomogeneousVolMaterial_Evaluate(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float3 lightDir, eyeDir;
	EvalStack_PopFloat3(eyeDir);
	EvalStack_PopFloat3(lightDir);

	float3 sigmaS, sigmaA;
	HomogeneousVolMaterial_Coeffs(material, hitPoint, &sigmaS, &sigmaA
			MATERIALS_PARAM);
	const float3 gTexVal = Texture_GetSpectrumValue(material->volume.homogenous.gTexIndex, hitPoint TEXTURES_PARAM);

	BSDFEvent event;
	float directPdfW;
	const float3 result = SchlickScatter_Evaluate(
			hitPoint, eyeDir, lightDir,
			&event, &directPdfW,
			sigmaS, sigmaA, gTexVal,
			material->volume.homogenous.phaseFunc);

	EvalStack_PushFloat3(result);
	EvalStack_PushBSDFEvent(event);
	EvalStack_PushFloat(directPdfW);
}

OPENCL_FORCE_INLINE void HomogeneousVolMaterial_Sample(__global const Material* restrict material,
		__global const HitPoint *hitPoint,
		__global float *evalStack, uint *evalStackOffset
		MATERIALS_PARAM_DECL) {
	float u0, u1, passThroughEvent;
	EvalStack_PopFloat(passThroughEvent);
	EvalStack_PopFloat(u1);
	EvalStack_PopFloat(u0);
	float3 fixedDir;
	EvalStack_PopFloat3(fixedDir);

	float3 sigmaS, sigmaA;
	HomogeneousVolMaterial_Coeffs(material, hitPoint, &sigmaS, &sigmaA
			MATERIALS_PARAM);
	const float3 gTexVal = Texture_GetSpectrumValue(material->volume.homogenous.gTexIndex, hitPoint TEXTURES_PARAM);

	float3 sampledDir;
	float pdfW;
	BSDFEvent event;
	const float3 result = SchlickScatter_Sample(
			hitPoint, fixedDir, &sampledDir,
			u0, u1,
			passThroughEvent,
			&pdfW, &event,
			sigmaS, sigmaA, gTexVal,
			material->volume.homogenous.phaseFunc);

	EvalStack_PushFloat3(result);
	EvalStack_PushFloat3(sampledDir);
	EvalStack_PushFloat(pdfW);
	EvalStack_PushBSDFEvent(event);
}

//------------------------------------------------------------------------------
// Material specific EvalOp
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE void HomogeneousVolMaterial_EvalOp(
		__global const Material* restrict material,
		const MaterialEvalOpType evalType,
		__global float *evalStack,
		uint *evalStackOffset,
		__global const HitPoint *hitPoint
		MATERIALS_PARAM_DECL) {
	switch (evalType) {
		case EVAL_ALBEDO:
			HomogeneousVolMaterial_Albedo(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_INTERIOR_VOLUME:
			HomogeneousVolMaterial_GetInteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EXTERIOR_VOLUME:
			HomogeneousVolMaterial_GetExteriorVolume(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_EMITTED_RADIANCE:
			HomogeneousVolMaterial_GetEmittedRadiance(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_GET_PASS_TROUGH_TRANSPARENCY:
			HomogeneousVolMaterial_GetPassThroughTransparency(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_EVALUATE:
			HomogeneousVolMaterial_Evaluate(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		case EVAL_SAMPLE:
			HomogeneousVolMaterial_Sample(material, hitPoint, evalStack, evalStackOffset MATERIALS_PARAM);
			break;
		default:
			// Something wrong here
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
