#line 2 "scene_funcs.cl"

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

// Sets the context of the ray that generated an hit point (read by the
// "rayinfo" texture). A NULL depthInfo means all depths are 0.
OPENCL_FORCE_INLINE void HitPoint_SetRayContext(__global HitPoint *hitPoint,
		const uint rayType, const BSDFEvent event,
		__global const PathDepthInfo *depthInfo, const float length) {
	hitPoint->rayEvent = event;
	hitPoint->rayFlags = rayType;
	hitPoint->rayDepth = depthInfo ? depthInfo->depth : 0u;
	hitPoint->rayDiffuseDepth = depthInfo ? depthInfo->diffuseDepth : 0u;
	hitPoint->rayGlossyDepth = depthInfo ? depthInfo->glossyDepth : 0u;
	hitPoint->raySpecularDepth = depthInfo ? depthInfo->specularDepth : 0u;
	hitPoint->rayTransmissionDepth = depthInfo ? depthInfo->transmitDepth : 0u;
	hitPoint->rayTransparentDepth = depthInfo ? depthInfo->transparentDepth : 0u;
	hitPoint->rayLength = length;
}

OPENCL_FORCE_NOT_INLINE bool Scene_Intersect(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const SceneRayType rayType,
		// The context of the incoming ray (used to fill BSDF->hitPoint ray
		// context fields, read by the "rayinfo" texture). A NULL
		// rayDepthInfo means all depths are 0. When not NULL, the
		// transparentDepth counter is updated while stepping through
		// pass-through materials (so it accumulates along the path).
		__global PathDepthInfo *rayDepthInfo,
		const BSDFEvent rayEvent,
		int *throughShadowTransparency,
		__global PathVolumeInfo *volInfo,
		__global HitPoint *tmpHitPoint,
		const float passThrough,
		__global Ray *ray,
		__global RayHit *rayHit,
		__global BSDF *bsdf,
		float3 *connectionThroughput,  const float3 pathThroughput,
		__global SampleResult *sampleResult,
		const bool backTracing
		MATERIALS_PARAM_DECL
		) {
	*connectionThroughput = WHITE;
	
	const bool cameraRay = rayType & CAMERA_RAY;
	const bool shadowRay = rayType & SHADOW_RAY;

	const bool hit = (rayHit->meshIndex != NULL_INDEX);

	uint rayVolumeIndex = volInfo->currentVolumeIndex;

#if defined(SLG_SPECTRAL)
	// Propagate the path wavelengths: the volume texture evals read
	// tmpHitPoint, everything shading-related reads bsdf->hitPoint.
	if (sampleResult) {
		tmpHitPoint->spectralW[0] = sampleResult->spectralW[0];
		tmpHitPoint->spectralW[1] = sampleResult->spectralW[1];
		tmpHitPoint->spectralW[2] = sampleResult->spectralW[2];
		tmpHitPoint->spectralHeroAlive = sampleResult->spectralHeroAlive;
	}
#endif

	if (hit) {
		// Initialize the BSDF of the hit point
		BSDF_Init(bsdf,
				*throughShadowTransparency,
				ray, rayHit,
				passThrough,
				volInfo
				MATERIALS_PARAM
				);
		// Fill the context of the ray that produced this hit point
		// (used by the "rayinfo" texture)
		HitPoint_SetRayContext(&bsdf->hitPoint, rayType, rayEvent,
				rayDepthInfo, rayHit->t);

#if defined(SLG_SPECTRAL)
		if (sampleResult) {
			bsdf->hitPoint.spectralW[0] = sampleResult->spectralW[0];
			bsdf->hitPoint.spectralW[1] = sampleResult->spectralW[1];
			bsdf->hitPoint.spectralW[2] = sampleResult->spectralW[2];
			bsdf->hitPoint.spectralHeroAlive = sampleResult->spectralHeroAlive;
		}
#endif

		rayVolumeIndex = bsdf->hitPoint.intoObject ? bsdf->hitPoint.exteriorVolumeIndex : bsdf->hitPoint.interiorVolumeIndex;
	} else if (rayVolumeIndex == NULL_INDEX) {
		// No volume information, I use the default volume
		rayVolumeIndex = scene->defaultVolumeIndex;
	}

	// Check if there is volume scatter event
	if (rayVolumeIndex != NULL_INDEX) {
		// This applies volume transmittance too
		// Note: by using passThrough here, I introduce subtle correlation
		// between scattering events and pass-through events
		float3 connectionEmission = BLACK;

		const float t = Volume_Scatter(&mats[rayVolumeIndex], ray,
				hit ? rayHit->t : ray->maxt,
				passThrough, volInfo->scatteredStart,
				connectionThroughput, &connectionEmission,
				tmpHitPoint
				TEXTURES_PARAM);

		// Add the volume emitted light to the appropriate light group
		if (!Spectrum_IsBlack(connectionEmission) && sampleResult)
			SampleResult_AddEmission(&taskConfig->film, sampleResult, BSDF_GetLightID(bsdf
				MATERIALS_PARAM),
					pathThroughput, connectionEmission);

		if (t > 0.f) {
			// There was a volume scatter event

			// I have to set RayHit fields even if there wasn't a real
			// ray hit
			rayHit->t = t;
			// This is a trick in order to have RayHit::Miss() return
			// false. I assume 0xfffffffeu will trigger a memory fault if
			// used (and the bug will be noticed)
			rayHit->meshIndex = 0xfffffffeu;

			BSDF_InitVolume(bsdf, *throughShadowTransparency, mats, ray, rayVolumeIndex, t, passThrough);
			HitPoint_SetRayContext(&bsdf->hitPoint, rayType, rayEvent,
					rayDepthInfo, t);
			volInfo->scatteredStart = true;

#if defined(SLG_SPECTRAL)
			if (sampleResult) {
				bsdf->hitPoint.spectralW[0] = sampleResult->spectralW[0];
				bsdf->hitPoint.spectralW[1] = sampleResult->spectralW[1];
				bsdf->hitPoint.spectralW[2] = sampleResult->spectralW[2];
				bsdf->hitPoint.spectralHeroAlive = sampleResult->spectralHeroAlive;
			}
#endif

			return false;
		}
	}

	if (hit) {
		bool continueToTrace =
			// Check if the volume priority system tells me to continue to trace the ray
			PathVolumeInfo_ContinueToTrace(volInfo, bsdf
				MATERIALS_PARAM) ||
			// Check if it is a camera invisible object and we are a tracing a camera ray
			(cameraRay && sceneObjs[rayHit->meshIndex].cameraInvisible);

		// Check if it is a pass through point
		if (!continueToTrace) {
			const float3 passThroughTrans = BSDF_GetPassThroughTransparency(bsdf, backTracing
				MATERIALS_PARAM);
			if (!Spectrum_IsBlack(passThroughTrans)) {
				*connectionThroughput *= passThroughTrans;

				// It is a pass through point, continue to trace the ray
				continueToTrace = true;
				// Account the crossed transparent surface for the
				// "rayinfo" texture (Cycles LightPath "Transparent Depth")
				if (rayDepthInfo)
					++(rayDepthInfo->transparentDepth);
			}
		}

		if (!continueToTrace && shadowRay) {
			const float3 shadowTransparency = BSDF_GetPassThroughShadowTransparency(bsdf
				MATERIALS_PARAM);

			if (!Spectrum_IsBlack(shadowTransparency)) {
				*connectionThroughput *= shadowTransparency;
				*throughShadowTransparency = true;
				continueToTrace = true;
				if (rayDepthInfo)
					++(rayDepthInfo->transparentDepth);
			}
		}

		if (continueToTrace) {
			// Update volume information
			const BSDFEvent eventTypes = BSDF_GetEventTypes(bsdf
						MATERIALS_PARAM);
			PathVolumeInfo_Update(volInfo, eventTypes, bsdf
					MATERIALS_PARAM);

			// It is a transparent material, continue to trace the ray
			ray->mint = rayHit->t + MachineEpsilon_E(rayHit->t);

			// A safety check in case of not enough numerical precision
			if ((ray->mint == rayHit->t) || (ray->mint >= ray->maxt))
				return false;
			else
				return true;
		} else
			return false;
	} else {
		// Nothing was hit, stop tracing the ray
		return false;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
