#line 2 "pathoclbase_funcs.cl"

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

// List of symbols defined at compile time:
//  PARAM_RAY_EPSILON_MIN
//  PARAM_RAY_EPSILON_MAX

/*void MangleMemory(__global unsigned char *ptr, const size_t size) {
	Seed seed;
	Rnd_Init(7 + get_global_id(0), &seed);

	for (uint i = 0; i < size; ++i)
		*ptr++ = (unsigned char)(Rnd_UintValue(&seed) & 0xff);
}*/

//------------------------------------------------------------------------------
// Init functions
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void InitSampleResult(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3,
		__global float *pixelFilterDistribution
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	SampleResult_Init(&taskConfig->film, sampleResult);

	float filmX = Sampler_GetSample(taskConfig, IDX_SCREEN_X SAMPLER_PARAM);
	float filmY = Sampler_GetSample(taskConfig, IDX_SCREEN_Y SAMPLER_PARAM);

	// Metropolis return IDX_SCREEN_X and IDX_SCREEN_Y between [0.0, 1.0] instead
	// that in film pixels like RANDOM and SOBOL samplers
	if (taskConfig->sampler.type == METROPOLIS) {
		filmX = filmSubRegion0 + filmX * (filmSubRegion1 - filmSubRegion0 + 1);
		filmY = filmSubRegion2 + filmY * (filmSubRegion3 - filmSubRegion2 + 1);
	}

	const uint pixelX = min(Floor2UInt(filmX), filmSubRegion1);
	const uint pixelY = min(Floor2UInt(filmY), filmSubRegion3);
	const float uSubPixelX = filmX - pixelX;
	const float uSubPixelY = filmY - pixelY;

	sampleResult->pixelX = pixelX;
	sampleResult->pixelY = pixelY;

	// Sample according the pixel filter distribution
	float distX, distY;
	FilterDistribution_SampleContinuous(&taskConfig->pixelFilter, pixelFilterDistribution,
			uSubPixelX, uSubPixelY, &distX, &distY);

	sampleResult->filmX = pixelX + .5f + distX;
	sampleResult->filmY = pixelY + .5f + distY;

	sampleResult->directShadowMask = 1.f;
	sampleResult->indirectShadowMask = 1.f;

	sampleResult->lastPathVertex = (taskConfig->pathTracer.maxPathDepth.depth == 1);
}

// Vertex connection (M6): the BiDirCPU MIS weighting function - power
// heuristic with beta=2 (bidircpu.h:71, SmallVCM convention)
OPENCL_FORCE_INLINE float VCMis(const float a) {
	return a * a;
}

// Vertex merging (M7): spatial hash cell index -> bucket id
// (VC_MERGE_BUCKETS is a power of two)
OPENCL_FORCE_INLINE uint VCMergeCellHash(const int cx, const int cy,
		const int cz) {
	uint h = (uint)cx * 0x8da6b343u ^ (uint)cy * 0xd8163841u ^
			(uint)cz * 0xcb1ab31fu;
	h ^= h >> 16;
	return h & (VC_MERGE_BUCKETS - 1u);
}

OPENCL_FORCE_INLINE void GenerateEyePath(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global const Camera* restrict camera,
		__global const float* restrict cameraBokehDistribution,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3,
		__global float *pixelFilterDistribution,
		__global Ray *ray,
		__global EyePathInfo *pathInfo
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		// cameraFilmWidth/cameraFilmHeight and filmWidth/filmHeight are usually
		// the same. They are different when doing tile rendering
		, const uint cameraFilmWidth, const uint cameraFilmHeight,
		const uint tileStartX, const uint tileStartY
#endif
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	EyePathInfo_Init(pathInfo);

	InitSampleResult(taskConfig,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1,
			filmSubRegion2, filmSubRegion3,
			pixelFilterDistribution
			SAMPLER_PARAM);

	// Generate the came ray
	const float timeSample = Sampler_GetSample(taskConfig, IDX_EYE_TIME SAMPLER_PARAM);

	const float dofSampleX = Sampler_GetSample(taskConfig, IDX_DOF_X SAMPLER_PARAM);
	const float dofSampleY = Sampler_GetSample(taskConfig, IDX_DOF_Y SAMPLER_PARAM);

#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
	Camera_GenerateRay(camera, cameraBokehDistribution, cameraFilmWidth, cameraFilmHeight,
			ray,
			&pathInfo->volume,
			sampleResult->filmX + tileStartX, sampleResult->filmY + tileStartY,
			timeSample,
			dofSampleX, dofSampleY);
#else
	Camera_GenerateRay(camera, cameraBokehDistribution, filmWidth, filmHeight,
			ray,
			&pathInfo->volume,
			sampleResult->filmX, sampleResult->filmY,
			timeSample,
			dofSampleX, dofSampleY);
#endif

	if (taskConfig->pathTracer.vertexConnect.enabled) {
		// Vertex connection (M6) eye-prefix init (CPU BIDIR eye loop):
		// dVCM = MIS(1/cameraPdfW), dVC = 0. The GPU cameraPdfW is the
		// importance density only - the DoF lens pdf term of the CPU
		// GetPDF is not folded (exact for pinhole cameras).
		float cameraPdfW, fluxToRadianceFactor;
		if (Camera_GetPDF(camera, ray, 0.f, &cameraPdfW,
				&fluxToRadianceFactor) && (cameraPdfW > 0.f))
			pathInfo->dVCM = VCMis(1.f / cameraPdfW);
		// With vertex merging on the camera vertex itself is a valid
		// merge/connect endpoint (BiDirVMCPU convention: dVC = dVM = 1);
		// without it BIDIRCPU's pure-BPT init is dVC = dVM = 0
		const bool vcMerge = taskConfig->pathTracer.vertexConnect.
				mergeEnable != 0u;
		pathInfo->dVC = vcMerge ? 1.f : 0.f;
		pathInfo->dVM = vcMerge ? 1.f : 0.f;
	}

	// Initialize the path state
	taskState->state = MK_RT_NEXT_VERTEX;
	VSTORE3F(WHITE, taskState->throughput.c);
	taskState->albedoToDo = true;
	VSTORE3F(BLACK, sampleResult->albedo.c);  // Just in case albedoToDo is never true
	VSTORE3F(BLACK, &sampleResult->shadingNormal.x);
	taskState->photonGICacheEnabledOnLastHit = false;
	taskState->photonGICausticCacheUsed = false;
	taskState->photonGIShowIndirectPathMixUsed = false;
	// Initialize the trough a shadow transparency flag used by Scene_Intersect()
	taskState->throughShadowTransparency = false;

	// Initialize the pass-through event seed
	//
	// Note: using the IDX_PASSTHROUGH of path depth 0
	const float passThroughEvent = Sampler_GetSample(taskConfig, IDX_BSDF_OFFSET + IDX_PASSTHROUGH SAMPLER_PARAM);
	Seed seedPassThroughEvent;
	Rnd_InitFloat(passThroughEvent, &seedPassThroughEvent);
	taskState->seedPassThroughEvent = seedPassThroughEvent;

#if defined(SLG_SPECTRAL)
	// Hero-wavelength spectral transport: draw the shared wavelength offset
	// (extra boot dimension) and store the stratified wavelengths in the
	// SampleResult -- every downstream funnel (Scene_Intersect, emission,
	// film splat) reaches them from there.
	const float wavelengthSample = Sampler_GetSample(taskConfig, IDX_WAVELENGTH SAMPLER_PARAM);
	float w[SLG_SPECTRAL_BINS];
	const uint hero = Spectral_SampleWavelengths(wavelengthSample, w);
	for (uint i = 0; i < SLG_SPECTRAL_BINS; ++i)
		sampleResult->spectralW[i] = w[i];
	sampleResult->spectralHeroAlive = SLG_SW_DEFAULT | (hero << SLG_SW_HERO_SHIFT);
#endif
}

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE bool CheckDirectHitVisibilityFlags(__global const LightSource* restrict lightSource,
		__global PathDepthInfo *depthInfo,
		const BSDFEvent lastBSDFEvent) {
	if (depthInfo->depth == 0)
		return true;

	if ((lastBSDFEvent & DIFFUSE) && (lightSource->visibility & DIFFUSE))
		return true;
	if ((lastBSDFEvent & GLOSSY) && (lightSource->visibility & GLOSSY))
		return true;
	if ((lastBSDFEvent & SPECULAR) && (lightSource->visibility & SPECULAR))
		return true;

	return false;
}

OPENCL_FORCE_INLINE void DirectHitInfiniteLight(__constant const Film* restrict film,
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const float sceneRadius,
		__global const float* restrict emitLightsDistribution,
		__global EyePathInfo *pathInfo, __global const Spectrum* restrict pathThroughput,
		const __global Ray *ray, __global const BSDF *bsdf, __global SampleResult *sampleResult
		LIGHTS_PARAM_DECL) {
	// If the material is shadow transparent, Direct Light sampling
	// will take care of transporting all emitted light
	if (bsdf && bsdf->hitPoint.throughShadowTransparency)
		return;

	const float3 throughput = VLOAD3F(pathThroughput->c);
	const bool vcEnabled = taskConfig->pathTracer.vertexConnect.enabled;

	for (uint i = 0; i < envLightCount; ++i) {
		__global const LightSource* restrict light = &lights[envLightIndices[i]];

		// Check if the light source is visible according the settings and
		// linked to the previous (receiving) vertex - light linking
		if (!CheckDirectHitVisibilityFlags(light, &pathInfo->depth, pathInfo->lastBSDFEvent) ||
				((light->linkMask != 0ull) &&
				((light->linkMask & pathInfo->linkAcceptMask) == 0ull)))
			continue;

		float directPdfW;
		float emissionPdfW = 0.f;
		const float3 envRadianceRGB = EnvLight_GetRadiance(light, sceneRadius, bsdf,
				-VLOAD3F(&ray->d.x), &directPdfW,
				vcEnabled ? &emissionPdfW : NULL
				LIGHTS_PARAM);
#if defined(SLG_SPECTRAL)
		// Env lights carry baked RGB radiance: upsample to the path bins
		// with the illuminant basis at this funnel (bsdf may be a miss
		// path, so the wavelengths come from the SampleResult).
		const float3 envRadiance = Spectral_Upsample(envRadianceRGB,
				sampleResult->spectralW, sampleResult->spectralHeroAlive, true,
				spectralUpsamplingTable);
#else
		const float3 envRadiance = envRadianceRGB;
#endif

		if (!Spectrum_IsBlack(envRadiance)) {
			float weight;
			if (vcEnabled) {
				// Vertex connection (M6): CPU DirectHitLight - the first
				// eye vertex (depth == 1) carries no MIS; deeper vertices
				// use the emit-strategy pick pdf:
				//   weightCamera = MIS(directPdfA*pick) * dVCM
				//       + MIS(emissionPdfW*pick) * dVC
				//   misWeight = 1 / (weightCamera + 1)
				if (sampleResult->firstPathVertex)
					weight = 1.f;
				else {
					const float lightPickProb = emitLightsDistribution ?
							Distribution1D_PdfDiscrete(emitLightsDistribution,
									envLightIndices[i]) : 0.f;
					const float weightCamera =
							VCMis(directPdfW * lightPickProb) * pathInfo->dVCM +
							VCMis(emissionPdfW * lightPickProb) * pathInfo->dVC;
					weight = 1.f / (weightCamera + 1.f);
				}
			} else if (!(pathInfo->lastBSDFEvent & SPECULAR)) {
				// The previous vertex picked its NEE distribution: a
				// shadow-catcher-only-infinite vertex used the infinite
				// distribution (no DLSC/light-BVH lookup - CPU parity)
				const float lightPickProb = LightStrategy_SampleLightPdf(
						pathInfo->lastOnlyInfiniteLights ?
								infiniteLightSourcesDistribution : lightsDistribution,
						pathInfo->lastOnlyInfiniteLights ? NULL : dlscAllEntries,
						dlscDistributions, dlscBVHNodes,
						dlscRadius2, dlscNormalCosAngle,
						pathInfo->lastOnlyInfiniteLights ? NULL : lightBVHNodes,
						lightBVHLightToLeaf, lightBVHMinDist2,
						VLOAD3F(&ray->o.x), VLOAD3F(&pathInfo->lastShadeN.x),
						pathInfo->lastFromVolume,
						light->lightSceneIndex);

				// MIS between BSDF sampling and direct light sampling
				weight = PowerHeuristic(pathInfo->lastBSDFPdfW, directPdfW * lightPickProb);
			} else
				weight = 1.f;

			SampleResult_AddEmission(film, sampleResult, light->lightID, throughput, weight * envRadiance);
		}
	}
}

OPENCL_FORCE_INLINE void DirectHitFiniteLight(__constant const Film* restrict film,
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global const float* restrict emitLightsDistribution,
		__global EyePathInfo *pathInfo,
		__global const Spectrum* restrict pathThroughput, const __global Ray *ray,
		const float distance, __global const BSDF *bsdf,
		__global SampleResult *sampleResult
		LIGHTS_PARAM_DECL) {
	__global const LightSource* restrict light = &lights[bsdf->triangleLightSourceIndex];

	// Check if the light source is visible according the settings and
	// linked to the previous (receiving) vertex - light linking
	if (!CheckDirectHitVisibilityFlags(light, &pathInfo->depth, pathInfo->lastBSDFEvent) ||
			((light->linkMask != 0ull) &&
			((light->linkMask & pathInfo->linkAcceptMask) == 0ull)) ||
			// If the material is shadow transparent, Direct Light sampling
			// will take care of transporting all emitted light
			bsdf->hitPoint.throughShadowTransparency)
		return;

	float directPdfA;
	float emissionPdfW = 0.f;
	const bool vcEnabled = taskConfig->pathTracer.vertexConnect.enabled;
	const float3 emittedRadiance = BSDF_GetEmittedRadiance(bsdf, &directPdfA,
			vcEnabled ? &emissionPdfW : NULL
			LIGHTS_PARAM);

	if (!Spectrum_IsBlack(emittedRadiance)) {
		// Add emitted radiance
		float weight = 1.f;
		if (vcEnabled) {
			// Vertex connection (M6): CPU DirectHitLight - directPdfA
			// stays in AREA measure here (CPU parity), the pick pdf is
			// the emit strategy's
			if (sampleResult->firstPathVertex)
				weight = 1.f;
			else {
				const float lightPickProb = emitLightsDistribution ?
						Distribution1D_PdfDiscrete(emitLightsDistribution,
								bsdf->triangleLightSourceIndex) : 0.f;
				const float weightCamera =
						VCMis(directPdfA * lightPickProb) * pathInfo->dVCM +
						VCMis(emissionPdfW * lightPickProb) * pathInfo->dVC;
				weight = 1.f / (weightCamera + 1.f);
			}
		} else if (!(pathInfo->lastBSDFEvent & SPECULAR)) {
			// Same distribution the previous vertex's NEE drew from:
			// shadow-catcher-only-infinite vertices use the infinite
			// distribution (no DLSC/light-BVH lookup - CPU parity)
			const float lightPickProb = LightStrategy_SampleLightPdf(
					pathInfo->lastOnlyInfiniteLights ?
							infiniteLightSourcesDistribution : lightsDistribution,
					pathInfo->lastOnlyInfiniteLights ? NULL : dlscAllEntries,
					dlscDistributions, dlscBVHNodes,
					dlscRadius2, dlscNormalCosAngle,
					pathInfo->lastOnlyInfiniteLights ? NULL : lightBVHNodes,
					lightBVHLightToLeaf, lightBVHMinDist2,
					VLOAD3F(&ray->o.x), VLOAD3F(&pathInfo->lastShadeN.x),
					pathInfo->lastFromVolume,
					light->lightSceneIndex);

#if !defined(RENDER_ENGINE_RTPATHOCL)
			// This is a specific check to avoid fireflies with DLSC. It
			// must not fire when the zero pick pdf comes from the
			// infinite-lights-only restriction: the BSDF hit then has the
			// sole coverage of this light and deserves weight 1, not a drop
			if (!pathInfo->lastOnlyInfiniteLights &&
					(lightPickProb == 0.f) && light->isDirectLightSamplingEnabled && dlscAllEntries)
				return;
#endif
			
			const float directPdfW = PdfAtoW(directPdfA, distance,
					fabs(dot(VLOAD3F(&bsdf->hitPoint.fixedDir.x), VLOAD3F(&bsdf->hitPoint.shadeN.x))));

			// MIS between BSDF sampling and direct light sampling
			//
			// Note: mats[bsdf->materialIndex].avgPassThroughTransparency = lightSource->GetAvgPassThroughTransparency()
			weight = PowerHeuristic(pathInfo->lastBSDFPdfW * Light_GetAvgPassThroughTransparency(light LIGHTS_PARAM), directPdfW * lightPickProb);
		}

		SampleResult_AddEmission(film, sampleResult, BSDF_GetLightID(bsdf
				MATERIALS_PARAM), VLOAD3F(pathThroughput->c), weight * emittedRadiance);
	}
}

OPENCL_FORCE_INLINE float RussianRouletteProb(const float importanceCap, const float3 color) {
	return clamp(Spectrum_Filter(color), importanceCap, 1.f);
}

//----------------------------------------------------------------------
// ReSTIR DI shared tail: merge the stored temporal reservoir into the
// fresh reservoir, cap the reuse count, and store the merged state back
// into the per-pixel reservoir slot. Extracted so the unshadowed path
// (DirectLight_Illuminate) and the visibility-weighted path
// (DirectLight_RestirResolveVisibility) share identical merge semantics.
//
// IMPORTANT: the stored reservoir holds the PRE-spatial-merge state.
// Storing post-spatial totals lets a pixel's inflated wSum feed back
// into its neighbours' merges on the next pass, which compounds and
// explodes (the same feedback pathology the CPU world-grid port showed
// - see e18 and the 1e9 wSum traps). Screen-space neighbours therefore
// always read each other's post-temporal reservoirs, never a merged
// one; the caller runs Restir_SpatialMergePixels() afterwards on the
// accumulators and computes risScale = wSum / (M * target) itself.
//
// In/out parameters carry the current selection accumulators:
//   ioLightIndex/ioPickPdf/ioTarget - winning sample
//   ioWSum/ioMTotal                 - weight sum / draw count (updated
//                                   in place by the temporal merge)
//   ioLSU/ioLSV/ioLSP               - winning sample's light-surface
//                                   draws (stored + replayed for the
//                                   reconnection shift, E2c)
//----------------------------------------------------------------------
OPENCL_FORCE_INLINE void Restir_MergeStore(
		uint *ioLightIndex, float *ioPickPdf, float *ioTarget,
		float *ioWSum, uint *ioMTotal,
		float *ioLSU, float *ioLSV, float *ioLSP,
		__global const float* restrict lightDist,
		const float hitPointX, const float hitPointY, const float hitPointZ,
		const float geomNX, const float geomNY, const float geomNZ,
		const uint M, const float u0,
		const int restirTemporalEnable, const int restirStoreEnable,
		const uint restirPixelIndex, __global RestirReservoir *restirReservoirs,
		bool *outSampleIsFresh
		LIGHTS_PARAM_DECL) {
	uint curLightIndex = *ioLightIndex;
	float curPickPdf = *ioPickPdf;
	float curTarget = *ioTarget;
	float curLSU = *ioLSU;
	float curLSV = *ioLSV;
	float curLSP = *ioLSP;
	float wSumTotal = *ioWSum;
	uint MTotal = *ioMTotal;
	// *outSampleIsFresh is in/out: the caller initializes it to whether
	// the current winner is a fresh candidate whose exact light-surface
	// sample is still available; a swapped-in stored winner clears it.
	// The visibility-weighted path uses this to decide between exact-ray
	// reuse and re-evaluation.

	//----------------------------------------------------------------------
	// Temporal reuse: the pixel's stored previous-pass reservoir is
	// merged as its own block of proposal draws from the same
	// distribution q. The merge uses the stored wSum/M (the REAL
	// accumulated state), not "M * target": the stored sample is a
	// random output of a whole set of draws, so weighting it with a
	// function of itself correlates the merge weight with the sample
	// and biases the estimator. With the stored wSum/M the combined
	// reservoir is the RIS of M + M_prev proposal draws - unbiased.
	// The stored target was evaluated at the previous position: on
	// static scenes it matches the current one exactly; on dynamic
	// scenes the mismatch introduces the usual small, bounded
	// temporal bias of ReSTIR.
	//----------------------------------------------------------------------
	if (restirTemporalEnable) {
		__global RestirReservoir *reservoir = &restirReservoirs[restirPixelIndex];
		if ((reservoir->lightIndex != NULL_INDEX) && (reservoir->wSum > 0.f)) {
			wSumTotal += reservoir->wSum;
			MTotal += reservoir->M;

			const float prevPickPdf =
					Distribution1D_PdfDiscrete(lightDist, reservoir->lightIndex);
			if (prevPickPdf > 0.f) {
				const float r = SobolSequence_BlueNoiseHash(
						SobolSequence_BlueNoiseHash(as_uint(u0) ^ 0xc2b2ae35u) ^
						(0x9E3779B9u + M)) * (1.f / 4294967296.f);
				if (r < reservoir->wSum / wSumTotal) {
					curLightIndex = reservoir->lightIndex;
					curPickPdf = prevPickPdf;
					curTarget = reservoir->target;
					curLSU = reservoir->lsU;
					curLSV = reservoir->lsV;
					curLSP = reservoir->lsP;
					*outSampleIsFresh = false;
				}
			}
		}
	}

	// Cap the reuse count at 2x the candidate count (CPU port): a
	// runaway M would let stale winners dominate the merge. The wSum
	// rescale keeps W = wSum/M exact.
	const uint mCap = 2u * M;
	if (MTotal > mCap) {
		wSumTotal *= (float)mCap / (float)MTotal;
		MTotal = mCap;
	}

	//------------------------------------------------------------------
	// Store the post-temporal reservoir for future passes and for this
	// pass's screen-space neighbour merges. Depth-0 vertices refresh
	// the slot (it always represents a primary-hit reservoir; deeper
	// vertices still merge with it, they just do not overwrite it).
	// The hit point / geometric normal go along for the spatial merge's
	// same-surface gate (E2b). The store runs when EITHER temporal or
	// spatial reuse is on (the caller ORs the two).
	//------------------------------------------------------------------
	if (restirStoreEnable) {
		__global RestirReservoir *reservoir = &restirReservoirs[restirPixelIndex];
		reservoir->lightIndex = curLightIndex;
		reservoir->wSum = wSumTotal;
		reservoir->M = MTotal;
		reservoir->target = curTarget;
		reservoir->hitP[0] = hitPointX;
		reservoir->hitP[1] = hitPointY;
		reservoir->hitP[2] = hitPointZ;
		reservoir->geomN[0] = geomNX;
		reservoir->geomN[1] = geomNY;
		reservoir->geomN[2] = geomNZ;
		reservoir->lsU = curLSU;
		reservoir->lsV = curLSV;
		reservoir->lsP = curLSP;
	}

	*ioLightIndex = curLightIndex;
	*ioPickPdf = curPickPdf;
	*ioTarget = curTarget;
	*ioLSU = curLSU;
	*ioLSV = curLSV;
	*ioLSP = curLSP;
	*ioWSum = wSumTotal;
	*ioMTotal = MTotal;
}

//----------------------------------------------------------------------
// ReSTIR DI spatial reuse (E2b): screen-space neighbour-pixel merge.
//
// Draws up to RESTIR_PIXEL_MERGES_MAX pseudo-random neighbour pixels in a
// 5x5 window (centre excluded) and merges their stored reservoirs with
// the same GRIS combine as the CPU spatial merge:
//   b_nbr = wSum_nbr * (pi_new / pi_old)
// where pi_new is the stored winner's target re-evaluated at THIS
// shade point and pi_old is its stored target. Neighbouring pixels
// mostly shade the same surface, so the ratio stays near 1 and reuse
// actually pays - the previous world-space hash grid merged unrelated
// surfaces and measured ~1.1-1.3x WORSE RMSE on manylights (e14).
//
// A same-surface geometric gate (hit-point distance < 2% of the world
// radius AND landing-normal within ~25 degrees) rejects silhouette
// neighbours whose targets do not transfer; it also keeps the ratio
// bounded, which is load-bearing: without it a stale tiny pi_old entry
// can amplify wSum through repeated merges (the CPU merge-explosion
// pathology). Reservoirs are advisory data shared between tasks: a
// torn read yields a bounded wrong-weight merge, never a crash or NaN.
//
// In/out accumulators mirror Restir_MergeStore; *ioWinnerIsFresh is
// cleared when a merged neighbour displaces the current winner (the
// stored sample's light-surface sample no longer exists, so the
// visibility path must re-evaluate it with the caller's own sample).
//----------------------------------------------------------------------
OPENCL_FORCE_INLINE void Restir_SpatialMergePixels(
		uint *ioLightIndex, float *ioPickPdf, float *ioTarget,
		float *ioWSum, uint *ioMTotal,
		float *ioLSU, float *ioLSV, float *ioLSP,
		__global const float* restrict lightDist,
		__global const BSDF *bsdf,
		__global Ray *shadowRay,
		const float time, const float u0,
		const float worldCenterX, const float worldCenterY, const float worldCenterZ,
		const float worldRadius,
		__global HitPoint *tmpHitPoint,
		const uint pixelIndex, const uint filmWidth,
		__global RestirReservoir *restirReservoirs,
		const uint reservoirCount,
		uint *mergeCount, bool *ioWinnerIsFresh
		LIGHTS_PARAM_DECL) {
	// No distribution compiled (e.g. no lights): nothing to merge
	if (!lightDist)
		return;
	const uint gridLightCount = as_uint(lightDist[0]);
	const float3 curGeomN = BSDF_GetLandingGeometryN(bsdf);
	// The caller selected lightDist; when it is the infinite
	// distribution (shadow-catcher-only-infinite vertex) the DLSC
	// lookup does not apply (CPU parity)
	const bool onlyInfLights =
			BSDF_IsShadowCatcherOnlyInfiniteLights(bsdf MATERIALS_PARAM);
	const float3 curShadeN = BSDF_GetLandingShadeN(bsdf);
	const float maxDist2 = RESTIR_PIXEL_MERGE_DIST2 * worldRadius * worldRadius;

	const uint px = pixelIndex % filmWidth;
	const uint py = pixelIndex / filmWidth;

	for (uint k = 0; k < RESTIR_PIXEL_MERGES_MAX; ++k) {
		// Pseudo-random neighbour offset in a 5x5 window, hashed off
		// (u0, pixelIndex, k) so different tasks and passes see
		// different neighbours
		const uint h = SobolSequence_BlueNoiseHash(
				as_uint(u0) ^ (pixelIndex * 0x9E3779B9u + k * 0x85EBCA6Bu));
		uint off = h % 25u;
		if (off == 12u)
			off = 24u; // skip the centre cell (self)
		const int nx = (int)px + (int)(off % 5u) - 2;
		const int ny = (int)py + (int)(off / 5u) - 2;
		if ((nx < 0) || (ny < 0) || ((uint)nx >= filmWidth))
			continue;
		const uint nIdx = (uint)ny * filmWidth + (uint)nx;
		if (nIdx >= reservoirCount)
			continue;

		__global RestirReservoir *entry = &restirReservoirs[nIdx];
		const uint eLightIndex = entry->lightIndex;
		const float eWSum = entry->wSum;
		const uint eM = entry->M;
		const float eTarget = entry->target;
		if ((eLightIndex == NULL_INDEX) || (eLightIndex >= gridLightCount) ||
				(eWSum <= 0.f) || (eM == 0u) || (eTarget <= 0.f))
			continue;

		// Representative-winner gate (the old world-grid applied this at
		// store time): a stored winner whose target is a tiny fraction of
		// its reservoir's mean mass is a fluke - re-evaluating it here
		// yields a large pi_new/pi_old ratio that explodes bNbr and the
		// resulting risScale (observed as ~20x RMSE hot pixels on e14's
		// spots scene). Filtering on the merge side keeps the temporal
		// reservoir's store unconditional.
		if (eTarget < 0.05f * eWSum / (float)eM)
			continue;

		// Same-surface geometric gate: positions within 2% of the world
		// radius and landing normals within ~25 degrees.
		const float ddx = entry->hitP[0] - bsdf->hitPoint.p.x;
		const float ddy = entry->hitP[1] - bsdf->hitPoint.p.y;
		const float ddz = entry->hitP[2] - bsdf->hitPoint.p.z;
		if (ddx * ddx + ddy * ddy + ddz * ddz > maxDist2)
			continue;
		const float dn = entry->geomN[0] * curGeomN.x +
				entry->geomN[1] * curGeomN.y +
				entry->geomN[2] * curGeomN.z;
		if (dn < RESTIR_PIXEL_MERGE_NORM)
			continue;

		const float nbPickPdf = LightStrategy_SampleLightPdf(
				lightDist,
				onlyInfLights ? NULL : dlscAllEntries,
				dlscDistributions, dlscBVHNodes,
				dlscRadius2, dlscNormalCosAngle,
				onlyInfLights ? NULL : lightBVHNodes,
				lightBVHLightToLeaf, lightBVHMinDist2,
				VLOAD3F(&bsdf->hitPoint.p.x), curShadeN,
				bsdf->isVolume,
				eLightIndex);
		if (nbPickPdf <= 0.f)
			continue;

		// NOTE: the merge Illuminate() writes into the global
		// shadowRay scratch buffer; the final Illuminate() in the
		// caller overwrites it with the winning candidate's ray.
		// The neighbour's stored light-surface sample is replayed here
		// (reconnection shift): pi_new is evaluated at the same light
		// point that produced pi_old, so the ratio tracks the shading
		// difference only.
		float nbPdfW;
		const float3 nbRadiance = Light_Illuminate(
				&lights[eLightIndex],
				bsdf,
				time, entry->lsU, entry->lsV, entry->lsP,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				tmpHitPoint,
				shadowRay, &nbPdfW, NULL, NULL
				LIGHTS_PARAM);
		if (Spectrum_IsBlack(nbRadiance) || (nbPdfW <= 0.f))
			continue;

		// Target re-evaluated HERE (pi_new); the entry's stored target
		// is pi_old at the storing point.
		const float nbTarget = Spectrum_Y(nbRadiance) /
				(nbPickPdf * nbPdfW);
		if (nbTarget <= 0.f)
			continue;

		// GRIS combine weight, unclamped - CPU parity: capping or
		// dropping the merge by its ratio is a data-dependent weight
		// distortion and biases the estimator dark (restirdi.cpp).
		// The outlier pathology is handled by the representative-winner
		// gate above, which both sides share.
		const float bNbr = eWSum * (nbTarget / eTarget);
		*ioWSum += bNbr;
		*ioMTotal += eM;
		++(*mergeCount);

		const uint acceptSeed = SobolSequence_BlueNoiseHash(
				as_uint(u0) ^ (0x45d9f3bu + k));
		const float r = SobolSequence_BlueNoiseHash(
				acceptSeed ^ (k * 0x85EBCA6Bu)) * (1.f / 4294967296.f);
		if ((*ioLightIndex == NULL_INDEX) || (r < bNbr / *ioWSum)) {
			*ioLightIndex = eLightIndex;
			*ioPickPdf = nbPickPdf;
			*ioTarget = nbTarget;
			*ioLSU = entry->lsU;
			*ioLSV = entry->lsV;
			*ioLSP = entry->lsP;
			*ioWinnerIsFresh = false;
		}
	}
}

//----------------------------------------------------------------------
// ReSTIR DI visibility-weighted target (E2a), phase 1.
//
// Draws visCandCount proposals from q, evaluates each candidate's
// unshadowed contribution with Light_Illuminate() and writes the
// candidate's shadow ray into candRays[i] plus a full
// RestirVisCandidate record into candData[i] (light index, proposal
// pdf, direct pdf, unshadowed target and the radiance itself).
// Returns true when at least one candidate produced a non-black
// contribution; the MK_RT_RESTIR state then resolves the reservoir on
// the next iteration, once the queued rays have been traced.
//
// The candidate's own light-surface sample IS the contribution
// sample: if it wins the merge, its stored radiance/pdf/ray are
// reused verbatim. Re-sampling the winner would make the binary V
// test cover a different light point than the payoff - a real bias
// on area/emissive-mesh lights (measured as a systematic darkening
// in the e16 regression test).
//----------------------------------------------------------------------
OPENCL_FORCE_NOT_INLINE bool DirectLight_RestirEnqueueVisibility(
		__global const BSDF *bsdf,
		__global Ray *candRays,
		__global RestirVisCandidate *candData,
		const uint visCandCount,
		const float worldCenterX,
		const float worldCenterY,
		const float worldCenterZ,
		const float worldRadius,
		__global HitPoint *tmpHitPoint,
		const float time, const float u0,
		const int restirSpatialEnable,
		const uint restirPixelIndex,
		const uint filmWidth, const uint reservoirCount,
		__global RestirReservoir *restirReservoirs
		LIGHTS_PARAM_DECL) {
	const bool onlyInfLights = BSDF_IsShadowCatcherOnlyInfiniteLights(bsdf MATERIALS_PARAM);
	__global const float* restrict lightDist = onlyInfLights ?
			infiniteLightSourcesDistribution : lightsDistribution;
	// The infinite distribution has no DLSC/light-BVH coverage (CPU parity)
	__global const DLSCacheEntry* restrict dlscEntries =
			onlyInfLights ? NULL : dlscAllEntries;
	__global const LightBVHNode* restrict lightBVH =
			onlyInfLights ? NULL : lightBVHNodes;
	const float3 landingShadeN = BSDF_GetLandingShadeN(bsdf);

	bool anyValid = false;
	for (uint i = 0; i < visCandCount; ++i) {
		const float u_i = fmod(u0 + i * (1.f / visCandCount), 1.f);

		// The candidate's light-surface sample doubles as the eventual
		// contribution sample (no winner re-sampling - that would break
		// the consistency between the binary visibility of the queued
		// ray and the evaluated radiance). So each candidate needs a
		// proper independent 3D sample, hashed off (u0, i), instead of
		// the (u_i, 0, 0) shorthand used by the unshadowed path - the
		// latter would degenerate to a fixed point on area lights.
		const uint h = SobolSequence_BlueNoiseHash(
				as_uint(u0) ^ (i * 0x9E3779B9u + 0x27D4EB2Fu));
		const float su = SobolSequence_BlueNoiseHash(h ^ 0x165667B1u) *
				(1.f / 4294967296.f);
		const float sv = SobolSequence_BlueNoiseHash(h ^ 0x9E3779B9u) *
				(1.f / 4294967296.f);
		const float sp = SobolSequence_BlueNoiseHash(h ^ 0x85EBCA6Bu) *
				(1.f / 4294967296.f);

		float candPickPdf = 0.f;
		const uint candIndex = LightStrategy_SampleLights(lightDist,
				dlscEntries,
				dlscDistributions, dlscBVHNodes,
				dlscRadius2, dlscNormalCosAngle,
				lightBVH, lightBVHLightToLeaf, lightBVHMinDist2,
				VLOAD3F(&bsdf->hitPoint.p.x), landingShadeN,
				bsdf->isVolume,
				u_i, &candPickPdf);

		candData[i].lightIndex = NULL_INDEX;
		candData[i].pickPdf = candPickPdf;
		candData[i].directPdfW = 0.f;
		candData[i].target = 0.f;
		candData[i].radianceR = 0.f;
		candData[i].radianceG = 0.f;
		candData[i].radianceB = 0.f;

		if ((candIndex == NULL_INDEX) || (candPickPdf <= 0.f)) {
			// Masked: both the MetalRT intersector and the software
			// kernels skip masked rays entirely. All fields are still
			// initialized - a garbage ray could produce NaNs if the
			// flag handling ever changed.
			Ray_Init4(&candRays[i], VLOAD3F(&bsdf->hitPoint.p.x),
					(float3)(0.f, 0.f, 1.f), 0.f, 0.f, time);
			candRays[i].flags = RAY_FLAGS_MASKED;
			continue;
		}

		float candPdfW;
		const float3 candRadiance = Light_Illuminate(
				&lights[candIndex],
				bsdf,
				time, su, sv, sp,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				tmpHitPoint,
				&candRays[i], &candPdfW, NULL, NULL
				LIGHTS_PARAM);
		if (Spectrum_IsBlack(candRadiance) || (candPdfW <= 0.f)) {
			Ray_Init4(&candRays[i], VLOAD3F(&bsdf->hitPoint.p.x),
					(float3)(0.f, 0.f, 1.f), 0.f, 0.f, time);
			candRays[i].flags = RAY_FLAGS_MASKED;
			continue;
		}

		candData[i].lightIndex = candIndex;
		candData[i].directPdfW = candPdfW;
		candData[i].target = Spectrum_Y(candRadiance) /
				(candPickPdf * candPdfW);
		candData[i].radianceR = candRadiance.x;
		candData[i].radianceG = candRadiance.y;
		candData[i].radianceB = candRadiance.z;
		candData[i].lsU = su;
		candData[i].lsV = sv;
		candData[i].lsP = sp;
		anyValid = true;
	}

	//------------------------------------------------------------------
	// Merge candidates (E2d visibility-aware spatial shift): slots
	// [visCandCount, visCandCount + RESTIR_PIXEL_MERGES_MAX) replay the
	// same gated neighbour reservoirs the unshadowed path merges - the
	// same-surface + representative-winner gates are identical - but the
	// ray rides the tail with the fresh candidates, so the resolve step
	// folds REAL visibility into pi_new instead of the V-free
	// approximation. The neighbour state is read here (pre-store), i.e.
	// it is the previous pass's stored reservoir; cross-task races are
	// the same ones the unshadowed merge already tolerates.
	//------------------------------------------------------------------
	for (uint j = 0; j < RESTIR_PIXEL_MERGES_MAX; ++j) {
		const uint slot = visCandCount + j;
		// Default: masked ray + empty record (merge simply does not
		// happen - consistent with the gate-fail continues below)
		candData[slot].lightIndex = NULL_INDEX;
		Ray_Init4(&candRays[slot], VLOAD3F(&bsdf->hitPoint.p.x),
				(float3)(0.f, 0.f, 1.f), 0.f, 0.f, time);
		candRays[slot].flags = RAY_FLAGS_MASKED;

		if (!restirSpatialEnable || !lightDist)
			continue;

		const uint gridLightCount = as_uint(lightDist[0]);
		const uint h2 = SobolSequence_BlueNoiseHash(
				as_uint(u0) ^ (restirPixelIndex * 0x9E3779B9u + j * 0x85EBCA6Bu));
		uint off = h2 % 25u;
		if (off == 12u)
			off = 24u; // skip the centre cell (self)
		const uint px = restirPixelIndex % filmWidth;
		const uint py = restirPixelIndex / filmWidth;
		const int nx = (int)px + (int)(off % 5u) - 2;
		const int ny = (int)py + (int)(off / 5u) - 2;
		if ((nx < 0) || (ny < 0) || ((uint)nx >= filmWidth))
			continue;
		const uint nIdx = (uint)ny * filmWidth + (uint)nx;
		if (nIdx >= reservoirCount)
			continue;

		__global RestirReservoir *entry = &restirReservoirs[nIdx];
		const uint eLightIndex = entry->lightIndex;
		const float eWSum = entry->wSum;
		const uint eM = entry->M;
		const float eTarget = entry->target;
		if ((eLightIndex == NULL_INDEX) || (eLightIndex >= gridLightCount) ||
				(eWSum <= 0.f) || (eM == 0u) || (eTarget <= 0.f))
			continue;
		// Representative-winner gate (same as the unshadowed merge)
		if (eTarget < 0.05f * eWSum / (float)eM)
			continue;
		// Same-surface geometric gate
		const float ddx = entry->hitP[0] - bsdf->hitPoint.p.x;
		const float ddy = entry->hitP[1] - bsdf->hitPoint.p.y;
		const float ddz = entry->hitP[2] - bsdf->hitPoint.p.z;
		if (ddx * ddx + ddy * ddy + ddz * ddz >
				RESTIR_PIXEL_MERGE_DIST2 * worldRadius * worldRadius)
			continue;
		const float3 curGeomN = BSDF_GetLandingGeometryN(bsdf);
		const float dn = entry->geomN[0] * curGeomN.x +
				entry->geomN[1] * curGeomN.y +
				entry->geomN[2] * curGeomN.z;
		if (dn < RESTIR_PIXEL_MERGE_NORM)
			continue;

		// Same distribution the fresh stream draws from (shadow-catcher
		// aware), so pi_new is measured under the current reservoir's q.
		const float nbPickPdf = LightStrategy_SampleLightPdf(
				lightDist,
				dlscEntries,
				dlscDistributions, dlscBVHNodes,
				dlscRadius2, dlscNormalCosAngle,
				lightBVH, lightBVHLightToLeaf, lightBVHMinDist2,
				VLOAD3F(&bsdf->hitPoint.p.x), landingShadeN,
				bsdf->isVolume,
				eLightIndex);
		if (nbPickPdf <= 0.f)
			continue;

		// Replay the neighbour's stored light-surface sample
		// (reconnection shift): the queued ray covers the exact light
		// point the stored target measured, so the traced V folds into
		// pi_new consistently.
		float nbPdfW;
		const float3 nbRadiance = Light_Illuminate(
				&lights[eLightIndex],
				bsdf,
				time, entry->lsU, entry->lsV, entry->lsP,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				tmpHitPoint,
				&candRays[slot], &nbPdfW, NULL, NULL
				LIGHTS_PARAM);
		if (Spectrum_IsBlack(nbRadiance) || (nbPdfW <= 0.f))
			continue;

		candData[slot].lightIndex = eLightIndex;
		candData[slot].pickPdf = nbPickPdf;
		candData[slot].directPdfW = nbPdfW;
		candData[slot].target = Spectrum_Y(nbRadiance) /
				(nbPickPdf * nbPdfW);
		candData[slot].radianceR = nbRadiance.x;
		candData[slot].radianceG = nbRadiance.y;
		candData[slot].radianceB = nbRadiance.z;
		candData[slot].lsU = entry->lsU;
		candData[slot].lsV = entry->lsV;
		candData[slot].lsP = entry->lsP;
		candData[slot].nbrWSum = eWSum;
		candData[slot].nbrTarget = eTarget;
		candData[slot].nbrM = eM;
		// A valid merge candidate alone justifies the resolve pass: it
		// can supply the winner when every fresh candidate was culled
		anyValid = true;
	}

	return anyValid;
}

//----------------------------------------------------------------------
// ReSTIR DI visibility-weighted target (E2a), phase 2.
//
// Called from the MK_RT_RESTIR state after the candidate shadow rays
// written by DirectLight_RestirEnqueueVisibility() have been traced.
// The binary visibility term V_i = (candHits[i] missed everything) is
// folded into each target; the RIS merge, temporal merge and spatial
// store then run exactly like the unshadowed path. The winning
// candidate's *real* shadow ray is still emitted into rays[gid] and
// traced through the normal MK_RT_DL path, so transparent shadows and
// shadow-catcher handling stay unchanged - V only steers the
// reservoir's target weights.
//----------------------------------------------------------------------
OPENCL_FORCE_NOT_INLINE bool DirectLight_RestirResolveVisibility(
		__global const BSDF *bsdf,
		__global Ray *shadowRay,
		__global const Ray *candRays,
		__global const RayHit *candHits,
		__global const RestirVisCandidate *candData,
		const uint visCandCount,
		const float worldCenterX,
		const float worldCenterY,
		const float worldCenterZ,
		const float worldRadius,
		__global HitPoint *tmpHitPoint,
		// u1/u2/lightPassThroughEvent stay consumed at the call site to
		// keep the sampler stream aligned with the unshadowed path; the
		// winner's real sample comes from its candidate record (fresh)
		// or the stored reservoir (merged) instead.
		const float time, const float u0, const float u1, const float u2,
		const float lightPassThroughEvent,
		const int restirTemporalEnable, const int restirStoreEnable,
		const uint restirPixelIndex, __global RestirReservoir *restirReservoirs,
		__global DirectLightIlluminateInfo *info
		LIGHTS_PARAM_DECL) {
	__global const float* restrict lightDist = BSDF_IsShadowCatcherOnlyInfiniteLights(bsdf MATERIALS_PARAM) ?
			infiniteLightSourcesDistribution : lightsDistribution;

	uint resSlot = NULL_INDEX;
	uint resLightIndex = NULL_INDEX;
	float resPickPdf = 0.f;
	float resTarget = 0.f;
	float wSum = 0.f;
	uint mTotal = 0u;
	uint mergeCount = 0u;
	// Tracks whether the winner is a fresh candidate whose exact
	// light-surface sample (and shadow ray) is still in candData/
	// candRays; a merged winner has no record and is re-evaluated.
	bool winnerIsFresh = false;
	// Winning sample's light-surface draws, propagated through the
	// merges for the reservoir store (reconnection shift, E2c)
	float resLSU = 0.f, resLSV = 0.f, resLSP = 0.f;

	for (uint i = 0; i < visCandCount; ++i) {
		// Every proposal draw counts toward M, culled ones included
		++mTotal;
		if (candData[i].lightIndex == NULL_INDEX)
			continue;

		// Binary visibility: occluded candidates contribute a zero
		// target but still count in M. The hit covers the exact ray
		// the contribution would use (the stored record pairs each
		// candidate's light-surface sample with its shadow ray), so
		// V and the payoff are consistent by construction.
		if (candHits[i].meshIndex != NULL_INDEX)
			continue;

		const float target = candData[i].target;
		if (target <= 0.f)
			continue;

		wSum += target;

		const uint acceptSeed = SobolSequence_BlueNoiseHash(
				as_uint(u0) ^ (i * 0x9E3779B9u + 0x85EBCA6Bu));
		const float r = SobolSequence_BlueNoiseHash(
				acceptSeed ^ (i * 0x85EBCA6Bu)) * (1.f / 4294967296.f);
		const float accept = target / wSum;
		if ((resLightIndex == NULL_INDEX) || (r < accept)) {
			resSlot = i;
			resLightIndex = candData[i].lightIndex;
			resPickPdf = candData[i].pickPdf;
			resTarget = target;
			winnerIsFresh = true;
		}
	}

	// The fresh winner's light-surface sample rides in its candidate
	// record (written at enqueue, E2d) so the reservoir can store and
	// later replay it (reconnection shift, E2c).
	if (winnerIsFresh) {
		resLSU = candData[resSlot].lsU;
		resLSV = candData[resSlot].lsV;
		resLSP = candData[resSlot].lsP;
	}

	// Temporal merge + cap + per-pixel store: the stored slot holds the
	// PRE-spatial-merge reservoir so a spatially-merged (inflated) wSum
	// never feeds back into the neighbours' merges on the next pass -
	// the e18 CPU merge-explosion pathology. The temporal merge can
	// also supply a winner when every fresh candidate was occluded, so
	// the empty-reservoir return only happens after all merges.
	bool sampleIsFresh = winnerIsFresh;
	const float3 mergeGeomN = BSDF_GetLandingGeometryN(bsdf);
	Restir_MergeStore(&resLightIndex, &resPickPdf, &resTarget,
			&wSum, &mTotal, &resLSU, &resLSV, &resLSP,
			lightDist,
			bsdf->hitPoint.p.x, bsdf->hitPoint.p.y, bsdf->hitPoint.p.z,
			mergeGeomN.x, mergeGeomN.y, mergeGeomN.z,
			visCandCount, u0,
			restirTemporalEnable, restirStoreEnable, restirPixelIndex,
			restirReservoirs,
			&sampleIsFresh
			LIGHTS_PARAM);

	//------------------------------------------------------------------
	// Spatial reuse with real visibility (E2d): the merge candidates
	// queued at enqueue now have traced hits, so pi_new carries the
	// actual V - the neighbour's stored target (pi_old, itself
	// V-weighted at the storing point) divides out, and an occluded-at-
	// this-point transfer contributes zero instead of the V-free
	// approximation of the unshadowed merge. An occluded or zero-target
	// merge is skipped entirely (matching the unshadowed gate: the
	// neighbour's draws only count in mTotal when the merge runs).
	//------------------------------------------------------------------
	for (uint j = 0; j < RESTIR_PIXEL_MERGES_MAX; ++j) {
		const uint slot = visCandCount + j;
		if (candData[slot].lightIndex == NULL_INDEX)
			continue;
		// Occluded at THIS point: the V-folded target is zero
		if (candHits[slot].meshIndex != NULL_INDEX)
			continue;

		const float nbTarget = candData[slot].target;
		if (nbTarget <= 0.f)
			continue;

		// GRIS combine weight, unclamped - CPU parity: capping or
		// dropping the merge by its ratio is a data-dependent weight
		// distortion and biases the estimator dark (restirdi.cpp).
		const float bNbr = candData[slot].nbrWSum *
				(nbTarget / candData[slot].nbrTarget);
		wSum += bNbr;
		mTotal += candData[slot].nbrM;
		++mergeCount;

		const uint acceptSeed = SobolSequence_BlueNoiseHash(
				as_uint(u0) ^ (0x45d9f3bu + j));
		const float r = SobolSequence_BlueNoiseHash(
				acceptSeed ^ (j * 0x85EBCA6Bu)) * (1.f / 4294967296.f);
		if ((resLightIndex == NULL_INDEX) || (r < bNbr / wSum)) {
			resSlot = slot;
			resLightIndex = candData[slot].lightIndex;
			resPickPdf = candData[slot].pickPdf;
			resTarget = nbTarget;
			resLSU = candData[slot].lsU;
			resLSV = candData[slot].lsV;
			resLSP = candData[slot].lsP;
			sampleIsFresh = false;
		}
	}

	if (resLightIndex == NULL_INDEX)
		return false;

	const float risScale = wSum / (mTotal * resTarget);

	__global const LightSource* restrict light = &lights[resLightIndex];
	info->lightIndex = resLightIndex;
	info->lightID = light->lightID;
	info->pickPdf = resPickPdf;
	info->risScale = risScale;

	float3 lightRadiance;
	if (sampleIsFresh) {
		// Winner survived the merge: reuse the candidate's stored
		// evaluation and its exact shadow ray - the visibility test
		// already covered precisely this ray, so the binary V and the
		// contribution are paired bit-exactly. The ray is re-emitted
		// through MK_RT_DL so transparent-shadow and shadow-catcher
		// handling stay identical to the unshadowed path.
		lightRadiance = MAKE_FLOAT3(candData[resSlot].radianceR,
				candData[resSlot].radianceG, candData[resSlot].radianceB);
		info->directPdfW = candData[resSlot].directPdfW;
		*shadowRay = candRays[resSlot];
	} else {
		// A merged winner carries its stored light-surface sample
		// (resLSU/lsV/lsP): replay it so the contribution is evaluated
		// at the same light point the stored target measured - the
		// reconnection shift. The emitted ray is traced through
		// MK_RT_DL, keeping V/contribution consistent.
		float directPdfW;
		// Scalar output params are thread pointers under Metal - they
		// can not alias device memory like &info->emissionPdfW
		float emissionPdfW, cosThetaAtLight;
		lightRadiance = Light_Illuminate(
				light,
				bsdf,
				time, resLSU, resLSV, resLSP,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				tmpHitPoint,
				shadowRay, &directPdfW,
				&emissionPdfW, &cosThetaAtLight
				LIGHTS_PARAM);
		info->directPdfW = directPdfW;
		info->emissionPdfW = emissionPdfW;
		info->cosThetaAtLight = cosThetaAtLight;
	}

	if (Spectrum_IsBlack(lightRadiance))
		return false;

	VSTORE3F(lightRadiance, info->lightRadiance.c);
	VSTORE3F(lightRadiance, info->lightIrradiance.c);
	return true;
}

OPENCL_FORCE_INLINE bool DirectLight_Illuminate(
		__global const BSDF *bsdf,
		__global Ray *shadowRay,
		const float worldCenterX,
		const float worldCenterY,
		const float worldCenterZ,
		const float worldRadius,
		__global HitPoint *tmpHitPoint,
		const float time, const float u0, const float u1, const float u2,
		const float lightPassThroughEvent,
		const int restirEnabled, const uint restirCandidateCount,
		const int restirTemporalEnable, const int restirStoreEnable,
		const uint restirPixelIndex, __global RestirReservoir *restirReservoirs,
		const int restirSpatialEnable,
		const uint filmWidth, const uint reservoirCount,
		__global DirectLightIlluminateInfo *info
		LIGHTS_PARAM_DECL) {
	// Select the light strategy to use. A shadow catcher restricted to
	// infinite lights samples the infinite distribution - which the DLSC
	// cache does NOT cover (the CPU infinite strategy instance skips the
	// cache lookup entirely), so the cache params are gated off too.
	const bool onlyInfLights = BSDF_IsShadowCatcherOnlyInfiniteLights(bsdf MATERIALS_PARAM);
	__global const float* restrict lightDist = onlyInfLights ?
		infiniteLightSourcesDistribution : lightsDistribution;
	__global const DLSCacheEntry* restrict dlscEntries =
		onlyInfLights ? NULL : dlscAllEntries;
	__global const LightBVHNode* restrict lightBVH =
		onlyInfLights ? NULL : lightBVHNodes;

	// Pick a light source to sample
	float lightPickPdf;
	uint lightIndex;
	float risScale = 1.f;

	if (restirEnabled) {
		//----------------------------------------------------------------------
		// ReSTIR DI: RIS reservoir over the proposal distribution (q). This is
		// the kernel port of LightStrategyRestirDI::SampleLightsBSDF() (CPU):
		//
		// 1. Draw M candidate lights from q (deterministic stratification of
		//    u0: u_i = u0 + i/M).
		// 2. Weight each candidate with the estimated direct contribution at
		//    this shade point: target = Y(radiance) / (q * directPdfW). The
		//    geometry term (1/directPdfW) is essential: without it the target
		//    ignores distance attenuation and the W-output compensation
		//    explodes on close lights.
		// 3. Streaming reservoir: keep candidate i with probability
		//    target_i / wSum. The accept random is decorrelated with a
		//    murmur3-style hash of (u0, i) - the same scheme as the CPU
		//    (AcceptSeed/AcceptRand in restirdi.cpp) so the proposal draws and
		//    the accept tests stay independent.
		// 4. Output: the reservoir light, its proposal pdf q (NOT the RIS
		//    pdf!) and risScale = wSum / (M * target). M is the TOTAL number
		//    of proposal draws, including candidates culled to zero target -
		//    they contribute 0 to wSum but still count in M.
		//
		// The RIS factor travels in info->risScale instead of being folded in
		// the pick pdf: the MIS against direct hits pairs this estimator with
		// a density based on SampleLightPdf() = q, and both sides must use the
		// same pdf for the MIS weights to sum to 1 (folding the RIS factor in
		// the pdf introduces a ~10% bias on mesh light scenes).
		//----------------------------------------------------------------------
		const uint M = restirCandidateCount;
		uint resLightIndex = NULL_INDEX;
		float resPickPdf = 0.f;
		float wSum = 0.f;
		float resTarget = 0.f;
		// Total proposal draws (fresh draws including culled ones plus
		// the sample counts of merged neighbor reservoirs). This is the
		// M of W = wSum/M, NOT just the loop trip count - mirroring
		// mTotal in the CPU implementation.
		uint mTotal = 0;
		uint mergeCount = 0;
		// Winning sample's light-surface draws, propagated through the
		// merges for the reservoir store (reconnection shift, E2c). The
		// fresh stream uses the degenerate (u_i, 0, 0) sample, so only
		// the first coordinate varies.
		float resLSU = 0.f, resLSV = 0.f, resLSP = 0.f;

		// The fresh stream draws all M candidates; the (bounded)
		// neighbour merges run after the temporal merge + store below.
		const uint candCount = M;
		for (uint i = 0; i < candCount; ++i) {
			// Every proposal draw counts toward mTotal, including the
			// culled ones below (they add 0 to wSum but still count in M).
			++mTotal;
			const float u_i = fmod(u0 + i * (1.f / candCount), 1.f);

			float candPickPdf;
			const uint candIndex = LightStrategy_SampleLights(lightDist,
					dlscEntries,
					dlscDistributions, dlscBVHNodes,
					dlscRadius2, dlscNormalCosAngle,
					lightBVH, lightBVHLightToLeaf, lightBVHMinDist2,
					// DLSC cache entries are keyed on the landing SHADE
					// normal (CPU GetLandingShadeN parity)
					VLOAD3F(&bsdf->hitPoint.p.x), BSDF_GetLandingShadeN(bsdf),
					bsdf->isVolume,
					u_i, &candPickPdf);
			if ((candIndex == NULL_INDEX) || (candPickPdf <= 0.f))
				continue;

			// Light linking: an incompatible candidate contributes 0 to
			// wSum but still counts in mTotal (unbiasedness requirement)
			const ulong candLinkMask = lights[candIndex].linkMask;
			if ((candLinkMask != 0ull) &&
					((candLinkMask & bsdf->hitPoint.linkAcceptMask) == 0ull))
				continue;

			// NOTE: the candidate Illuminate() writes into the global
			// shadowRay scratch buffer; the final Illuminate() below
			// overwrites it with the winning candidate's shadow ray.
			float candPdfW;
			const float3 candRadiance = Light_Illuminate(
					&lights[candIndex],
					bsdf,
					time, u_i, 0.f, 0.f,
					worldCenterX, worldCenterY, worldCenterZ, worldRadius,
					tmpHitPoint,
					shadowRay, &candPdfW, NULL, NULL
					LIGHTS_PARAM);

			if (Spectrum_IsBlack(candRadiance) || (candPdfW <= 0.f))
				continue;

			// Luminance target, mirroring the CPU Spectrum::Y()
			const float target = Spectrum_Y(candRadiance) /
					(candPickPdf * candPdfW);
			if (target <= 0.f)
				continue;

			wSum += target;

			// Weighted reservoir update (single pass algorithm from the
			// ReSTIR paper): accept candidate i with prob w_i / wSum. The
			// accept random is hashed so it is independent of the
			// stratified proposal draws.
			const uint acceptSeed = SobolSequence_BlueNoiseHash(
					as_uint(u0) ^ (i * 0x9E3779B9u + 0x85EBCA6Bu));
			const float r = SobolSequence_BlueNoiseHash(
					acceptSeed ^ (i * 0x85EBCA6Bu)) * (1.f / 4294967296.f);
			const float accept = target / wSum;
			if ((resLightIndex == NULL_INDEX) || (r < accept)) {
				resLightIndex = candIndex;
				resPickPdf = candPickPdf;
				resTarget = target;
				resLSU = u_i;
				resLSV = 0.f;
				resLSP = 0.f;
			}
		}

		// Temporal merge + cap + per-pixel store: shared with the
		// visibility-weighted path (see Restir_MergeStore()). The stored
		// slot holds the PRE-spatial-merge reservoir - storing a merged
		// reservoir would feed its inflated wSum back into the
		// neighbours' merges next pass and compound (the e18 CPU
		// merge-explosion pathology). The temporal merge can also
		// supply a winner when every fresh candidate was culled, so the
		// empty-reservoir return only happens after all merges.
		bool sampleIsFresh = true;
		const float3 mergeGeomN = BSDF_GetLandingGeometryN(bsdf);
		Restir_MergeStore(&resLightIndex, &resPickPdf, &resTarget,
				&wSum, &mTotal, &resLSU, &resLSV, &resLSP,
				lightDist,
				bsdf->hitPoint.p.x, bsdf->hitPoint.p.y, bsdf->hitPoint.p.z,
				mergeGeomN.x, mergeGeomN.y, mergeGeomN.z,
				M, u0,
				restirTemporalEnable, restirStoreEnable, restirPixelIndex,
				restirReservoirs,
				&sampleIsFresh
				LIGHTS_PARAM);

		//------------------------------------------------------------------
		// Spatial reuse (E2b): screen-space neighbour-pixel merge on the
		// accumulators, AFTER the store. The shared helper re-evaluates
		// each stored neighbour winner at THIS point (pi_new) and merges
		// it with weight bNbr = wSum_nbr * (pi_new / pi_old); the
		// same-surface geometric gate keeps the ratio near 1 so reuse
		// actually pays (the old world-space hash grid merged unrelated
		// surfaces and measured ~1.1-1.3x WORSE RMSE on manylights -
		// e14). The neighbour's stored light-surface sample is replayed
		// for the re-evaluation (reconnection shift, E2c) and propagates
		// to the winner; the contribution below still draws a fresh
		// surface sample (u1,u2), unchanged.
		//------------------------------------------------------------------
		if (restirSpatialEnable) {
			Restir_SpatialMergePixels(&resLightIndex, &resPickPdf, &resTarget,
					&wSum, &mTotal, &resLSU, &resLSV, &resLSP,
					lightDist, bsdf, shadowRay, time, u0,
					worldCenterX, worldCenterY, worldCenterZ, worldRadius,
					tmpHitPoint, restirPixelIndex, filmWidth,
					restirReservoirs, reservoirCount,
					&mergeCount, &sampleIsFresh
					LIGHTS_PARAM);
		}

		// Empty reservoir (no contributing candidate in any stream)
		if (resLightIndex == NULL_INDEX)
			return false;
		risScale = wSum / (mTotal * resTarget);

		lightIndex = resLightIndex;
		lightPickPdf = resPickPdf;
	} else {
		lightIndex = LightStrategy_SampleLights(lightDist,
				dlscEntries,
				dlscDistributions, dlscBVHNodes,
				dlscRadius2, dlscNormalCosAngle,
				lightBVH, lightBVHLightToLeaf, lightBVHMinDist2,
				VLOAD3F(&bsdf->hitPoint.p.x), BSDF_GetLandingShadeN(bsdf),
				bsdf->isVolume,
				u0, &lightPickPdf);
		if ((lightIndex == NULL_INDEX) || (lightPickPdf <= 0.f))
			return false;
	}

	__global const LightSource* restrict light = &lights[lightIndex];

	// Light linking: an incompatible pick contributes 0 but keeps its
	// proposal pdf, so the estimator stays unbiased. This also rejects
	// winners merged from reservoirs of other receivers (spatial ReSTIR).
	if ((light->linkMask != 0ull) &&
			((light->linkMask & bsdf->hitPoint.linkAcceptMask) == 0ull))
		return false;

	info->lightIndex = lightIndex;
	info->lightID = light->lightID;
	info->pickPdf = lightPickPdf;

	// Illuminate the point
	float directPdfW;
	float emissionPdfW, cosThetaAtLight;
	const float3 lightRadiance = Light_Illuminate(
			&lights[lightIndex],
			bsdf,
			time, u1, u2,
			lightPassThroughEvent,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius,
			tmpHitPoint,
			shadowRay, &directPdfW,
			&emissionPdfW, &cosThetaAtLight
			LIGHTS_PARAM);

	if (Spectrum_IsBlack(lightRadiance))
		return false;
	else {
		info->directPdfW = directPdfW;
		info->emissionPdfW = emissionPdfW;
		info->cosThetaAtLight = cosThetaAtLight;
		info->risScale = risScale;
		VSTORE3F(lightRadiance, info->lightRadiance.c);
		VSTORE3F(lightRadiance, info->lightIrradiance.c);
		return true;
	}
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// ReSTIR GI (G1 GPU): first-bounce reservoir resampling.
//
// Kernel port of RestirGI::ResampleFirstBounce() (src/slg/engines/
// restirgi.cpp, see dev-tools/restir-gi-design.md). The estimator is
// identical to the CPU side:
//
//   pi_hat(dir) = Y(f_r * cos)(dir) * (Y(lHat) + eps)
//
// where lHat is a one-sample proxy of x2's incident radiance (x2
// emission + one NEE shadow ray) and eps is 5% of the brightest proxy
// in the candidate set (the support floor - without it zero-proxy
// directions can never win and their real contribution is dropped).
//
// GPU decomposition over two tail-queue rounds (the MK_RT_RESTIR
// pattern extended to two stages):
//
//   MK_GENERATE_NEXT_VERTEX_RAY  -- K BSDF proposals; the bounce rays
//       ride the GI tail of rays[] (slots [0,K) of the task's 2K
//       block at giCandRayBase).
//   MK_RT_GI_BOUNCE              -- consumes the bounce hits; builds
//       each hit candidate's x2 BSDF in tmpBsdf (sequential reuse),
//       records x2 emission / env radiance and queues the NEE shadow
//       rays (slots [K,2K)).
//   MK_RT_GI_RESOLVE             -- folds the NEE binary visibility
//       into lHat, runs the RIS + temporal/spatial merges, stores the
//       pre-spatial reservoir and hands the winner to
//       MK_GENERATE_NEXT_VERTEX_RAY through the RestirGIResult record
//       (pending = 1) so the shared AddVertex/RR/throughput tail stays
//       untouched.
//
// Merge visibility: the temporal merge traces the reconnected x1->x2
// segment through tail slot 2K (queued by MK_RT_GI_BOUNCE, read by the
// resolve) - matching the CPU side exactly. This test is load-bearing,
// not cosmetic: without it, occluded or grazing reconnection transfers
// merge their stale-bright proxy radiance, the GRIS pi_new/pi_old ratio
// saturates at the 64x clamp every pass and wSum compounds
// geometrically into ~1e7 hot pixels (measured on cornell). Spatial
// merges stay V-free on purpose - the same-surface gate on x1 rejects
// nearly all cross-surface transfers, and the ratio clamp bounds the
// residual tail.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float RestirGI_Hash(const uint seed, const uint stream) {
	return SobolSequence_BlueNoiseHash(seed ^ (stream * 0x85EBCA6Bu)) *
			(1.f / 4294967296.f);
}

// Phase 1 (in MK_GENERATE_NEXT_VERTEX_RAY): draw K BSDF proposals and
// queue their bounce rays. The candidate records carry everything the
// resolve needs that is not recoverable from the trace results.
OPENCL_FORCE_NOT_INLINE bool RestirGI_EnqueueBounce(
		__global BSDF *bsdf,
		__global Ray *candRays,
		__global RestirGICandidate *candData,
		const uint K, const uint baseSeed, const float time
		MATERIALS_PARAM_DECL) {
	bool anyValid = false;
	for (uint i = 0; i < K; ++i) {
		__global RestirGICandidate *rec = &candData[i];
		const uint seed = baseSeed ^ (i * 0x9E3779B9u);

		float3 dir;
		float pdfW, cosDir;
		BSDFEvent event;
		const float3 bsdfSample = BSDF_Sample(bsdf,
				RestirGI_Hash(seed, 0x01u), RestirGI_Hash(seed, 0x02u),
				&dir, &pdfW, &cosDir, &event
				MATERIALS_PARAM);
		if (Spectrum_IsBlack(bsdfSample) || !(pdfW > 0.f)) {
			// Culled proposal: counts toward M (it is a proposal draw)
			// but can never win.
			rec->pdfW = 0.f;
			rec->miss = 0u;
			Ray_Init4(&candRays[i], VLOAD3F(&bsdf->hitPoint.p.x),
					(float3)(0.f, 0.f, 1.f), 0.f, 0.f, time);
			candRays[i].flags = RAY_FLAGS_MASKED;
			candRays[K + i].flags = RAY_FLAGS_MASKED;
			continue;
		}

		VSTORE3F(dir, &rec->dirX);
		VSTORE3F(bsdfSample * pdfW, &rec->fcosR);
		rec->pdfW = pdfW;
		rec->event = (uint)event;
		rec->miss = 0u;
		rec->emisR = 0.f; rec->emisG = 0.f; rec->emisB = 0.f;
		rec->neeR = 0.f; rec->neeG = 0.f; rec->neeB = 0.f;
		Ray_Init2(&candRays[i], BSDF_GetRayOrigin(bsdf, dir), dir, time);
		// The NEE slot stays masked until the bounce hit is resolved
		candRays[K + i].flags = RAY_FLAGS_MASKED;
		anyValid = true;
	}
	return anyValid;
}

// Phase 2 (MK_RT_GI_BOUNCE): resolve the bounce hits into x2 records
// and queue each hit candidate's one-sample NEE shadow ray.
OPENCL_FORCE_NOT_INLINE void RestirGI_Bounce(
		__global const BSDF *x1bsdf,
		__global BSDF *tmpBsdf,
		__global PathVolumeInfo *scratchVolInfo,
		__global const EyePathInfo *pathInfo,
		__global HitPoint *tmpHitPoint,
		__global PathDepthInfo *tmpPathDepthInfo,
		__global Ray *candRays,
		__global const RayHit *candHits,
		__global RestirGICandidate *candData,
		__global RestirGIResult *result,
		__global const RestirGIReservoir *giReservoirs,
		const uint pixelIndex, const uint pass, const int temporalEnable,
		const uint K, const uint baseSeed, const float time,
		const float worldCenterX, const float worldCenterY,
		const float worldCenterZ, const float worldRadius
		LIGHTS_PARAM_DECL) {
	for (uint i = 0; i < K; ++i) {
		__global RestirGICandidate *rec = &candData[i];
		if (!(rec->pdfW > 0.f))
			continue;

		__global Ray *bRay = &candRays[i];
		const float3 dir = VLOAD3F(&rec->dirX);

		if (candHits[i].meshIndex == NULL_INDEX) {
			// Environment miss: the proxy is the env radiance along the
			// direction (part of the indirect integral).
			rec->miss = 1u;
			float3 env = BLACK;
			float directPdfA;
			for (uint e = 0; e < envLightCount; ++e)
				env += EnvLight_GetRadiance(&lights[envLightIndices[e]],
						worldRadius, x1bsdf, -dir, &directPdfA, NULL
						LIGHTS_PARAM);
			VSTORE3F(env, &rec->emisR);
			// The bounce slot is consumed; mask it so the next trace
			// pass does not re-trace a stale ray.
			candRays[i].flags = RAY_FLAGS_MASKED;
			continue;
		}

		// Surface hit: build x2's BSDF in the task's scratch slot (the
		// candidate BSDFs are evaluated sequentially, one per loop
		// trip). The volume info copy keeps BSDF_Init's interior/
		// exterior updates off the path's live PathVolumeInfo.
		*scratchVolInfo = pathInfo->volume;
		BSDF_Init(tmpBsdf, false, bRay, &candHits[i],
				RestirGI_Hash(baseSeed ^ (i * 0x9E3779B9u), 0x03u),
				scratchVolInfo
				MATERIALS_PARAM);
		// The candidate vertex sits one bounce (of the sampled event)
		// past the current path vertex
		*tmpPathDepthInfo = pathInfo->depth;
		PathDepthInfo_IncDepths(tmpPathDepthInfo, rec->event);
		HitPoint_SetRayContext(&tmpBsdf->hitPoint, EYE_RAY | INDIRECT_RAY,
				rec->event, tmpPathDepthInfo, candHits[i].t);
		VSTORE3F(VLOAD3F(&tmpBsdf->hitPoint.geometryN.x), &rec->x2nX);

		float directPdfA;
		float3 lHat = BSDF_IsLightSource(tmpBsdf) ?
				BSDF_GetEmittedRadiance(tmpBsdf, &directPdfA, NULL
						LIGHTS_PARAM) : BLACK;

		// One-sample NEE probe (light pick + shadow ray): the binary
		// visibility is folded in at resolve once the tail trace has
		// run.
		const uint seed = baseSeed ^ (i * 0x9E3779B9u);
		float pickPdf;
		const uint lightIndex = LightStrategy_SampleLights(
				lightsDistribution,
				dlscAllEntries, dlscDistributions, dlscBVHNodes,
				dlscRadius2, dlscNormalCosAngle,
				lightBVHNodes, lightBVHLightToLeaf, lightBVHMinDist2,
				VLOAD3F(&tmpBsdf->hitPoint.p.x),
				// DLSC entries are keyed on the landing shade normal
				BSDF_GetLandingShadeN(tmpBsdf),
				tmpBsdf->isVolume,
				RestirGI_Hash(seed, 0x72u), &pickPdf);
		bool neeQueued = false;
		if ((lightIndex != NULL_INDEX) && (pickPdf > 0.f)) {
			float directPdfW;
			const float3 lightRadiance = Light_Illuminate(
					&lights[lightIndex],
					tmpBsdf, time,
					RestirGI_Hash(seed, 0x73u), RestirGI_Hash(seed, 0x74u),
					RestirGI_Hash(seed, 0x75u),
					worldCenterX, worldCenterY, worldCenterZ, worldRadius,
					tmpHitPoint,
					&candRays[K + i], &directPdfW, NULL, NULL
					LIGHTS_PARAM);
			if (!Spectrum_IsBlack(lightRadiance) && (directPdfW > 0.f)) {
				BSDFEvent event2;
				float pdfW2;
				const float3 eval2 = BSDF_Evaluate(tmpBsdf,
						VLOAD3F(&candRays[K + i].d.x), &event2, &pdfW2
						MATERIALS_PARAM);
				if (!Spectrum_IsBlack(eval2)) {
					const float3 nee = lightRadiance * eval2 /
							(directPdfW * pickPdf);
					VSTORE3F(nee, &rec->neeR);
					neeQueued = true;
				}
			}
		}
		if (!neeQueued)
			candRays[K + i].flags = RAY_FLAGS_MASKED;

		VSTORE3F(lHat, &rec->emisR);
		// Bounce slot consumed: mask it (the NEE half rides the next
		// trace pass).
		candRays[i].flags = RAY_FLAGS_MASKED;
	}

	// Temporal-merge visibility ray (CPU parity: restirgi.cpp traces the
	// reconnected x1->x2 segment and rejects occluded transfers). Without
	// it, occluded/stale transfers merge their old proxy radiance anyway:
	// the GRIS ratio then saturates at the clamp every pass and wSum
	// compounds geometrically (measured ~1e7 hot pixels on cornell).
	// Slot 2K of the tail; a masked slot means "no usable stored segment",
	// which the resolve treats identically to the CPU's !hasDir.
	__global Ray *vRay = &candRays[2u * K];
	const __global RestirGIReservoir *storedRes = &giReservoirs[pixelIndex];
	float3 vDir = (float3)(0.f, 0.f, 1.f);
	float vMax = 0.f;
	bool queueV = false;
	if (temporalEnable && (storedRes->m > 0u) && (storedRes->pass < pass) &&
			(storedRes->wSum > 0.f) && (storedRes->target > 0.f) &&
			!storedRes->isMiss) {
		const float3 x1 = VLOAD3F(&x1bsdf->hitPoint.p.x);
		const float3 sx2 = MAKE_FLOAT3(storedRes->x2X, storedRes->x2Y,
				storedRes->x2Z);
		const float3 dv = sx2 - x1;
		const float dCur = length(dv);
		if (dCur > MachineEpsilon_E_Float3(x1)) {
			vDir = dv / dCur;
			vMax = dCur;
			queueV = true;
		}
	}
	if (queueV)
		Ray_Init4(vRay, BSDF_GetRayOrigin(x1bsdf, vDir), vDir, 0.f, vMax, time);
	else
		vRay->flags = RAY_FLAGS_MASKED;
	// Seqlock tag: the resolve accepts this ray's hit only while the
	// stored entry still carries this exact stamp. A sibling rewriting
	// the reservoir between bounce and resolve invalidates the pairing.
	result->vSeq = queueV ? storedRes->pass : 0xFFFFFFFFu;
}

// Phase 3 (MK_RT_GI_RESOLVE): fold the NEE visibility into the proxy
// targets, run the RIS + temporal/spatial merges, store the
// pre-spatial reservoir and record the winner for MK_GENERATE.
OPENCL_FORCE_NOT_INLINE void RestirGI_Resolve(
		__global const BSDF *x1bsdf,
		__global const Ray *candRays,
		__global const RayHit *candHits,
		__global RestirGICandidate *candData,
		__global RestirGIResult *result,
		__global RestirGIReservoir *giReservoirs,
		const uint pixelIndex, const uint pass,
		const uint K, const uint baseSeed,
		const int temporalEnable, const int spatialEnable,
		const uint filmWidth,
		const uint reservoirCount, const float worldRadius
		MATERIALS_PARAM_DECL) {
	__global RestirGIReservoir *stored = &giReservoirs[pixelIndex];
	const float3 x1 = VLOAD3F(&x1bsdf->hitPoint.p.x);
	const float3 x1n = VLOAD3F(&x1bsdf->hitPoint.geometryN.x);

	// Proxy radiance per candidate: emission/env + V * NEE term.
	// (K is memory-clamped to <= 4 on the host; the bound is still
	// applied here so a misconfigured taskConfig cannot overrun the
	// stack array.)
	const uint Kv = min(K, 8u);
	float lHatY[8];
	float lHatMax = 0.f;
	for (uint i = 0; i < Kv; ++i) {
		lHatY[i] = 0.f;
		__global RestirGICandidate *rec = &candData[i];
		if (!(rec->pdfW > 0.f))
			continue;
		float lY = Spectrum_Y(MAKE_FLOAT3(rec->emisR, rec->emisG,
				rec->emisB));
		if ((rec->miss == 0u) &&
				(candHits[K + i].meshIndex == NULL_INDEX) &&
				!(candRays[K + i].flags & RAY_FLAGS_MASKED))
			lY += Spectrum_Y(MAKE_FLOAT3(rec->neeR, rec->neeG,
					rec->neeB));
		lHatY[i] = lY;
		lHatMax = fmax(lHatMax, lY);
	}
	const float eps = 0.05f * lHatMax;

	//------------------------------------------------------------------
	// RIS over the fresh candidates (culled proposals count in M).
	//------------------------------------------------------------------
	float wSum = 0.f;
	uint mTotal = 0;
	int winner = -1;
	float wTarget = 0.f;
	for (uint i = 0; i < Kv; ++i) {
		__global RestirGICandidate *rec = &candData[i];
		if (!(rec->pdfW > 0.f))
			continue;
		++mTotal;
		const float target = Spectrum_Y(MAKE_FLOAT3(rec->fcosR,
				rec->fcosG, rec->fcosB)) * (lHatY[i] + eps);
		const float w = target / rec->pdfW;
		wSum += w;
		const float r = RestirGI_Hash(baseSeed ^ (i * 0x9E3779B9u), 0x04u);
		if ((winner < 0) || (r < w / wSum)) {
			winner = (int)i;
			wTarget = target;
		}
	}

	//------------------------------------------------------------------
	// Winner record (fresh candidate, stored reservoir or neighbour).
	//------------------------------------------------------------------
	float3 outDir = (float3)(0.f, 0.f, 1.f);
	float3 outFcos = BLACK;
	float outTarget = 0.f;
	float3 outLHat = BLACK;
	uint outEvent = 0u;
	float outPdfW = 0.f;
	uint outIsMiss = 0u;
	float3 outX2 = (float3)(0.f, 0.f, 0.f);
	float3 outX2n = (float3)(0.f, 1.f, 0.f);
	bool haveOut = false;
	bool outIsFresh = false;
	if (winner >= 0) {
		__global RestirGICandidate *rec = &candData[winner];
		outDir = VLOAD3F(&rec->dirX);
		outFcos = VLOAD3F(&rec->fcosR);
		outTarget = wTarget;
		outLHat = MAKE_FLOAT3(rec->emisR, rec->emisG, rec->emisB);
		if ((rec->miss == 0u) &&
				(candHits[K + winner].meshIndex == NULL_INDEX) &&
				!(candRays[K + winner].flags & RAY_FLAGS_MASKED))
			outLHat += MAKE_FLOAT3(rec->neeR, rec->neeG, rec->neeB);
		outEvent = rec->event;
		outPdfW = rec->pdfW;
		outIsMiss = rec->miss;
		if (outIsMiss == 0u) {
			outX2 = VLOAD3F(&candRays[winner].o.x) +
					candHits[winner].t * VLOAD3F(&candRays[winner].d.x);
			outX2n = VLOAD3F(&rec->x2nX);
		}
		haveOut = true;
		outIsFresh = true;
	}

	//------------------------------------------------------------------
	// Temporal merge: reconnect the pixel's stored x2 to the current
	// x1 (reconnection shift with Jacobian). The reconnected segment's
	// binary visibility arrives through tail slot 2K (queued by
	// MK_RT_GI_BOUNCE, consumed here): occluded transfers get
	// pi_new = 0 exactly like the CPU side. The pass-stamp gate (see
	// the struct comment) keeps the merge to strictly older entries -
	// same-pass re-reads would compound wSum into itself.
	//------------------------------------------------------------------
	// Seqlock read (E2d): the reservoir word is shared with sibling
	// tasks whose stores are unordered, so snapshot the entry, verify
	// the stamp is unchanged afterwards, and - for surface hits - also
	// require the stamp to still equal the one the bounce tagged on the
	// visibility ray (result->vSeq). A store publishes pass last after
	// writing 0xFFFFFFFF first, so any torn or superseded entry fails
	// one of the two comparisons.
	const uint p0 = stored->pass;
	if (temporalEnable && (p0 != 0xFFFFFFFFu) && (p0 < pass)) {
		const RestirGIReservoir snap = *stored;
		if ((snap.m > 0u) && (snap.wSum > 0.f) && (snap.target > 0.f) &&
				// Representative-winner gate (E2b): a stored winner
				// whose target is far below the reservoir's mean weight
				// indicates a torn/stale entry; merging it compounds
				// the wSum error. Mirrors the spatial filter below.
				(snap.target >= 0.05f * snap.wSum / (float)snap.m) &&
				// Seqcheck: the stamp must be unchanged across the
				// snapshot, else sibling stores raced the read.
				(stored->pass == p0)) {
			float piNew = 0.f;
			float J = 1.f;
			bool hasDir = false;
			bool mergeVisible = true;
			float3 stDir = (float3)(0.f, 0.f, 1.f);
			float3 stEval = BLACK;
			uint stEvent = 0u;
			if (snap.isMiss) {
				stDir = MAKE_FLOAT3(snap.dirX, snap.dirY, snap.dirZ);
				hasDir = true;
			} else {
				const float3 sx2 = MAKE_FLOAT3(snap.x2X, snap.x2Y,
						snap.x2Z);
				const float3 dv = sx2 - x1;
				const float dCur = length(dv);
				if (dCur > MachineEpsilon_E_Float3(x1)) {
					stDir = dv / dCur;
					hasDir = true;

					// Jacobian of the solid-angle shift src -> cur
					const float3 toSrc = MAKE_FLOAT3(snap.x1X,
							snap.x1Y, snap.x1Z) - sx2;
					const float dSrc = length(toSrc);
					if (dSrc > 0.f) {
						const float3 n2 = MAKE_FLOAT3(snap.x2nX,
								snap.x2nY, snap.x2nZ);
						const float cosCur = fabs(dot(n2, -stDir));
						const float cosSrc = fabs(dot(n2, toSrc / dSrc));
						const float denom = cosSrc * dCur * dCur;
						if (denom > 0.f)
							J = (cosCur * dSrc * dSrc) / denom;
					}
				}
			}
			// The merge visibility ray (tail slot 2K) was traced in
			// the previous pass: masked means the segment was
			// degenerate when queued, a hit means occluded - both
			// reject the transfer. The vSeq match proves the hit
			// belongs to THIS entry, not to a predecessor a sibling
			// store has since replaced.
			if (hasDir && (snap.isMiss == 0u))
				mergeVisible = (result->vSeq == p0) &&
						!(candRays[2u * K].flags & RAY_FLAGS_MASKED) &&
						(candHits[2u * K].meshIndex == NULL_INDEX);
			if (hasDir && mergeVisible) {
				// A failed shift (degenerate or occluded reconnected
				// segment, or an unvalidated vSeq pairing) produces a
				// sample outside the current target domain - reject it
				// WITHOUT counting its mass. Adding m anyway inflates
				// mTotal with draws that could never win here, which
				// measurably darkens the output (CPU temporal-only on
				// cornell: -3.5% -> -1.9% vs reference after the same
				// change in restirgi.cpp).
				mTotal += snap.m;

				float pdfS;
				BSDFEvent evS;
				stEval = BSDF_Evaluate(x1bsdf, stDir, &evS, &pdfS
						MATERIALS_PARAM);
				stEvent = (uint)evS;
				piNew = Spectrum_Y(stEval) * (Spectrum_Y(MAKE_FLOAT3(
						snap.lHatR, snap.lHatG, snap.lHatB)) + eps);
			}

			const float ratio = fmin((piNew / snap.target) * J,
					RESTIR_MERGE_MAX_TARGET_RATIO);
			const float bNbr = snap.wSum * ratio;
			wSum += bNbr;
			const float r = RestirGI_Hash(baseSeed, 0x41u);
			// A zero-weight merge cannot become the winner even when
			// nothing else was selected (CPU parity: its target is 0
			// and the resolve would reject it anyway).
			if ((!haveOut && (bNbr > 0.f)) || (r < bNbr / wSum)) {
				outDir = stDir;
				outFcos = stEval;
				outTarget = piNew;
				outLHat = MAKE_FLOAT3(snap.lHatR, snap.lHatG,
						snap.lHatB);
				outIsMiss = snap.isMiss;
				if (!outIsMiss) {
					outX2 = MAKE_FLOAT3(snap.x2X, snap.x2Y,
							snap.x2Z);
					outX2n = MAKE_FLOAT3(snap.x2nX, snap.x2nY,
							snap.x2nZ);
				}
				outEvent = stEvent;
				outPdfW = 0.f;
				haveOut = true;
				outIsFresh = false;
			}
		}
	}

	//------------------------------------------------------------------
	// M cap (2x the candidate count) + pre-spatial store: the stored
	// reservoir must carry the state BEFORE the spatial merges below,
	// else a neighbour-inflated wSum feeds back into the same entries
	// next pass (the merge-explosion pathology).
	//------------------------------------------------------------------
	const uint mCap = 2u * K;
	if (mTotal > mCap) {
		wSum *= (float)mCap / (float)mTotal;
		mTotal = mCap;
	}
	if (haveOut) {
		// Seqlock publish: invalidate the stamp first and write the
		// real one last. A reader mid-write either sees the old stamp,
		// the INVALID marker or the new stamp - and the resolve's
		// before/after stamp comparison plus the vSeq pairing reject
		// all three cases for the temporal merge.
		stored->pass = 0xFFFFFFFFu;
		stored->x1X = x1.x; stored->x1Y = x1.y; stored->x1Z = x1.z;
		stored->x1nX = x1n.x; stored->x1nY = x1n.y; stored->x1nZ = x1n.z;
		stored->x2X = outX2.x; stored->x2Y = outX2.y; stored->x2Z = outX2.z;
		stored->x2nX = outX2n.x; stored->x2nY = outX2n.y;
		stored->x2nZ = outX2n.z;
		stored->dirX = outDir.x; stored->dirY = outDir.y;
		stored->dirZ = outDir.z;
		stored->lHatR = outLHat.x; stored->lHatG = outLHat.y;
		stored->lHatB = outLHat.z;
		stored->wSum = wSum;
		stored->target = outTarget;
		stored->m = mTotal;
		stored->isMiss = outIsMiss;
		stored->pass = pass;
	}

	//------------------------------------------------------------------
	// Spatial merge: 2 pseudo-random neighbour pixels in a 5x5 window
	// (E2b constants), same-surface gated on x1 and shifted by the same
	// Jacobian-corrected reconnection (V-free, see the header comment).
	// Neighbour entries are written by other tasks concurrently, so
	// each is read through the same seqlock snapshot as the temporal
	// merge: the pass stamp must bracket the read unchanged.
	//------------------------------------------------------------------
	if (spatialEnable) {
		const float maxDist2 = RESTIR_PIXEL_MERGE_DIST2 * worldRadius *
				worldRadius;
		const uint px = pixelIndex % filmWidth;
		const uint py = pixelIndex / filmWidth;
		for (uint k = 0; k < RESTIR_PIXEL_MERGES_MAX; ++k) {
			const uint h = SobolSequence_BlueNoiseHash(baseSeed ^
					(k * 0x85EBCA6Bu) ^ 0x5A5A5A5Au);
			uint off = h % 25u;
			if (off == 12u)
				off = 24u; // skip the centre cell (self)
			const int nx = (int)px + (int)(off % 5u) - 2;
			const int ny = (int)py + (int)(off / 5u) - 2;
			if ((nx < 0) || (ny < 0))
				continue;
			const uint nIdx = (uint)ny * filmWidth + (uint)nx;
			if (nIdx >= reservoirCount)
				continue;
			__global RestirGIReservoir *nbr = &giReservoirs[nIdx];
			if (nbr == stored)
				continue;
			// Seqlock read like the temporal merge above: neighbour
			// entries are written by other tasks concurrently, so
			// snapshot the record and accept it only if the stamp
			// brackets the read consistently.
			const uint np0 = nbr->pass;
			const RestirGIReservoir nSnap = *nbr;
			if ((np0 == 0xFFFFFFFFu) || !(nSnap.m > 0u) ||
					!(nSnap.wSum > 0.f) || !(nSnap.target > 0.f))
				continue;
			// Representative-winner gate (E2b)
			if (nSnap.target < 0.05f * nSnap.wSum / (float)nSnap.m)
				continue;
			// Same-surface gate on the primary vertex
			const float ddx = nSnap.x1X - x1.x, ddy = nSnap.x1Y - x1.y,
					ddz = nSnap.x1Z - x1.z;
			if (ddx * ddx + ddy * ddy + ddz * ddz > maxDist2)
				continue;
			const float dn = nSnap.x1nX * x1n.x + nSnap.x1nY * x1n.y +
					nSnap.x1nZ * x1n.z;
			if (dn < RESTIR_PIXEL_MERGE_NORM)
				continue;
			if (nbr->pass != np0)
				continue;

			float3 nbDir = (float3)(0.f, 0.f, 1.f);
			float J = 1.f;
			bool hasDir = false;
			if (nSnap.isMiss) {
				nbDir = MAKE_FLOAT3(nSnap.dirX, nSnap.dirY, nSnap.dirZ);
				hasDir = true;
			} else {
				const float3 nx2 = MAKE_FLOAT3(nSnap.x2X, nSnap.x2Y,
						nSnap.x2Z);
				const float3 dv = nx2 - x1;
				const float dCur = length(dv);
				if (dCur > MachineEpsilon_E_Float3(x1)) {
					nbDir = dv / dCur;
					hasDir = true;
					const float3 toSrc = MAKE_FLOAT3(nSnap.x1X,
							nSnap.x1Y, nSnap.x1Z) - nx2;
					const float dSrc = length(toSrc);
					if (dSrc > 0.f) {
						const float3 n2 = MAKE_FLOAT3(nSnap.x2nX,
								nSnap.x2nY, nSnap.x2nZ);
						const float cosCur = fabs(dot(n2, -nbDir));
						const float cosSrc = fabs(dot(n2,
								toSrc / dSrc));
						const float denom = cosSrc * dCur * dCur;
						if (denom > 0.f)
							J = (cosCur * dSrc * dSrc) / denom;
					}
				}
			}

			float piNew = 0.f;
			float3 nbEval = BLACK;
			uint nbEvent = 0u;
			if (hasDir) {
				// Failed shifts (degenerate reconnected segment) are
				// rejected without counting their mass - same rule as
				// the temporal merge above.
				mTotal += nSnap.m;

				float pdfS;
				BSDFEvent evS;
				nbEval = BSDF_Evaluate(x1bsdf, nbDir, &evS, &pdfS
						MATERIALS_PARAM);
				nbEvent = (uint)evS;
				piNew = Spectrum_Y(nbEval) * (Spectrum_Y(MAKE_FLOAT3(
						nSnap.lHatR, nSnap.lHatG, nSnap.lHatB)) + eps);
			}

			const float ratio = fmin((piNew / nSnap.target) * J,
					RESTIR_MERGE_MAX_TARGET_RATIO);
			const float bNbr = nSnap.wSum * ratio;
			wSum += bNbr;
			const float r = RestirGI_Hash(baseSeed, 0x60u + k);
			if ((!haveOut && (bNbr > 0.f)) || (r < bNbr / wSum)) {
				outDir = nbDir;
				outFcos = nbEval;
				outTarget = piNew;
				outLHat = MAKE_FLOAT3(nSnap.lHatR, nSnap.lHatG,
						nSnap.lHatB);
				outIsMiss = nSnap.isMiss;
				if (!outIsMiss) {
					outX2 = MAKE_FLOAT3(nSnap.x2X, nSnap.x2Y,
							nSnap.x2Z);
					outX2n = MAKE_FLOAT3(nSnap.x2nX, nSnap.x2nY,
							nSnap.x2nZ);
				}
				outEvent = nbEvent;
				outPdfW = 0.f;
				haveOut = true;
				outIsFresh = false;
			}
		}

		if (mTotal > mCap) {
			wSum *= (float)mCap / (float)mTotal;
			mTotal = mCap;
		}
	}

	//------------------------------------------------------------------
	// Winner resolution -> result record for MK_GENERATE.
	//------------------------------------------------------------------
	if (!haveOut) {
		result->pending = 2u; // normal BSDF sample
		return;
	}
	float W = 0.f;
	if ((outTarget > 0.f) && (wSum > 0.f))
		W = wSum / (mTotal * outTarget);
	if (!isfinite(W) || !(W > 0.f)) {
		if (!outIsFresh) {
			result->pending = 2u;
			return;
		}
		// The fresh winner is a plain proposal draw: returning it with
		// the BSDF payoff (W = 1/pdfW) is unbiased - a conditioned
		// redraw would bias the estimate.
		W = 1.f / outPdfW;
	}

	VSTORE3F(outDir, &result->dirX);
	VSTORE3F(outFcos * W, &result->bsdfR);
	result->pdfW = 1.f / W; // risPdfW: RIS marginal selection density
	result->event = outEvent;
	result->pending = 1u;
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Path guiding (P1-3 M4e): flattened SD-tree + per-leaf vMF mixture.
//
// Ports PathGuidingCache::Sample/Pdf exactly - the GPU queries the same
// fitted field the CPU uses (the coarse-grid snapshot of M2b is gone).
// One-sample MIS stays exact because the drawn direction and its pdf
// come from the identical compound distribution.
//
// nodes: uint4 per node, DFS-emitted so the root is always index 0.
//   inner: {child0, child1, axis, splitBits}; leaf: {~0u, ~0u, leafIndex, 0}.
// leaves: 24 floats per leaf - [0..3] component weights w_k,
//   [4+4k..6+4k] mean direction mu_k.xyz, [7+4k] concentration kappa_k,
//   [20] visitation count, [21] informativeness peak, [22] nComp,
//   [23] round flux total.
// Training records flow device-to-host through the 16 small record
// buffers (M2b-2); the tree upload refreshes on the same ForceSwap
// cadence, so queries agree by construction within a round.
//------------------------------------------------------------------------------

#define GUIDE_VMF_K 4u
#define GUIDE_WARMUP_RECORDS 256.f
#define GUIDE_FLOOR_W_SURFACE .10f
#define GUIDE_FLOOR_W_VOLUME .15f
#define GUIDE_LEAF_FLOATS 24u
// Depth cap: TREE_MAX_DEPTH is 12 on the host; anything deeper means a
// corrupt upload and must not loop forever.
#define GUIDE_MAX_DEPTH 32u

// Float atomic add via CAS on the raw bits - the only float atomic
// needed on device (OpenCL has no atomic float add; cl2msl provides
// atomic_cmpxchg for both backends).
OPENCL_FORCE_INLINE void AtomicAddFloat(__global float *addr,
		const float v) {
	uint cur = as_uint(*addr);
	for (;;) {
		const uint prev = atomic_cmpxchg((__global uint *)addr, cur,
				as_uint(as_float(cur) + v));
		if (prev == cur)
			return;
		cur = prev;
	}
}

OPENCL_FORCE_INLINE uint GuidingHash(uint x) {
	// murmur3 32-bit finalizer (matches the CPU GuidingHash)
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// Descend the flattened SD-tree to the leaf holding p; returns the
// leaf's 24-float record, or NULL on a corrupt/degenerate upload.
OPENCL_FORCE_INLINE __global const float *GuideTree_LeafAt(
		__global const uint4 *nodes, __global const float *leaves,
		float3 p) {
	uint ni = 0u;
	for (uint d = 0u; d < GUIDE_MAX_DEPTH; ++d) {
		const uint4 nd = nodes[ni];
		if (nd.x == 0xffffffffu)
			return leaves + nd.z * GUIDE_LEAF_FLOATS;
		const float split = as_float(nd.w);
		const float pc = (nd.z == 0u) ? p.x : ((nd.z == 1u) ? p.y : p.z);
		ni = (pc < split) ? nd.x : nd.y;
	}
	return NULL;
}

// vMF pdf on the sphere: k e^{k(c-1)} / (2 pi (1-e^{-2k})); kappa ~ 0
// reads as the uniform distribution (mirrors PathGuidingCache::VmfPdf).
OPENCL_FORCE_INLINE float Guide_VmfPdf(float cosMuW, float kappa) {
	if (kappa < 1e-3f)
		return .25f / M_PI_F;
	return kappa * exp(kappa * (cosMuW - 1.f)) /
			(2.f * M_PI_F * (1.f - exp(-2.f * kappa)));
}

// A(kappa) = coth(k) - 1/k, the vMF mean resultant length; ~k/3 near 0.
OPENCL_FORCE_INLINE float Guide_VmfMeanCos(float kappa) {
	if (kappa < 1e-3f)
		return kappa / 3.f;
	const float e = exp(-2.f * kappa);
	return (1.f + e) / (1.f - e) - 1.f / kappa;
}

// Exact vMF sample via closed-form z inversion + uniform phi
// (kappa ~ 0 degenerates to uniform sphere).
OPENCL_FORCE_INLINE float3 Guide_VmfSample(float3 mu, float kappa,
		float u0, float u1) {
	if (kappa < 1e-3f)
		return UniformSampleSphere(u0, u1);
	const float z = 1.f + log(max(u1 + (1.f - u1) * exp(-2.f * kappa),
			1e-30f)) / kappa;
	const float s = sqrt(max(0.f, 1.f - z * z));
	const float phi = 2.f * M_PI_F * u0;
	const float3 helper = (fabs(mu.z) < .999f) ?
			MAKE_FLOAT3(0.f, 0.f, 1.f) : MAKE_FLOAT3(0.f, 1.f, 0.f);
	const float3 u = normalize(cross(helper, mu));
	const float3 v = cross(mu, u);
	return u * (s * cos(phi)) + v * (s * sin(phi)) + mu * z;
}

// Per-component density at a direction (mirrors LobePdf): vMF for
// kappa > 0; kappa ~ 0 reads as the cosine lobe on surfaces and the
// uniform sphere in volumes.
OPENCL_FORCE_INLINE float Guide_LobePdf(float kappa, float cosMuW,
		float cosDirN, const bool isotropic) {
	if (kappa < 1e-3f)
		return isotropic ? (.25f / M_PI_F) :
				((cosDirN > 0.f) ? cosDirN / M_PI_F : 0.f);
	return Guide_VmfPdf(cosMuW, kappa);
}

// Component selection weights (mirrors CompWeights): surfaces scale
// each component by its expected cosine A(kappa)*(mu.n), clamped
// positive; volumes use raw weights. Returns the weight sum.
OPENCL_FORCE_INLINE float GuideTree_CompWeights(__global const float *leaf,
		float3 n, const bool isotropic, float *s) {
	const uint nComp = (uint)leaf[22];
	float sSum = 0.f;
	for (uint k = 0u; k < nComp; ++k) {
		float sk = leaf[k];
		if (!isotropic) {
			const float kappa = leaf[7 + k * 4];
			if (kappa < 1e-3f) {
				// kappa ~ 0 doubles as the cosine lobe (E[cos] = 2/3)
				sk *= 2.f / 3.f;
			} else {
				const float d = leaf[4 + k * 4] * n.x +
						leaf[5 + k * 4] * n.y + leaf[6 + k * 4] * n.z;
				sk *= max(0.f, Guide_VmfMeanCos(kappa) * d);
			}
		}
		s[k] = sk;
		sSum += sk;
	}
	return sSum;
}

OPENCL_FORCE_INLINE bool Guide_CosineSample(float3 n, float u0, float u1,
		float3 *sampledDir, float *pdfW) {
	const float cosTheta = sqrt(max(0.f, 1.f - u0));
	if (!(cosTheta > 0.f))
		return false;
	const float sinTheta = sqrt(max(0.f, 1.f - cosTheta * cosTheta));
	const float phi = 2.f * M_PI_F * u1;
	const float3 helper = (fabs(n.z) < .999f) ? MAKE_FLOAT3(0.f, 0.f, 1.f) : MAKE_FLOAT3(0.f, 1.f, 0.f);
	const float3 u = normalize(cross(helper, n));
	const float3 v = cross(n, u);
	*sampledDir = u * (sinTheta * cos(phi)) + v * (sinTheta * sin(phi)) + n * cosTheta;
	*pdfW = cosTheta * (1.f / M_PI_F);
	return true;
}

// M2c adaptive mixture (mirrors PathGuidingCache::MixWeight):
// guide-side selection probability from the leaf's record count times
// the informativeness gate. Thin or diffuse leaves sample mostly BSDF;
// rich peaked leaves trust the guide.
OPENCL_FORCE_INLINE float Guide_PeakGate(float peak) {
	return clamp((peak - 3.5f) / 5.5f, 0.f, 1.f);
}
OPENCL_FORCE_INLINE float Guide_MixWeight(float count, float peak) {
	return min(count / (count + 1024.f), .75f) * Guide_PeakGate(peak);
}

// Ports PathGuidingCache::Sample. leaf may be NULL (empty/corrupt
// field) - that folds into the same cold-leaf fallback the CPU takes.
OPENCL_FORCE_INLINE bool GuideTree_Sample(__global const float *leaf,
		float3 n, float uBin, float uDir0, float uDir1,
		float3 *sampledDir, float *pdfW, const bool isotropic) {
	const float floorW = isotropic ? GUIDE_FLOOR_W_VOLUME : GUIDE_FLOOR_W_SURFACE;
	const uint nComp = leaf ? (uint)leaf[22] : 0u;
	const float count = leaf ? leaf[20] : 0.f;
	if (nComp == 0u || count < GUIDE_WARMUP_RECORDS) {
		// Cold leaf: pure-floor fallback (same density reported by Pdf).
		if (isotropic) {
			*sampledDir = UniformSampleSphere(uDir0, uDir1);
			*pdfW = .25f / M_PI_F;
			return true;
		}
		return Guide_CosineSample(n, uDir0, uDir1, sampledDir, pdfW);
	}

	float s[GUIDE_VMF_K];
	const float sSum = GuideTree_CompWeights(leaf, n, isotropic, s);
	if (!(sSum > 0.f)) {
		if (isotropic) {
			*sampledDir = UniformSampleSphere(uDir0, uDir1);
			*pdfW = .25f / M_PI_F;
			return true;
		}
		return Guide_CosineSample(n, uDir0, uDir1, sampledDir, pdfW);
	}

	// Usable-field fraction: sSum/wSum is the share of mixture mass that
	// is not grazing/dead; the unusable part folds back into the floor.
	float wSum = 0.f;
	for (uint k = 0u; k < nComp; ++k)
		wSum += leaf[k];
	const float usable = (wSum > 0.f) ? clamp(sSum / wSum, 0.f, 1.f) : 0.f;
	const float effFloorW = floorW + (1.f - floorW) * (1.f - usable);

	if (uBin < effFloorW) {
		if (isotropic) {
			*sampledDir = UniformSampleSphere(uDir0, uDir1);
		} else {
			float fp;
			Guide_CosineSample(n, uDir0, uDir1, sampledDir, &fp);
		}
	} else {
		const float u = (uBin - effFloorW) / (1.f - effFloorW);
		float pick = u * sSum;
		uint k = 0u;
		for (; k + 1u < nComp; ++k) {
			pick -= s[k];
			if (pick <= 0.f)
				break;
		}
		if (!(s[k] > 0.f))
			k = 0u;
		const float kappa = leaf[7 + k * 4];
		if (kappa < 1e-3f && !isotropic) {
			float fp;
			Guide_CosineSample(n, uDir0, uDir1, sampledDir, &fp);
		} else {
			const float3 mu = MAKE_FLOAT3(leaf[4 + k * 4],
					leaf[5 + k * 4], leaf[6 + k * 4]);
			*sampledDir = Guide_VmfSample(mu, kappa, uDir0, uDir1);
		}
	}

	// Exact pdf of the compound distribution we just drew from
	const float d = dot(*sampledDir, n);
	float mix = 0.f;
	for (uint k = 0u; k < nComp; ++k)
		mix += (s[k] / sSum) * Guide_LobePdf(leaf[7 + k * 4],
				sampledDir->x * leaf[4 + k * 4] +
				sampledDir->y * leaf[5 + k * 4] +
				sampledDir->z * leaf[6 + k * 4],
				d, isotropic);
	const float floorPdf = isotropic ? .25f / M_PI_F :
			((d > 0.f) ? d / M_PI_F : 0.f);
	*pdfW = effFloorW * floorPdf + (1.f - effFloorW) * mix;
	return true;
}

// Ports PathGuidingCache::Pdf: the exact same compound distribution
// GuideTree_Sample draws from.
OPENCL_FORCE_INLINE float GuideTree_Pdf(__global const float *leaf,
		float3 n, float3 dir, const bool isotropic) {
	const float floorW = isotropic ? GUIDE_FLOOR_W_VOLUME : GUIDE_FLOOR_W_SURFACE;
	const float d = dot(dir, n);
	const float floorPdf = isotropic ? .25f / M_PI_F :
			((d > 0.f) ? d / M_PI_F : 0.f);
	const uint nComp = leaf ? (uint)leaf[22] : 0u;
	const float count = leaf ? leaf[20] : 0.f;
	if (nComp == 0u || count < GUIDE_WARMUP_RECORDS)
		return floorPdf;

	float s[GUIDE_VMF_K];
	const float sSum = GuideTree_CompWeights(leaf, n, isotropic, s);
	if (!(sSum > 0.f))
		return floorPdf;

	float wSum = 0.f;
	for (uint k = 0u; k < nComp; ++k)
		wSum += leaf[k];
	const float usable = (wSum > 0.f) ? clamp(sSum / wSum, 0.f, 1.f) : 0.f;
	const float effFloorW = floorW + (1.f - floorW) * (1.f - usable);

	float mix = 0.f;
	for (uint k = 0u; k < nComp; ++k)
		mix += (s[k] / sSum) * Guide_LobePdf(leaf[7 + k * 4],
				dir.x * leaf[4 + k * 4] + dir.y * leaf[5 + k * 4] +
				dir.z * leaf[6 + k * 4],
				d, isotropic);
	return effFloorW * floorPdf + (1.f - effFloorW) * mix;
}

// Incident-radiance field estimate Lhat(p,d) = leaf.total x raw fitted
// vMF mixture (no cosine weighting, no sampling floor - a kappa ~ 0 EM
// component reads as the sphere uniform here, so support stays positive
// wherever the true field is nonzero; mirrors
// PathGuidingCache::IncidentEstimate with floorFrac = 0). The RIS
// product-guiding target t = f|cos|*Lhat uses this.
OPENCL_FORCE_INLINE float GuideTree_IncidentEstimate(
		__global const float *leaf, const float3 d) {
	const uint nComp = leaf ? (uint)leaf[22] : 0u;
	if ((nComp == 0u) || (leaf[23] <= 0.f))
		return 0.f;
	float mix = 0.f;
	for (uint k = 0u; k < nComp; ++k)
		mix += leaf[k] * Guide_VmfPdf(
				d.x * leaf[4 + k * 4] + d.y * leaf[5 + k * 4] +
				d.z * leaf[6 + k * 4],
				leaf[7 + k * 4]);
	return leaf[23] * mix;
}

// RIS product-guiding candidate (M4b): draw one direction from the
// (1-wG)*BSDF + wG*guide mixture and return its resampling weight
// w = t/pMix with the product target t = f|cos|*Lhat (0 = dead
// candidate). Mirrors the candWeight lambda in PathTracer::RenderEyePath
// (same per-candidate hash salts). The candidate state is stored through
// the out params only when cStore is set - the independent zHatMis pool
// draws without keeping the candidates.
OPENCL_FORCE_INLINE float Guide_RisCandidate(
		__global const float *leaf,
		__global const BSDF *bsdf,
		const float3 shadeN, const bool isVol,
		const float wG, const uint cs, const bool cStore,
		float3 *cDir, float3 *cEval, BSDFEvent *cEvent,
		float *cUD0, float *cUD1, bool *cBsdf, float *cT, float *cPMix
		MATERIALS_PARAM_DECL) {
	const float uSide = GuidingHash(cs ^ 0xa3b19535u) * (1.f / 4294967296.f);
	const float uD0 = GuidingHash(cs ^ 0x85ebca6bu) * (1.f / 4294967296.f);
	const float uD1 = GuidingHash(cs ^ 0xc2b2ae35u) * (1.f / 4294967296.f);
	float3 d;
	float gPdf, bPdf;
	BSDFEvent ev;
	float3 eval;
	bool sideBsdf;
	if (uSide < wG) {
		// Guide-side candidate: sample the fitted mixture (incl. floor)
		// then evaluate the BSDF there.
		const float uBin = GuidingHash(cs ^ 0x27d4eb2fu) * (1.f / 4294967296.f);
		if (!GuideTree_Sample(leaf, shadeN, uBin, uD0, uD1,
				&d, &gPdf, isVol) || !(gPdf > 0.f))
			return 0.f;
		eval = BSDF_Evaluate(bsdf, d, &ev, &bPdf
				MATERIALS_PARAM);
		// Normalize to the single-cos convention (Disney's Evaluate
		// double-counts the cosine) so t is one function for both
		// candidate sides and the DL-side pHat evaluation.
		if (mats[bsdf->materialIndex].type == DISNEY) {
			const float cosLocal = fabs(Frame_ToLocal(&bsdf->frame, d).z);
			eval = (cosLocal > 1e-3f) ? eval / cosLocal : BLACK;
		}
		sideBsdf = false;
	} else {
		// BSDF-side candidate: Sample returns f*|cos|/p single-cos for
		// all materials - recover f*|cos| and keep the draw's event.
		float cosd;
		eval = BSDF_Sample(bsdf, uD0, uD1, &d, &bPdf, &cosd, &ev
				MATERIALS_PARAM) * bPdf;
		sideBsdf = true;
		if (Spectrum_IsBlack(eval) || !(bPdf > 0.f))
			return 0.f;
		gPdf = GuideTree_Pdf(leaf, shadeN, d, isVol);
	}
	const float pMix = (1.f - wG) * bPdf + wG * gPdf;
	if (!(pMix > 0.f))
		return 0.f;
	// Target t = f|cos| * Lhat (single-cos convention on both sides).
	const float t = Spectrum_Filter(eval) *
			GuideTree_IncidentEstimate(leaf, d);
	if (cStore) {
		*cDir = d;
		*cEval = eval;
		*cEvent = ev;
		*cUD0 = uD0;
		*cUD1 = uD1;
		*cBsdf = sideBsdf;
		*cT = t;
		*cPMix = pMix;
	}
	return t / pMix;
}

//------------------------------------------------------------------------------
// Portal-guided bounce sampling (M5, path.portal.*): GPU port of the
// PathTracer::PortalRect proposal. Each rect is 4 float4 records:
//   [0] = v0.xyz, invArea        [1] = e1.xyz, a = g22*invDet
//   [2] = e2.xyz, b = g12*invDet [3] = n.xyz,  c = g11*invDet
// (a,b,c) are the host's pre-multiplied 2x2 Gram inverse: the inside-rect
// solve u = a*de1 - b*de2, v = c*de2 - b*de1 is identical to the CPU's
// invDet*(g22,-g12 ; -g12,g11) application.
//------------------------------------------------------------------------------

// Solid-angle density of direction d from p under one rect's uniform-area
// proposal: r^2/(A*|cos_s|), zero when the ray misses or runs parallel.
OPENCL_FORCE_INLINE float Portal_RectPdfW(__global const float4 *pr,
		const float3 p, const float3 d) {
	const float3 n = MAKE_FLOAT3(pr[3].x, pr[3].y, pr[3].z);
	const float dn = dot(d, n);
	if (dn == 0.f)
		return 0.f;
	const float3 v0 = MAKE_FLOAT3(pr[0].x, pr[0].y, pr[0].z);
	const float t = dot(v0 - p, n) / dn;
	if (!(t > 0.f))
		return 0.f;
	const float3 ds = (p + t * d) - v0;
	const float de1 = ds.x * pr[1].x + ds.y * pr[1].y + ds.z * pr[1].z;
	const float de2 = ds.x * pr[2].x + ds.y * pr[2].y + ds.z * pr[2].z;
	const float u = pr[1].w * de1 - pr[2].w * de2;
	const float v = pr[3].w * de2 - pr[2].w * de1;
	if ((u < 0.f) || (u >= 1.f) || (v < 0.f) || (v >= 1.f))
		return 0.f;
	return t * t * pr[0].w / fabs(dn);
}

// Aggregate proposal density: the bounce picks one rect uniformly, so the
// marginal is (1/N)*sum of per-rect pdfs (mirrors PathTracer::PortalPdfW).
OPENCL_FORCE_INLINE float Portal_PdfW(__global const float4 *rects,
		const uint count, const float3 p, const float3 d) {
	float sum = 0.f;
	for (uint i = 0u; i < count; ++i)
		sum += Portal_RectPdfW(rects + 4u * i, p, d);
	return sum / count;
}

// False when p lies on every portal's plane (all candidate directions
// would run in-plane). Mirrored exactly between the bounce side and the
// DL-side density - on the GPU both read the MK_HIT_OBJECT decision.
OPENCL_FORCE_INLINE bool Portal_UsableAt(__global const float4 *rects,
		const uint count, const float3 p) {
	for (uint i = 0u; i < count; ++i) {
		__global const float4 *pr = rects + 4u * i;
		const float3 rel = p - MAKE_FLOAT3(pr[0].x, pr[0].y, pr[0].z);
		if (fabs(rel.x * pr[3].x + rel.y * pr[3].y + rel.z * pr[3].z) > 1e-4f)
			return true;
	}
	return false;
}

// Half-space gate (env LUX_PG_PORTALSIDE): +1 fires only on the +n side,
// -1 only on -n, 0 on both.
OPENCL_FORCE_INLINE bool Portal_SideOK(__global const float4 *rects,
		const uint count, const float sideGate, const float3 p) {
	if (sideGate == 0.f)
		return true;
	for (uint i = 0u; i < count; ++i) {
		__global const float4 *pr = rects + 4u * i;
		const float3 rel = p - MAKE_FLOAT3(pr[0].x, pr[0].y, pr[0].z);
		const float s = rel.x * pr[3].x + rel.y * pr[3].y + rel.z * pr[3].z;
		if (s * sideGate > 1e-4f)
			return true;
	}
	return false;
}

// The aperture must sit in the surface's upper hemisphere: a portal
// behind the shading normal can never deliver light to this vertex.
OPENCL_FORCE_INLINE bool Portal_FacingOK(__global const float4 *rects,
		const uint count, const float3 p, const float3 n) {
	for (uint i = 0u; i < count; ++i) {
		__global const float4 *pr = rects + 4u * i;
		const float3 ctr = MAKE_FLOAT3(pr[0].x, pr[0].y, pr[0].z) +
				.5f * (MAKE_FLOAT3(pr[1].x, pr[1].y, pr[1].z) +
				MAKE_FLOAT3(pr[2].x, pr[2].y, pr[2].z));
		if (dot(ctr - p, n) > 0.f)
			return true;
	}
	return false;
}

// Adaptive portal share (mirrors PathTracer::PortalShareAt): the
// technique earns the fraction of the leaf's incident field arriving
// through the aperture - Sum_i Omega_i * Lhat(d_i) / leaf total, capped
// by portalShare. Falls back to the full share while the field is
// untrained (portalAdapt off, guiding off, or a cold leaf).
OPENCL_FORCE_INLINE float Portal_ShareAt(__global const float4 *rects,
		const uint count, const float share, const bool adapt,
		__global const uint4 *guideNodes,
		__global const float *guideLeaves,
		const uint guidingEnable, const float3 p) {
	__global const float *leaf = (guidingEnable != 0u) ?
			GuideTree_LeafAt(guideNodes, guideLeaves, p) : NULL;
	if (!adapt || !leaf || ((uint)leaf[22] == 0u) ||
			(leaf[20] < GUIDE_WARMUP_RECORDS))
		return share;
	const float tot = max(leaf[23], 1e-9f);
	float fSum = 0.f;
	for (uint i = 0u; i < count; ++i) {
		__global const float4 *pr = rects + 4u * i;
		const float3 dc = MAKE_FLOAT3(pr[0].x, pr[0].y, pr[0].z) +
				.5f * (MAKE_FLOAT3(pr[1].x, pr[1].y, pr[1].z) +
				MAKE_FLOAT3(pr[2].x, pr[2].y, pr[2].z)) - p;
		const float d2 = max(dot(dc, dc), 1e-12f);
		const float3 dn = dc / sqrt(d2);
		// rect solid angle ~ projected area / r^2
		const float omega = fabs(dot(dn,
				MAKE_FLOAT3(pr[3].x, pr[3].y, pr[3].z))) / (pr[0].w * d2);
		fSum += omega * GuideTree_IncidentEstimate(leaf, dn);
	}
	return clamp(fSum / tot, 0.f, share);
}

// Per-sample pass for the guide bin-pick hash (mirrors Sampler::GetPass):
// RANDOM/SOBOL/PMJ02 share the leading (bucketIndex, pixelOffset,
// passOffset, pass) per-work-item layout; TilePath has its own.
// Record-buffer select (M2b-2): task t writes buffer (t&15), 8-float
// record at slot (t>>5)&127 (no atomics; overwrites are valid data)
OPENCL_FORCE_INLINE __global float4 *Guide_RecBuf(uint t,
		__global float4 *r0, __global float4 *r1,
		__global float4 *r2, __global float4 *r3,
		__global float4 *r4, __global float4 *r5,
		__global float4 *r6, __global float4 *r7,
		__global float4 *r8, __global float4 *r9,
		__global float4 *r10, __global float4 *r11,
		__global float4 *r12, __global float4 *r13,
		__global float4 *r14, __global float4 *r15) {
	switch (t & 15u) {
		case 0u: return r0;
		case 1u: return r1;
		case 2u: return r2;
		case 3u: return r3;
		case 4u: return r4;
		case 5u: return r5;
		case 6u: return r6;
		case 7u: return r7;
		case 8u: return r8;
		case 9u: return r9;
		case 10u: return r10;
		case 11u: return r11;
		case 12u: return r12;
		case 13u: return r13;
		case 14u: return r14;
		default: return r15;
	}
}

OPENCL_FORCE_INLINE uint GuidingPass(__constant const GPUTaskConfiguration* restrict taskConfig,
		const size_t gid, __global void *samplesBuff) {
	// The pass field sits at the same offset in RandomSample, SobolSample
	// and TilePathSample but the struct STRIDES differ: indexing with the
	// wrong element type lands mid-record and reads rng bit patterns as
	// the pass (observed as garbage ~1e9 values feeding GI reservoir
	// stamps and guide seeds). Dispatch on the actual sampler type.
	switch (taskConfig->sampler.type) {
		case TILEPATHSAMPLER:
			return ((__global TilePathSample *)samplesBuff)[gid].pass;
		case SOBOL:
			return ((__global SobolSample *)samplesBuff)[gid].pass;
		case PMJ02SAMPLER:
			return ((__global RandomSample *)samplesBuff)[gid].pass;
		case RANDOM:
			// RANDOM never sets sample->pass; return 0 so GI reservoir
			// stamps stay ordered (temporal reuse degrades to off).
			return 0u;
		default:
			return 0u;
	}
}

OPENCL_FORCE_INLINE bool DirectLight_BSDFSampling(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global DirectLightIlluminateInfo *info,
		const float time,
		const bool lastPathVertex,
		__global EyePathInfo *pathInfo,
		__global PathDepthInfo *tmpDepthInfo,
		__global const BSDF *bsdf,
		const float3 shadowRayDir
		LIGHTS_PARAM_DECL,
		__global const uint4* restrict guideNodes,
		__global const float* restrict guideLeaves,
		const uint guidingEnable,
		// RIS product guiding (M4b): the independent normalization
		// zHatMis of the bounce winner drawn in MK_HIT_OBJECT (0 = off).
		const float risZhat,
		// Portal bounce proposal (M5): the effective share decided in
		// MK_HIT_OBJECT (0 = off; the gates already ran there).
		__global const float4* restrict portalRects,
		const uint portalCount,
		const float portalW) {
	// Sample the BSDF
	BSDFEvent event;
	float bsdfPdfW;
	const float3 bsdfEval = BSDF_Evaluate(bsdf,
			shadowRayDir, &event, &bsdfPdfW
			MATERIALS_PARAM);

	if (Spectrum_IsBlack(bsdfEval) ||
			(taskConfig->pathTracer.hybridBackForward.enabled &&
			(taskConfig->pathTracer.hybridBackForward.adaptiveCaustic ?
				// Adaptive partition: the pending connection vertex is
				// the light-adjacent terminal; its lobe vs the light's
				// solid angle decides eye-side difficulty
				EyePathInfo_IsAdaptiveCausticPath(pathInfo, event,
					BSDF_GetGlossiness(bsdf MATERIALS_PARAM),
					taskConfig->pathTracer.hybridBackForward.terminalGlossiness,
					taskConfig->pathTracer.hybridBackForward.connectProb,
					Light_ConnectionSolidAngle(&lights[info->lightIndex],
							VLOAD3F(&bsdf->hitPoint.p.x))) :
				EyePathInfo_IsCausticPathWithEvent(pathInfo, event,
					BSDF_GetGlossiness(bsdf MATERIALS_PARAM),
					taskConfig->pathTracer.hybridBackForward.glossinessThreshold)))
			)
		return false;

	// Create a new DepthInfo for the path to the light source
	//
	// Note: I was using a local variable before to save, use and than restore
	// the depthInfo variable but it was triggering a AMD OpenCL compiler bug.
	*tmpDepthInfo = pathInfo->depth;
	PathDepthInfo_IncDepths(tmpDepthInfo, event);

	const float directLightSamplingPdfW = info->directPdfW * info->pickPdf;
	// risScale carries the RIS output weight of the ReSTIR DI reservoir
	// pick (1.f when ReSTIR is disabled). The MIS pdf stays the proposal
	// q so both MIS sides remain consistent.
	const float factor = info->risScale / directLightSamplingPdfW;

	// Path guiding (P1-3 M4e): same mixture competitor as the CPU side
	// (see PathTracer::DirectLightSampling)
	float bouncePdfW = bsdfPdfW;
	if (risZhat > 0.f) {
		// RIS product guiding (M4b): the bounce technique's effective
		// density at w is pHat = t(w)/zHatMis with t = f|cos|*Lhat
		// (single-cos convention, matching the candidate loop). risZhat
		// carries the INDEPENDENT normalization zHatMis so the density
		// stays decorrelated from the winner selection.
		float tEval = Spectrum_Filter(bsdfEval);
		if (mats[bsdf->materialIndex].type == DISNEY) {
			const float cosLocal = fabs(Frame_ToLocal(&bsdf->frame,
					shadowRayDir).z);
			tEval = (cosLocal > 1e-3f) ? tEval / cosLocal : 0.f;
		}
		__global const float *risLeaf = GuideTree_LeafAt(guideNodes,
				guideLeaves, VLOAD3F(&bsdf->hitPoint.p.x));
		bouncePdfW = tEval *
				GuideTree_IncidentEstimate(risLeaf, shadowRayDir) / risZhat;
	} else if (guidingEnable != 0u) {
		const BSDFEvent eventTypes = BSDF_GetEventTypes(bsdf MATERIALS_PARAM);
		__global const float *guideLeaf = GuideTree_LeafAt(guideNodes,
				guideLeaves, VLOAD3F(&bsdf->hitPoint.p.x));
		// Mirrors CPU GuidableBsdf(): volume scattering vertices are
		// always guidable (phase lobes sample blind w.r.t. the incident
		// field); glossy bounces need enough roughness; pure diffuse is
		// opt-in on CPU (LUX_PG_DIFFUSE) and stays off here.
		const bool guidableBsdf = bsdf->isVolume ||
				(((eventTypes & GLOSSY) != 0u) &&
				(BSDF_GetGlossiness(bsdf MATERIALS_PARAM) >= .3f));
		// Same gate as CPU CanGuide(): fitted leaf past warmup.
		if (!BSDF_IsDelta(bsdf MATERIALS_PARAM) && guidableBsdf &&
				(pathInfo->depth.depth >= 2u) && guideLeaf &&
				((uint)guideLeaf[22] > 0u) &&
				(guideLeaf[20] >= GUIDE_WARMUP_RECORDS)) {
			const float3 shadeN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
			const float wDl = Guide_MixWeight(guideLeaf[20], guideLeaf[21]);
			bouncePdfW = (1.f - wDl) * bsdfPdfW + wDl *
					GuideTree_Pdf(guideLeaf, shadeN, shadowRayDir,
					bsdf->isVolume);
		}
	}

	// Portal bounce technique (M5): the bounce-side mixture gains
	// wP*pPortal(d) + (1-wP)*rest wherever the aperture proposal can
	// fire - the mirrored predicate already ran in MK_HIT_OBJECT and
	// left its share in portalW (0 under RIS, on the GI-eligible first
	// vertex, and on a portal plane; same as the CPU gates).
	if (portalW > 0.f)
		bouncePdfW = portalW * Portal_PdfW(portalRects, portalCount,
				VLOAD3F(&bsdf->hitPoint.p.x), shadowRayDir) +
				(1.f - portalW) * bouncePdfW;

	// No Russian Roulette factor in the bounce density: RR is orthogonal to
	// direction sampling and the analog-hit weights don't carry it (see
	// PathTracer::DirectLightSampling). The old bsdfEval-based factor broke
	// MIS symmetry and inflated interior volume NEE by ~1/cap.

	// Account for material transparency
	__global const LightSource* restrict light = &lights[info->lightIndex];
	bouncePdfW *= Light_GetAvgPassThroughTransparency(light
			LIGHTS_PARAM);

	// MIS between direct light sampling and BSDF sampling
	//
	// Note: I have to avoiding MIS on the last path vertex

	const bool misEnabled = !lastPathVertex &&
			Light_IsEnvOrIntersectable(light) &&
			CheckDirectHitVisibilityFlags(light, tmpDepthInfo, event) &&
			!bsdf->hitPoint.throughShadowTransparency;

	float weight;
	if (taskConfig->pathTracer.vertexConnect.enabled) {
		// Vertex connection (M6): CPU DirectLightSampling MIS -
		//   weightLight = MIS(bsdfPdfW / directLightSamplingPdfW)
		//   weightCamera = MIS(emissionPdfW*cosThetaToLight /
		//       (directPdfW*cosThetaAtLight)) * (dVCM + dVC*MIS(revPdfW))
		//   misWeight = 1/(weightLight + 1 + weightCamera)
		float bsdfRevPdfW;
		BSDF_Pdf(bsdf, shadowRayDir, NULL, &bsdfRevPdfW MATERIALS_PARAM);
		// Non-intersectable lights can not be sampled by the BSDF
		// technique (CPU parity)
		float wLightNum = Light_IsEnvOrIntersectable(light) ?
				bouncePdfW : 0.f;
		// CPU checks (eyeVertex.depth + 1 >= rrDepth) where eyeVertex.depth
		// is 1-based and depth.depth is 0-based here: +2 (CPU parity)
		const uint vDepth = pathInfo->depth.depth + 2u;
		if (vDepth >= taskConfig->pathTracer.rrDepth) {
			const float prob = RussianRouletteProb(
					taskConfig->pathTracer.rrImportanceCap, bsdfEval);
			wLightNum *= prob;
			bsdfRevPdfW *= prob;
		}
		const float cosThetaToLight = fabs(dot(shadowRayDir,
				VLOAD3F(&bsdf->hitPoint.shadeN.x)));
		const float weightLight = VCMis(wLightNum / directLightSamplingPdfW);
		const float denom = info->directPdfW * info->cosThetaAtLight;
		const float weightCamera = (denom > 0.f) ?
				VCMis(info->emissionPdfW * cosThetaToLight / denom) *
				(pathInfo->dVCM + pathInfo->dVC * VCMis(bsdfRevPdfW)) : 0.f;
		weight = 1.f / (weightLight + 1.f + weightCamera);
	} else
		weight = misEnabled ? PowerHeuristic(directLightSamplingPdfW,
				bouncePdfW) : 1.f;
	// The shadow-transparent override is applied in MK_RT_DL once the
	// occluder flag is known (CPU parity)
	info->vcMisWeight = weight;

	const float3 lightRadiance = VLOAD3F(info->lightRadiance.c);
	VSTORE3F(bsdfEval * (weight * factor) * lightRadiance, info->lightRadiance.c);
	VSTORE3F(factor * lightRadiance, info->lightIrradiance.c);

	return true;
}

//------------------------------------------------------------------------------
// MNEE (Manifold Next Event Estimation): direct light sampling through a
// single delta specular chain x0 -> x1 -> y. Kernel port of
// src/slg/engines/pathtracer_mnee.cpp (Hanika et al. 2015; Zeltner et al.
// 2020 SS solver). The solver runs as the MK_MNEE_NEXT_VERTEX sub-state
// machine with one trace per render iteration and the persistent state in
// GPUTaskDirectLight::mnee.
//
// Disjointness (unbiasedness) with the plain direct light estimator: the
// plain estimator contributes 0 whenever a delta specular surface blocks
// the shadow ray, and forward BSDF sampling hits a positional delta light
// with probability 0. MNEE fills exactly those paths, so no MIS is needed.
// Same gates as the CPU: mirror/glass occluders, point/spot/mappoint
// lights, non-delta receivers.
//
// Porting notes: no unqualified struct pointer parameters (the CL2MSL
// translator only auto-qualifies scalar pointers, structs are passed by
// value), no OpenCL cast-style vector constructors (component assignment),
// no forward declarations (every function is defined before its first use).
//------------------------------------------------------------------------------

// Working copy of the persistent MneeState vertex (float3 arithmetic)
typedef struct {
	float3 p, dpdu, dpdv, n, gn, dndu, dndv;
	// Orthonormal tangents (Zeltner make_orthonormal result)
	float3 s, t;
	// Generalized half-vector IOR ratio: +1 same-side mirror reflection,
	// -1 opposite-side mirror reflection, nt/nc for glass transmission.
	float eta;
} MneeVtx;

OPENCL_FORCE_INLINE void Mnee_LoadVtx(__global const MneeState *mnee, MneeVtx *v) {
	v->p = MAKE_FLOAT3(mnee->vtx.px, mnee->vtx.py, mnee->vtx.pz);
	v->dpdu = MAKE_FLOAT3(mnee->vtx.dpduX, mnee->vtx.dpduY, mnee->vtx.dpduZ);
	v->dpdv = MAKE_FLOAT3(mnee->vtx.dpdvX, mnee->vtx.dpdvY, mnee->vtx.dpdvZ);
	v->n = MAKE_FLOAT3(mnee->vtx.nX, mnee->vtx.nY, mnee->vtx.nZ);
	v->gn = MAKE_FLOAT3(mnee->vtx.gnX, mnee->vtx.gnY, mnee->vtx.gnZ);
	v->dndu = MAKE_FLOAT3(mnee->vtx.dnduX, mnee->vtx.dnduY, mnee->vtx.dnduZ);
	v->dndv = MAKE_FLOAT3(mnee->vtx.dndvX, mnee->vtx.dndvY, mnee->vtx.dndvZ);
	v->s = MAKE_FLOAT3(mnee->vtx.sX, mnee->vtx.sY, mnee->vtx.sZ);
	v->t = MAKE_FLOAT3(mnee->vtx.tX, mnee->vtx.tY, mnee->vtx.tZ);
	v->eta = mnee->vtx.eta;
}

OPENCL_FORCE_INLINE void Mnee_StoreVtx(__global MneeState *mnee, const MneeVtx *v) {
	mnee->vtx.px = v->p.x; mnee->vtx.py = v->p.y; mnee->vtx.pz = v->p.z;
	mnee->vtx.dpduX = v->dpdu.x; mnee->vtx.dpduY = v->dpdu.y; mnee->vtx.dpduZ = v->dpdu.z;
	mnee->vtx.dpdvX = v->dpdv.x; mnee->vtx.dpdvY = v->dpdv.y; mnee->vtx.dpdvZ = v->dpdv.z;
	mnee->vtx.nX = v->n.x; mnee->vtx.nY = v->n.y; mnee->vtx.nZ = v->n.z;
	mnee->vtx.gnX = v->gn.x; mnee->vtx.gnY = v->gn.y; mnee->vtx.gnZ = v->gn.z;
	mnee->vtx.dnduX = v->dndu.x; mnee->vtx.dnduY = v->dndu.y; mnee->vtx.dnduZ = v->dndu.z;
	mnee->vtx.dndvX = v->dndv.x; mnee->vtx.dndvY = v->dndv.y; mnee->vtx.dndvZ = v->dndv.z;
	mnee->vtx.sX = v->s.x; mnee->vtx.sY = v->s.y; mnee->vtx.sZ = v->s.z;
	mnee->vtx.tX = v->t.x; mnee->vtx.tY = v->t.y; mnee->vtx.tZ = v->t.z;
	mnee->vtx.eta = v->eta;
}

OPENCL_FORCE_INLINE void Mnee_CoordinateSystem(const float3 n, float3 *s, float3 *t) {
	if (fabs(n.x) > fabs(n.y)) {
		const float invNorm = 1.f / sqrt(n.x * n.x + n.z * n.z);
		*s = MAKE_FLOAT3(-n.z * invNorm, 0.f, n.x * invNorm);
	} else {
		const float invNorm = 1.f / sqrt(n.y * n.y + n.z * n.z);
		*s = MAKE_FLOAT3(0.f, n.z * invNorm, -n.y * invNorm);
	}
	*t = cross(n, *s);
}

// Orthonormalize the surface parameterization (Zeltner
// ManifoldVertex::make_orthonormal)
OPENCL_FORCE_INLINE void Mnee_Orthonormalize(MneeVtx *v) {
	const float invNorm1 = 1.f / sqrt(dot(v->dpdu, v->dpdu));
	v->dpdu *= invNorm1;
	v->dndu *= invNorm1;

	const float dp = dot(v->dpdu, v->dpdv);
	const float3 dpdvTmp = v->dpdv - dp * v->dpdu;
	const float3 dndvTmp = v->dndv - dp * v->dndu;
	const float invNorm2 = 1.f / sqrt(dot(dpdvTmp, dpdvTmp));
	v->dpdv = dpdvTmp * invNorm2;
	v->dndv = dndvTmp * invNorm2;

	v->s = v->dpdu;
	v->t = v->dpdv;
}

// MneeInitVertex port. The traced kernel BSDF hit point already carries the
// mesh differentials (see HitPoint_Init()), including dndu/dndv curvature
// terms. Meshes without UVs have degenerate differentials: fall back to an
// orthonormal flat parameterization around the shading normal.
OPENCL_FORCE_INLINE void Mnee_InitVtxFromBsdf(__global const BSDF *bsdf, const float eta, MneeVtx *v) {
	v->p = VLOAD3F(&bsdf->hitPoint.p.x);
	v->dpdu = VLOAD3F(&bsdf->hitPoint.dpdu.x);
	v->dpdv = VLOAD3F(&bsdf->hitPoint.dpdv.x);
	v->n = VLOAD3F(&bsdf->hitPoint.shadeN.x);
	v->gn = VLOAD3F(&bsdf->hitPoint.geometryN.x);
	v->dndu = VLOAD3F(&bsdf->hitPoint.dndu.x);
	v->dndv = VLOAD3F(&bsdf->hitPoint.dndv.x);
	v->eta = eta;

	const float dpduLenSq = dot(v->dpdu, v->dpdu);
	const float dpdvLenSq = dot(v->dpdv, v->dpdv);
	const float dpDot = dot(v->dpdu, v->dpdv);
	if ((dpduLenSq < 1e-24f) || (dpdvLenSq < 1e-24f) ||
			(dpDot * dpDot > 0.9999f * 0.9999f * dpduLenSq * dpdvLenSq)) {
		Mnee_CoordinateSystem(v->n, &v->dpdu, &v->dpdv);
		v->dndu = MAKE_FLOAT3(0.f, 0.f, 0.f);
		v->dndv = MAKE_FLOAT3(0.f, 0.f, 0.f);
	}

	Mnee_Orthonormalize(v);
}

// Deterministic tangent offset applied to the initial vertex so the
// half-vector never degenerates exactly (CPU MNEEDirectSampling seedShift).
// The offset only selects the Newton basin; the solve itself is pulled to
// the exact constraint solution.
OPENCL_FORCE_INLINE void Mnee_ApplySeedShift(__global MneeState *mnee, const float3 x0p) {
	MneeVtx v;
	Mnee_LoadVtx(mnee, &v);
	const float seedShift = fmax(1e-4f, 1e-3f * length(x0p - v.p));
	v.p += (v.dpdu + v.dpdv) * (seedShift * 0.70710678118654752440f);
	Mnee_StoreVtx(mnee, &v);
}

// Half-vector constraint residual C = (h.s, h.t) (Zeltner
// compute_step_halfvector, n_offset = 0). ok = false on degenerate
// distances.
// lightIsDir: a directional endpoint has no finite position, so lightPos
// carries the constant unit direction toward the light instead and wo is
// independent of the vertex position.
OPENCL_FORCE_INLINE bool Mnee_Residual(const float3 x0p, const float3 lightPos,
		const bool lightIsDir, const MneeVtx *v, float2 *C) {
	*C = MAKE_FLOAT2(0.f, 0.f);

	float3 wi = x0p - v->p;
	const float r01 = length(wi);
	if (r01 < 1e-3f)
		return false;
	wi /= r01;

	float3 wo;
	if (lightIsDir) {
		wo = lightPos;
	} else {
		wo = lightPos - v->p;
		const float r12 = length(wo);
		if (r12 < 1e-3f)
			return false;
		wo /= r12;
	}

	float eta = v->eta;
	if (dot(wi, v->gn) < 0.f)
		eta = 1.f / eta;
	float3 h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	h *= 1.f / length(h);

	C->x = dot(v->s, h);
	C->y = dot(v->t, h);
	return true;
}

// Analytic constraint Jacobians and geometric term (Zeltner geometric_term
// structure, evaluated with the physical vertex eta): dw0/dx1 *
// |det(inv(J1) * J2)|. Returns g = 0.f on degenerate configurations; jac =
// J1 ({a11,a12,a21,a22}).
//
// No clamping of dx1dx2: the true Jacobian exceeds 1 near glancing
// configurations and clamping would bias the estimate (CPU notes).
OPENCL_FORCE_INLINE float Mnee_GeometricTerm(const float3 x0p, const float3 lightPos,
		const bool lightIsDir, const MneeVtx *v, float4 *j1Out) {
	if (j1Out)
		*j1Out = MAKE_FLOAT4(0.f, 0.f, 0.f, 0.f);

	float3 wi = x0p - v->p;
	const float r01 = length(wi);
	if (r01 < 1e-3f)
		return 0.f;
	wi /= r01;

	float3 wo;
	float r12 = 0.f;
	if (lightIsDir) {
		// Directional endpoint: wo is the constant light direction, it does
		// not move with the vertex (ilo = 0 for the vertex Jacobian below).
		wo = lightPos;
	} else {
		wo = lightPos - v->p;
		r12 = length(wo);
		if (r12 < 1e-3f)
			return 0.f;
		wo /= r12;
	}

	float eta = v->eta;
	if (dot(wi, v->gn) < 0.f)
		eta = 1.f / eta;
	float3 h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	const float ilh = 1.f / length(h);
	h *= ilh;
	// Vertex-side coupling of wo to x1: a point endpoint moves with x1
	// (ilo = eta*ilh/r12); a directional endpoint is constant (ilo = 0).
	const float ilo = lightIsDir ? 0.f : (1.f / r12) * eta * ilh;
	const float ili = (1.f / r01) * ilh;

	const float3 s = v->s;
	const float3 t = v->t;

	// dC/dx1 (vertex side, with curvature terms)
	float3 dhDdu = -v->dpdu * (ili + ilo) +
			wi * (dot(wi, v->dpdu) * ili) +
			wo * (dot(wo, v->dpdu) * ilo);
	float3 dhDdv = -v->dpdv * (ili + ilo) +
			wi * (dot(wi, v->dpdv) * ili) +
			wo * (dot(wo, v->dpdv) * ilo);
	dhDdu -= h * dot(dhDdu, h);
	dhDdv -= h * dot(dhDdv, h);
	if (eta != 1.f) {
		dhDdu = -dhDdu;
		dhDdv = -dhDdv;
	}
	const float dotHN = dot(h, v->n);
	const float dotHDndu = dot(h, v->dndu);
	const float dotHDndv = dot(h, v->dndv);
	const float dotDpduN = dot(v->dpdu, v->n);
	const float dotDpdvN = dot(v->dpdv, v->n);
	float4 j1;
	j1.x = dot(dhDdu, s) - dot(v->dpdu, v->dndu) * dotHN - dotDpduN * dotHDndu;
	j1.y = dot(dhDdv, s) - dot(v->dpdu, v->dndv) * dotHN - dotDpduN * dotHDndv;
	j1.z = dot(dhDdu, t) - dot(v->dpdv, v->dndu) * dotHN - dotDpdvN * dotHDndu;
	j1.w = dot(dhDdv, t) - dot(v->dpdv, v->dndv) * dotHN - dotDpdvN * dotHDndv;

	// dC/dx2: perturb the endpoint on its own measure. For a point light the
	// endpoint is the emitter position and the frame is built on -wo; moving
	// the position by eps*s2 turns wo by (s2 - wo*wo.s2)/r12, folded into ilo
	// = eta*ilh/r12. For a directional light the endpoint IS the direction:
	// rotating wo by eps*s2 changes wo by (s2 - wo*wo.s2) directly, so the
	// factor is eta*ilh (no 1/r12). The frame is perpendicular to wo either
	// way, i.e. dLight = -wo.
	const float3 dLight = -wo;
	float3 s2, t2;
	Mnee_CoordinateSystem(dLight, &s2, &t2);
	const float ilo2 = lightIsDir ? eta * ilh : ilo;
	float3 dhDdu2 = ilo2 * (s2 - wo * dot(wo, s2));
	float3 dhDdv2 = ilo2 * (t2 - wo * dot(wo, t2));
	dhDdu2 -= h * dot(dhDdu2, h);
	dhDdv2 -= h * dot(dhDdv2, h);
	if (eta != 1.f) {
		dhDdu2 = -dhDdu2;
		dhDdv2 = -dhDdv2;
	}
	float4 j2;
	j2.x = dot(dhDdu2, s);
	j2.y = dot(dhDdv2, s);
	j2.z = dot(dhDdu2, t);
	j2.w = dot(dhDdv2, t);

	if (j1Out)
		*j1Out = j1;

	const float det1 = j1.x * j1.w - j1.y * j1.z;
	const float det2 = j2.x * j2.w - j2.y * j2.z;
	if ((fabs(det1) < 1e-9f) || (fabs(det2) < 1e-15f))
		return 0.f;

	const float dx1Dx2 = fabs(det2 / det1);
	const float3 d01 = x0p - v->p;
	const float r01sq = dot(d01, d01);
	const float dw0Dx1 = fabs(dot(d01, v->gn)) / (sqrt(r01sq) * r01sq);
	return dw0Dx1 * dx1Dx2;
}

// Specular factor at the solved vertex, with LuxCore's own material code
// (mirror: Kr; glass: (1 - F) * eta^2 through the renderer's static
// evaluation, in the eye-path convention like the CPU port).
OPENCL_FORCE_INLINE float3 Mnee_SpecFactor(__global const Material *mat,
		__global const HitPoint *hitPoint, __global const BSDF *bsdf,
		const float3 wiWorld, BSDFEvent *specEvent
		MATERIALS_PARAM_DECL) {
	*specEvent = SPECULAR | REFLECT;
	if (mat->type == MIRROR) {
		return Spectrum_Clamp(Texture_GetSpectrumValue(mat->mirror.krTexIndex,
				hitPoint TEXTURES_PARAM));
	}

	const float3 kt = Spectrum_Clamp(Texture_GetSpectrumValue(mat->glass.ktTexIndex,
			hitPoint TEXTURES_PARAM));
	const float nc = ExtractExteriorIors(hitPoint, mat->glass.exteriorIorTexIndex
			TEXTURES_PARAM);
	const float nt = ExtractInteriorIors(hitPoint, mat->glass.interiorIorTexIndex
			TEXTURES_PARAM);

	const float3 localFixedDir = Frame_ToLocal(&bsdf->frame, wiWorld);
	float3 localSampledDir;
	*specEvent = SPECULAR | TRANSMIT;
	float cauchyB = 0.f;
#if defined(SLG_SPECTRAL)
	// Dispersive glass transmits at the path hero wavelength (the solver's
	// vertex eta was derived from the same hero IOR); pass the real
	// coefficient so EvalSpecularTransmission picks the spectral branch.
	if (mat->glass.cauchyBTex != NULL_INDEX)
		cauchyB = Texture_GetFloatValue(mat->glass.cauchyBTex, hitPoint
				TEXTURES_PARAM);
#endif
	return GlassMaterial_EvalSpecularTransmission(hitPoint, localFixedDir, 0.f,
			kt, nc, nt, cauchyB, &localSampledDir);
}

// Exit transition of the MNEE sub-state machine (on success and failure):
// back to the normal path advance.
OPENCL_FORCE_INLINE void Mnee_ExitTransition(__global GPUTaskState *taskState,
		__global SampleResult *sampleResult) {
	taskState->state = sampleResult->lastPathVertex ?
		MK_SPLAT_SAMPLE : MK_GENERATE_NEXT_VERTEX_RAY;
}

// Newton step: residual + analytic Jacobian at the current vertex,
// convergence check, step computation and line search proposal ray write.
// CPU newton_solver loop-top iteration bound check included. Returns 1 =
// proposal ray written, 2 = converged (*gOut = geometric term), 0 = failure.
OPENCL_FORCE_NOT_INLINE int Mnee_StepAndWriteProposal(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global MneeState *mnee,
		const float3 x0p, const float3 lightPos, const MneeVtx *v,
		__global const BSDF *x0Bsdf, __global Ray *ray, float *gOut) {
	*gOut = 0.f;

	// CPU newton_solver loop-top bound check
	if (mnee->iteration >= taskConfig->pathTracer.mnee.maxIterations)
		return 0;

	const bool lightIsDir = (mnee->lightIsDir != 0);
	float2 C;
	if (!Mnee_Residual(x0p, lightPos, lightIsDir, v, &C))
		return 0;
	const float resNorm = length(C);
	mnee->resNorm = resNorm;

	float4 jac;
	const float g = Mnee_GeometricTerm(x0p, lightPos, lightIsDir, v, &jac);
	if (resNorm < 3e-4f) {
		*gOut = g;
		return 2;
	}

	const float det = jac.x * jac.w - jac.y * jac.z;
	if (fabs(det) < 1e-9f)
		return 0;
	float2 dX;
	dX.x = (jac.w * C.x - jac.y * C.y) / det;
	dX.y = (-jac.z * C.x + jac.x * C.y) / det;
	// Clamp the step near singular configurations (det(J1) -> 0 along the
	// mirror axis): the Newton direction is still the descent direction
	// but its magnitude explodes, so cap it to a fraction of the vertex
	// distance and let the line search find the residual decrease.
	const float dXNorm = length(dX);
	const float dXMax = .25f * length(x0p - v->p);
	if (dXNorm > dXMax) {
		dX.x *= dXMax / dXNorm;
		dX.y *= dXMax / dXNorm;
	}

	// Line search proposal (Zeltner beta backtracking, extended with a
	// residual decrease check in the consuming launch; the half-vector
	// residual is strongly nonlinear for coarse seeds, so this keeps the
	// iteration inside the Newton basin).
	const float3 pProp = v->p - mnee->beta * (v->dpdu * dX.x + v->dpdv * dX.y);
	const float3 dProp = normalize(pProp - x0p);
	Ray_Init2(ray, BSDF_GetRayOrigin(x0Bsdf, dProp), dProp, ray->time);

	return 1;
}

//------------------------------------------------------------------------------
// MNEE manifold seed cache (E4).
//
// Every single-vertex solve stores its converged vertex in a fixed-size
// world-space hash grid keyed by (light, occluder mesh, quantized shadow-ray
// occluder hit position). A later attempt whose shadow ray was blocked by
// the same occluder region can then warm-start Newton from the cached vertex
// instead of the line seed (glass) or the mirrored-light seed trace
// (mirror), saving the extra trace and most iterations.
//
// The seed only selects the Newton basin: the solver still verifies the
// half-vector constraint on re-projected surface vertices, so a stale,
// colliding or torn entry can cost iterations but never biases the
// estimator. Cells are last-writer-wins; no atomics are needed.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE uint Mnee_SeedKey(
		const uint lightIndex, const uint meshIndex, const float3 p,
		const float cellSize) {
	const int cx = Floor2Int(p.x / cellSize);
	const int cy = Floor2Int(p.y / cellSize);
	const int cz = Floor2Int(p.z / cellSize);
	uint h = (uint)cx * 73856093u ^ (uint)cy * 19349663u ^
			(uint)cz * 83492791u;
	h ^= lightIndex * 2654435761u;
	h ^= meshIndex * 40503u;
	h ^= h >> 16;
	h *= 2246822519u;
	h ^= h >> 13;
	return h & (MNEE_SEED_CACHE_SIZE - 1u);
}

OPENCL_FORCE_INLINE void Mnee_SeedCacheStore(
		__global MneeSeedEntry *mneeSeeds,
		const uint key, const float3 p, const float3 n,
		const uint lightIndex, const uint meshIndex, const int mirrorMode) {
	__global MneeSeedEntry *e = &mneeSeeds[key];
	e->vx = p.x; e->vy = p.y; e->vz = p.z;
	e->nx = n.x; e->ny = n.y; e->nz = n.z;
	e->lightIndex = lightIndex;
	e->meshIndex = meshIndex;
	e->mirrorMode = (unsigned int)(mirrorMode ? 1 : 0);
	e->valid = 1u;
}

// Returns true when a usable seed was found and stored into mnee->vtx.
// The cached vertex carries a flat tangent frame (the degenerate-
// differentials fallback of Mnee_InitVtxFromBsdf): the first Newton
// proposal re-projects onto the real surface anyway.
OPENCL_FORCE_INLINE bool Mnee_SeedCacheLookup(
		__global MneeSeedEntry *mneeSeeds,
		const uint key, const uint lightIndex, const uint meshIndex,
		const int mirrorMode, const float eta,
		__global MneeState *mnee) {
	__global const MneeSeedEntry *e = &mneeSeeds[key];
	if (!e->valid || (e->lightIndex != lightIndex) ||
			(e->meshIndex != meshIndex) ||
			(e->mirrorMode != (unsigned int)(mirrorMode ? 1 : 0)))
		return false;

	const float3 p = MAKE_FLOAT3(e->vx, e->vy, e->vz);
	const float3 n = MAKE_FLOAT3(e->nx, e->ny, e->nz);
	if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) ||
			!isfinite(n.x) || !isfinite(n.y) || !isfinite(n.z) ||
			(dot(n, n) < 1e-12f))
		return false;

	MneeVtx v;
	v.p = p;
	v.n = n;
	v.gn = n;
	Mnee_CoordinateSystem(n, &v.dpdu, &v.dpdv);
	v.dndu = MAKE_FLOAT3(0.f, 0.f, 0.f);
	v.dndv = MAKE_FLOAT3(0.f, 0.f, 0.f);
	v.eta = eta;
	Mnee_Orthonormalize(&v);
	Mnee_StoreVtx(mnee, &v);
	return true;
}

// Start of the MNEE sub-state machine, called from MK_RT_DL when the direct
// light shadow ray was blocked. Returns 1 when the single vertex solve was
// started, 2 when the single vertex solver does not apply but the
// multi-specular chain might (mirror opposite-side seed: the CPU multi solver
// discovers that chain too and rejects it at the post-solve checks), 0 when
// neither applies.
OPENCL_FORCE_NOT_INLINE int Mnee_Start(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global const RayHit *rayHit, __global Ray *ray,
		__global MneeSeedEntry *mneeSeeds,
		const float worldRadius
		LIGHTS_PARAM_DECL
		) {
	if (!taskConfig->pathTracer.mnee.enabled)
		return 0;

	__global MneeState *mnee = &taskDirectLight->mnee;
	__global const DirectLightIlluminateInfo *info = &taskDirectLight->illumInfo;
	__global const BSDF *occlBsdf = &task->tmpBsdf;
	__global const Material *occlMat = &mats[occlBsdf->materialIndex];
	__global const LightSource *light = &lights[info->lightIndex];

	// Same gates as PathTracer::DirectLightSampling() on the CPU: positional
	// delta light, delta specular occluder, mirror or glass material. A
	// sharpdistant light is a delta-direction emitter, disjoint from forward
	// BSDF sampling (a sharp direction can never be sampled): it is disjoint
	// and needs no MIS. A distant/sun cone has finite solid angle - forward
	// refraction CAN reach it, so it is excluded until MIS is in place.
	const bool lightIsDir = (light->type == TYPE_SHARPDISTANT);
	if ((light->type != TYPE_POINT) && (light->type != TYPE_SPOT) &&
			(light->type != TYPE_MAPPOINT) && !lightIsDir)
		return 0;
	if (BSDF_IsShadowCatcher(&taskState->bsdf MATERIALS_PARAM))
		return 0;
	if (occlBsdf->isVolume || !occlMat->isDelta || !(occlMat->eventTypes & SPECULAR))
		return 0;
	if ((occlMat->type != MIRROR) && (occlMat->type != GLASS))
		return 0;

	// Endpoint of the specular connection. A point-like emitter has a finite
	// position: the shadow ray maxt has been rewritten by the trace to the
	// occluder distance, so recover the light distance from directPdfW (=
	// squared distance to the light, preserved by Illuminate). A directional
	// light has no finite position: the endpoint is the constant shadow-ray
	// direction toward the light (its directPdfW is a solid-angle pdf, not a
	// distance); it is stored in lightPosX/Y/Z and disambiguated by
	// mnee->lightIsDir.
	float3 lightPos;
	if (lightIsDir) {
		lightPos = normalize(VLOAD3F(&ray->d.x));
	} else {
		const float r12 = sqrt(info->directPdfW);
		if (!isfinite(r12) || (r12 < 1e-3f))
			return 0;
		lightPos = VLOAD3F(&ray->o.x) + VLOAD3F(&ray->d.x) * r12;
	}

	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	// Generalized half-vector IOR ratio of the occluder (CPU MNEEDirectSampling)
	float etaVertex;
	bool dispersive = false;
	if (occlMat->type == MIRROR) {
		const float3 gn1s = VLOAD3F(&occlBsdf->hitPoint.geometryN.x);
		const float3 x1p = VLOAD3F(&occlBsdf->hitPoint.p.x);
		const float3 toX0 = x0p - x1p;
		// For a directional endpoint lightPos holds the constant direction
		// toward the light, so toY is that direction itself.
		const float3 toY = lightIsDir ? lightPos : (lightPos - x1p);
		// +1 when the two endpoints are on the same side of the surface
		// (h = wi + wo), the only physical reflection case: the reflected ray
		// leaves with the normal component of its direction flipped, so the
		// receiver is on the same side of the tangent plane as the light. The
		// opposite-side case would need the surface to transmit, which a
		// mirror does not do (matches Mnee_Start in the CPU solver; see
		// dev-tools/sota_p1_mnee_mirror_physics_test.py).
		etaVertex = (dot(toX0, gn1s) * dot(toY, gn1s) > 0.f) ? 1.f : -1.f;
		if (etaVertex != 1.f)
			// Escalate to the chain solver (it handles directional endpoints
			// in direction space, like the single vertex solver).
			return 2;
	} else {
		const float nc = ExtractExteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
		const float nt = ExtractInteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.interiorIorTexIndex TEXTURES_PARAM);
		if ((nt <= 0.f) || (nc <= 0.f))
			return 0;
		const float cauchyB = (occlMat->glass.cauchyBTex != NULL_INDEX) ?
				Texture_GetFloatValue(occlMat->glass.cauchyBTex,
					&occlBsdf->hitPoint TEXTURES_PARAM) : 0.f;
		if (cauchyB > 0.f) {
#if defined(SLG_SPECTRAL)
			// Dispersive glass: the manifold constraint is solved at the
			// path hero wavelength (the direction-defining wavelength, same
			// as GlassMaterial_Sample's dispersive branch). The connect
			// exists only for that bin: the hero-only collapse is applied
			// at contribution assembly (mnee->dispersive).
			dispersive = true;
			etaVertex = Spectral_DispersiveIOR(nt, cauchyB,
					&occlBsdf->hitPoint) / nc;
#else
			// No wavelength state on the path: keep skipping dispersive
			// occluders like the CPU solver.
			return 0;
#endif
		} else
			etaVertex = nt / nc;
	}

	mnee->mirrorMode = (occlMat->type == MIRROR);
	mnee->dispersive = dispersive ? 1 : 0;
	mnee->chainN = 0;
	mnee->lightIsDir = lightIsDir ? 1 : 0;
	mnee->lightPosX = lightPos.x;
	mnee->lightPosY = lightPos.y;
	mnee->lightPosZ = lightPos.z;
	mnee->shadowMeshIndex = rayHit->meshIndex;
	mnee->shadowSide = (dot(normalize(VLOAD3F(&ray->d.x)),
			VLOAD3F(&occlBsdf->hitPoint.geometryN.x)) > 0.f) ? 1u : 0u;
	// Occluder hit position: seed-cache key component, reused by the
	// store on solve success (SolveEnd).
	const float3 occlP = VLOAD3F(&occlBsdf->hitPoint.p.x);
	mnee->occlX = occlP.x;
	mnee->occlY = occlP.y;
	mnee->occlZ = occlP.z;
	mnee->beta = 1.f;
	mnee->iteration = 0;
	mnee->needsTrace = false;
	mnee->seedCacheTried = 0;

	MneeVtx v;
	Mnee_InitVtxFromBsdf(occlBsdf, etaVertex, &v);
	Mnee_StoreVtx(mnee, &v);
	// The current chain vertex BSDF starts as the shadow-ray occluder
	taskDirectLight->mneeBsdfFinal = task->tmpBsdf;

	// E4: the mirror cold seed costs an extra seed trace, so the cache
	// gets first refusal for eta == 1 (a hit skips the trace entirely; the
	// Newton solve re-verifies the constraint). For glass (eta != 1) the
	// line seed is free and defines the reference basin selection - the
	// cache must not displace it: it is consulted only as a failure
	// rescue in Mnee_FailToChainOrExit (cache-first seeding pinned nearby
	// attempts to the first-cached basin, ~5% caustic energy loss
	// measured on the bumpy-sphere seedcache scene).
	if ((etaVertex == 1.f) && taskConfig->pathTracer.mnee.seedCacheEnable) {
		const float cellSize = fmax(worldRadius / MNEE_SEED_CELL_FRAC, 1e-4f);
		const uint seedMesh = rayHit->meshIndex * 2u + mnee->shadowSide;
		const uint key = Mnee_SeedKey(info->lightIndex,
				seedMesh, occlP, cellSize);
		if (Mnee_SeedCacheLookup(mneeSeeds, key, info->lightIndex,
				seedMesh, mnee->mirrorMode, etaVertex, mnee)) {
			mnee->seedCacheTried = 1;
			Mnee_ApplySeedShift(mnee, x0p);
			mnee->phase = MNEE_PHASE_STEP;
			taskState->state = MK_MNEE_NEXT_VERTEX;
			return 1;
		}
	}

	if (mnee->mirrorMode && (etaVertex == 1.f)) {
		// The shadow-ray seed lies exactly on the x0->y line, where the
		// generalized half-vector degenerates (h = wi + eta*wo =
		// (1 - eta)*wi). For a mirror (eta == 1) the line seed carries no
		// information at all and the Newton iteration diverges from there.
		// Seed the reflection chain with the first hit of the ray from x0
		// toward the light mirrored across the tangent plane at the shadow
		// hit: for a flat mirror this is the exact solution, for curved
		// reflectors the best local approximation (Zeltner's "Modified
		// MNEE" idea, with the mirrored light instead of the shape bbox
		// center).
		const float3 gn1 = VLOAD3F(&occlBsdf->hitPoint.geometryN.x);
		float3 dSeed;
		if (lightIsDir) {
			// Reflect the constant light direction across the tangent plane:
			// the virtual emitter is at infinity along the mirrored
			// direction, so the seed direction is the reflection itself.
			dSeed = lightPos - 2.f * dot(lightPos, gn1) * gn1;
		} else {
			const float3 x1Line = VLOAD3F(&occlBsdf->hitPoint.p.x);
			const float3 x1ToLight = lightPos - x1Line;
			const float proj = 2.f * dot(x1ToLight, gn1);
			const float3 mirroredLight = lightPos - proj * gn1;
			dSeed = normalize(mirroredLight - x0p);
		}
		Ray_Init2(ray, BSDF_GetRayOrigin(&taskState->bsdf, dSeed), dSeed, ray->time);

		mnee->phase = MNEE_PHASE_SEED_TRACE;
		// The seed ray was written in this same render iteration (by
		// MK_RT_DL), so rayHits[] still holds the previous trace result:
		// skip the first processing dispatch.
		mnee->needsTrace = true;
	} else {
		// eta != 1: the line seed is informative ((1 - eta) * wi != 0) and
		// is kept; same for glass (no seed trace at all).
		Mnee_ApplySeedShift(mnee, x0p);
		mnee->phase = MNEE_PHASE_STEP;
	}

	taskState->state = MK_MNEE_NEXT_VERTEX;
	return 1;
}

//------------------------------------------------------------------------------
// MNEE multi-specular chain (MNEEMultiDirectSampling port, used when
// path.mnee.maxspecular > 1 and the single vertex solve fails).
//
// The CPU solver is synchronous (traces on demand); the kernel replays the
// same sequence one trace per MK_MNEE_NEXT_VERTEX launch with the chain in
// MneeState (chainN > 0 selects this path). Every numeric step mirrors
// pathtracer_mnee.cpp exactly:
// - straight-ray discovery (MneeChainDiscover, vertex 0 re-traced instead
//   of reusing the shadow occluder),
// - residuals-first FD Jacobian with re-projection (MneeChainJacobian,
//   including the always-compute rule of trap #1),
// - block Thomas Newton step with the same beta/iteration accounting,
// - post-solve mode/TIR checks and the spec product (evaluated on fresh
//   re-projections of the solved vertices, which land deterministically on
//   the same facets, so no per-vertex BSDF slots are stored),
// - the chain geometric term from the final Newton Jacobian (same vertex,
//   so identical to the CPU's fresh evaluation) and the shared xN -> y
//   shadow + contribution phases.
//------------------------------------------------------------------------------

// Scalar-struct converters (MneeState uses component floats for the host
// C++ compile; kernel arithmetic uses float2/float4).
OPENCL_FORCE_INLINE float4 MneeMat2T_ToFloat4(__global const MneeMat2T *b) {
	return MAKE_FLOAT4(b->x, b->y, b->z, b->w);
}

OPENCL_FORCE_INLINE void MneeMat2T_FromFloat4(__global MneeMat2T *b, const float4 v) {
	b->x = v.x; b->y = v.y; b->z = v.z; b->w = v.w;
}

OPENCL_FORCE_INLINE float2 MneeVec2T_ToFloat2(__global const MneeVec2T *b) {
	return MAKE_FLOAT2(b->x, b->y);
}

OPENCL_FORCE_INLINE void MneeVec2T_FromFloat2(__global MneeVec2T *b, const float2 v) {
	b->x = v.x; b->y = v.y;
}

OPENCL_FORCE_INLINE void MneeChain_LoadVtx(__global const MneeState *mnee,
		const int i, MneeVtx *v) {
	v->p = MAKE_FLOAT3(mnee->chainVtx[i].px, mnee->chainVtx[i].py, mnee->chainVtx[i].pz);
	v->dpdu = MAKE_FLOAT3(mnee->chainVtx[i].dpduX, mnee->chainVtx[i].dpduY, mnee->chainVtx[i].dpduZ);
	v->dpdv = MAKE_FLOAT3(mnee->chainVtx[i].dpdvX, mnee->chainVtx[i].dpdvY, mnee->chainVtx[i].dpdvZ);
	v->n = MAKE_FLOAT3(mnee->chainVtx[i].nX, mnee->chainVtx[i].nY, mnee->chainVtx[i].nZ);
	v->gn = MAKE_FLOAT3(mnee->chainVtx[i].gnX, mnee->chainVtx[i].gnY, mnee->chainVtx[i].gnZ);
	v->dndu = MAKE_FLOAT3(mnee->chainVtx[i].dnduX, mnee->chainVtx[i].dnduY, mnee->chainVtx[i].dnduZ);
	v->dndv = MAKE_FLOAT3(mnee->chainVtx[i].dndvX, mnee->chainVtx[i].dndvY, mnee->chainVtx[i].dndvZ);
	v->s = MAKE_FLOAT3(mnee->chainVtx[i].sX, mnee->chainVtx[i].sY, mnee->chainVtx[i].sZ);
	v->t = MAKE_FLOAT3(mnee->chainVtx[i].tX, mnee->chainVtx[i].tY, mnee->chainVtx[i].tZ);
	v->eta = mnee->chainVtx[i].eta;
}

OPENCL_FORCE_INLINE void MneeChain_StoreVtx(__global MneeState *mnee,
		const int i, const MneeVtx *v) {
	mnee->chainVtx[i].px = v->p.x; mnee->chainVtx[i].py = v->p.y; mnee->chainVtx[i].pz = v->p.z;
	mnee->chainVtx[i].dpduX = v->dpdu.x; mnee->chainVtx[i].dpduY = v->dpdu.y; mnee->chainVtx[i].dpduZ = v->dpdu.z;
	mnee->chainVtx[i].dpdvX = v->dpdv.x; mnee->chainVtx[i].dpdvY = v->dpdv.y; mnee->chainVtx[i].dpdvZ = v->dpdv.z;
	mnee->chainVtx[i].nX = v->n.x; mnee->chainVtx[i].nY = v->n.y; mnee->chainVtx[i].nZ = v->n.z;
	mnee->chainVtx[i].gnX = v->gn.x; mnee->chainVtx[i].gnY = v->gn.y; mnee->chainVtx[i].gnZ = v->gn.z;
	mnee->chainVtx[i].dnduX = v->dndu.x; mnee->chainVtx[i].dnduY = v->dndu.y; mnee->chainVtx[i].dnduZ = v->dndu.z;
	mnee->chainVtx[i].dndvX = v->dndv.x; mnee->chainVtx[i].dndvY = v->dndv.y; mnee->chainVtx[i].dndvZ = v->dndv.z;
	mnee->chainVtx[i].sX = v->s.x; mnee->chainVtx[i].sY = v->s.y; mnee->chainVtx[i].sZ = v->s.z;
	mnee->chainVtx[i].tX = v->t.x; mnee->chainVtx[i].tY = v->t.y; mnee->chainVtx[i].tZ = v->t.z;
	mnee->chainVtx[i].eta = v->eta;
}

OPENCL_FORCE_INLINE float3 MneeChain_VtxPos(__global const MneeState *mnee, const int i) {
	return MAKE_FLOAT3(mnee->chainVtx[i].px, mnee->chainVtx[i].py, mnee->chainVtx[i].pz);
}

// The generalized half-vector constraint at one chain vertex (CPU
// MneeChainResidual port): h = wi + eta * wo parallel to the surface,
// eta flipped when the previous point is on the -geometryN side.
// woIsDir: pNext carries the constant unit light direction instead of a
// point (directional endpoint, CPU dirNext).
OPENCL_FORCE_INLINE bool MneeChain_ResidualAt(const float3 pPrev, const float3 pNext,
		const bool woIsDir, const MneeVtx *v, const float etaVertex, float2 *C) {
	float3 wi = pPrev - v->p;
	const float r0 = length(wi);
	if (r0 < 1e-4f)
		return false;
	wi /= r0;

	float3 wo;
	if (woIsDir) {
		wo = pNext;
	} else {
		wo = pNext - v->p;
		const float r1 = length(wo);
		if (r1 < 1e-4f)
			return false;
		wo /= r1;
	}

	float eta = etaVertex;
	if (dot(wi, v->gn) < 0.f)
		eta = 1.f / eta;
	float3 h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	const float l = length(h);
	if (l < 1e-5f)
		return false;
	h *= 1.f / l;

	C->x = dot(v->s, h);
	C->y = dot(v->t, h);
	return true;
}

// Residuals of the whole chain (CPU MneeChainResiduals port, no scene access).
OPENCL_FORCE_INLINE bool MneeChain_ResidualsAll(__global MneeState *mnee,
		const float3 x0p, const float3 lightPos, float *maxResOut) {
	const int n = mnee->chainN;
	float maxRes = 0.f;
	for (int i = 0; i < n; ++i) {
		MneeVtx v;
		MneeChain_LoadVtx(mnee, i, &v);
		const float3 pPrev = (i == 0) ? x0p : MneeChain_VtxPos(mnee, i - 1);
		const float3 pNext = (i == n - 1) ? lightPos : MneeChain_VtxPos(mnee, i + 1);
		const bool woDir = (mnee->lightIsDir != 0) && (i == n - 1);
		float2 C;
		if (!MneeChain_ResidualAt(pPrev, pNext, woDir, &v, v.eta, &C))
			return false;
		MneeVec2T_FromFloat2(&mnee->chainRes[i], C);
		maxRes = fmax(maxRes, length(C));
	}
	*maxResOut = maxRes;
	return true;
}

OPENCL_FORCE_INLINE void MneeChain_ZeroJacobian(__global MneeState *mnee) {
	const int n = mnee->chainN;
	const float4 zero = MAKE_FLOAT4(0.f, 0.f, 0.f, 0.f);
	for (int i = 0; i < n; ++i) {
		MneeMat2T_FromFloat4(&mnee->chainJacPrev[i], zero);
		MneeMat2T_FromFloat4(&mnee->chainJacCur[i], zero);
		MneeMat2T_FromFloat4(&mnee->chainJacNxt[i], zero);
	}
}

// FD epsilon of vertex j (CPU MneeChainJacobian port): scaled by the
// distance from the previous chain point, exactly like the CPU.
OPENCL_FORCE_INLINE float MneeChain_FdEps(__global const MneeState *mnee,
		const float3 x0p, const int j) {
	const float3 pPrev = (j == 0) ? x0p : MneeChain_VtxPos(mnee, j - 1);
	return fmax(1e-5f, 1e-4f * length(pPrev - MneeChain_VtxPos(mnee, j)));
}

// 2x2 helpers on float4 (row-major x = a11, y = a12, z = a21, w = a22).
OPENCL_FORCE_INLINE float Mnee44_Det(const float4 m) {
	return m.x * m.w - m.y * m.z;
}

OPENCL_FORCE_INLINE float4 Mnee44_Mul(const float4 A, const float4 B) {
	float4 R;
	R.x = A.x * B.x + A.y * B.z; R.y = A.x * B.y + A.y * B.w;
	R.z = A.z * B.x + A.w * B.z; R.w = A.z * B.y + A.w * B.w;
	return R;
}

OPENCL_FORCE_INLINE float4 Mnee44_Sub(const float4 A, const float4 B) {
	return MAKE_FLOAT4(A.x - B.x, A.y - B.y, A.z - B.z, A.w - B.w);
}

OPENCL_FORCE_INLINE float4 Mnee44_Inv(const float4 m, const float det) {
	return MAKE_FLOAT4(m.w / det, -m.y / det, -m.z / det, m.x / det);
}

OPENCL_FORCE_INLINE float2 Mnee42_MatVec(const float4 A, const float2 v) {
	return MAKE_FLOAT2(A.x * v.x + A.y * v.y, A.z * v.x + A.w * v.y);
}

// Block Thomas decomposition (CPU MneeTridiagonalInvert port). False on a
// singular diagonal block.
OPENCL_FORCE_INLINE bool MneeChain_ThomasInvert(__global const MneeState *mnee,
		const int n, float4 *tmp, float4 *invLambda) {
	float det = Mnee44_Det(MneeMat2T_ToFloat4(&mnee->chainJacCur[0]));
	if (fabs(det) < 1e-12f)
		return false;
	invLambda[0] = Mnee44_Inv(MneeMat2T_ToFloat4(&mnee->chainJacCur[0]), det);
	tmp[0] = MneeMat2T_ToFloat4(&mnee->chainJacPrev[0]);

	for (int i = 1; i < n; ++i) {
		tmp[i] = Mnee44_Mul(MneeMat2T_ToFloat4(&mnee->chainJacPrev[i]), invLambda[i - 1]);
		const float4 mati = Mnee44_Sub(MneeMat2T_ToFloat4(&mnee->chainJacCur[i]),
				Mnee44_Mul(tmp[i], MneeMat2T_ToFloat4(&mnee->chainJacNxt[i - 1])));
		det = Mnee44_Det(mati);
		if (fabs(det) < 1e-12f)
			return false;
		invLambda[i] = Mnee44_Inv(mati, det);
	}
	return true;
}

// Newton step with a vector right hand side (CPU MneeTridiagonalSolve port).
OPENCL_FORCE_INLINE bool MneeChain_ThomasSolveVec(__global MneeState *mnee, const int n) {
	float4 tmp[MNEE_MS_MAX_VERTICES], invLambda[MNEE_MS_MAX_VERTICES];
	if (!MneeChain_ThomasInvert(mnee, n, tmp, invLambda))
		return false;

	float2 dx[MNEE_MS_MAX_VERTICES];
	dx[0] = MneeVec2T_ToFloat2(&mnee->chainRes[0]);
	for (int i = 1; i < n; ++i) {
		const float2 back = Mnee42_MatVec(tmp[i], dx[i - 1]);
		const float2 base = MneeVec2T_ToFloat2(&mnee->chainRes[i]);
		dx[i] = MAKE_FLOAT2(base.x - back.x, base.y - back.y);
	}
	dx[n - 1] = Mnee42_MatVec(invLambda[n - 1], dx[n - 1]);
	for (int i = n - 2; i >= 0; --i) {
		const float2 back = Mnee42_MatVec(MneeMat2T_ToFloat4(&mnee->chainJacNxt[i]), dx[i + 1]);
		dx[i] = Mnee42_MatVec(invLambda[i], MAKE_FLOAT2(dx[i].x - back.x, dx[i].y - back.y));
	}
	for (int i = 0; i < n; ++i)
		MneeVec2T_FromFloat2(&mnee->chainDx[i], dx[i]);
	return true;
}

// (A^-1)_{1,N} block via a matrix right hand side (CPU
// MneeTridiagonalSolveMatrixRhs port).
OPENCL_FORCE_INLINE bool MneeChain_ThomasSolveMat(__global const MneeState *mnee,
		const int n, float4 *dxFirstOut) {
	float4 tmp[MNEE_MS_MAX_VERTICES], invLambda[MNEE_MS_MAX_VERTICES];
	if (!MneeChain_ThomasInvert(mnee, n, tmp, invLambda))
		return false;

	const float4 zero = MAKE_FLOAT4(0.f, 0.f, 0.f, 0.f);
	float4 d[MNEE_MS_MAX_VERTICES];
	for (int i = 0; i < n; ++i)
		d[i] = zero;
	d[n - 1] = MAKE_FLOAT4(1.f, 0.f, 0.f, 1.f);

	for (int i = 1; i < n; ++i)
		d[i] = Mnee44_Sub(d[i], Mnee44_Mul(tmp[i], d[i - 1]));
	d[n - 1] = Mnee44_Mul(invLambda[n - 1], d[n - 1]);
	for (int i = n - 2; i >= 0; --i)
		d[i] = Mnee44_Mul(invLambda[i], Mnee44_Sub(d[i],
				Mnee44_Mul(MneeMat2T_ToFloat4(&mnee->chainJacNxt[i]), d[i + 1])));

	*dxFirstOut = d[0];
	return true;
}

// dC_last/dy (CPU MneeChainLightJacobian port). For a directional endpoint
// (lightIsDir) lightPos carries the constant light direction and the
// perturbation tilts it instead of moving a point (the position->direction
// 1/r conversion does not apply, same as Mnee_GeometricTerm's j2 block).
OPENCL_FORCE_INLINE float4 MneeChain_LightJac(const float3 pPrev, const float3 lightPos,
		const bool lightIsDir, const MneeVtx *v, const float etaVertex,
		const float eps) {
	const float4 zero = MAKE_FLOAT4(0.f, 0.f, 0.f, 0.f);

	const float3 wo = lightIsDir ? lightPos : normalize(lightPos - v->p);
	const float3 dLight = -wo;
	float3 s2, t2;
	Mnee_CoordinateSystem(dLight, &s2, &t2);

	const float3 wi = normalize(pPrev - v->p);
	float eta = etaVertex;
	if (dot(wi, v->gn) < 0.f)
		eta = 1.f / eta;

	float3 h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	const float l = length(h);
	if (l < 1e-6f)
		return zero;
	h *= 1.f / l;
	const float C0x = dot(v->s, h), C0y = dot(v->t, h);

	float CPx[2], CPy[2];
	for (int k = 0; k < 2; ++k) {
		// Point endpoint: move the position on the tangent frame.
		// Directional endpoint: tilt the direction by eps on the same frame.
		const float3 woP = lightIsDir ?
				normalize(lightPos + ((k == 0) ? eps * s2 : eps * t2)) :
				normalize(lightPos + ((k == 0) ? eps * s2 : eps * t2) - v->p);
		float3 hP = wi + eta * woP;
		if (eta != 1.f)
			hP = -hP;
		hP *= 1.f / length(hP);
		CPx[k] = dot(v->s, hP);
		CPy[k] = dot(v->t, hP);
	}

	return MAKE_FLOAT4((CPx[0] - C0x) / eps, (CPx[1] - C0x) / eps,
			(CPy[0] - C0y) / eps, (CPy[1] - C0y) / eps);
}

// Line search trial position of vertex i (CPU trial loop port).
OPENCL_FORCE_INLINE float3 MneeChain_TrialPos(__global MneeState *mnee, const int i) {
	MneeVtx v;
	MneeChain_LoadVtx(mnee, i, &v);
	const float2 dx = MneeVec2T_ToFloat2(&mnee->chainDx[i]);
	return v.p - mnee->beta * (v.dpdu * dx.x + v.dpdv * dx.y);
}

// Re-projection ray writer (CPU MneeReproject port): from slightly above
// the surface along +gn, looking down -gn with a short range.
OPENCL_FORCE_INLINE void MneeChain_WriteReprojectRay(const float3 p, const float3 gn,
		const float gap, __global Ray *ray, const float time) {
	const float3 origin = MAKE_FLOAT3(p.x + gap * gn.x, p.y + gap * gn.y, p.z + gap * gn.z);
	const float3 dir = MAKE_FLOAT3(-gn.x, -gn.y, -gn.z);
	Ray_Init3(ray, origin, dir, .1f, time);
	// CPU parity (pathtracer_mnee.cpp MneeReproject builds its ray with
	// mint exactly 0): Ray_Init3 lifts mint to the machine epsilon
	// (~1e-5), but the re-projection target sits only ~gap above the
	// surface and gap reaches 5e-6 (FdEps floor 1e-5 halved) for
	// closely spaced chain vertices. With mint at 1e-5 those rays sit
	// exactly on the hit/miss boundary and fail by float rounding, which
	// aborts the chain (JACPERT/TRIAL/COMMIT/POST exits) and shows up as
	// missing value with run-varying, task-mapping-sensitive shortfall.
	// A 5x thicker slab (gaps far above mint) renders clean, confirming
	// this boundary as the mechanism.
	ray->mint = 0.f;
}

// Straight discovery ray writer (CPU MneeChainDiscover port).
OPENCL_FORCE_INLINE void MneeChain_WriteDiscoverRay(__global const BSDF *originBsdf,
		const float3 dir, __global Ray *ray, const float time) {
	Ray_Init2(ray, BSDF_GetRayOrigin(originBsdf, dir), dir, time);
}

// Continue the discovery walk through a collected vertex: refract at
// dielectrics (etaVertex = interior/exterior), reflect at mirrors and on
// TIR. Walking the physical refraction - instead of marching along the
// straight receiver -> endpoint line - finds interfaces the straight ray
// misses when the solved path deviates far from it (CPU MneeChainWalkDir
// port).
OPENCL_FORCE_INLINE float3 MneeChain_WalkDir(const float3 d,
		__global const BSDF *vBsdf, const float etaVertex, const bool isMirror) {
	// d travels into the vertex; geometryN is the raw mesh normal. When it
	// faces the incident side the ray enters the denser medium
	// (etaRel = nc/nt), otherwise it exits (etaRel = nt/nc) - the same
	// side test the solver's half-vector residual uses (dot(wi, gn)).
	const float3 gn = VLOAD3F(&vBsdf->hitPoint.geometryN.x);
	if (isMirror)
		return d - 2.f * dot(d, gn) * gn;

	float3 N = gn;
	float cosI = -dot(d, N);
	float etaRel;
	if (cosI > 0.f)
		etaRel = 1.f / etaVertex;
	else {
		N = -N;
		cosI = -cosI;
		etaRel = etaVertex;
	}
	const float sin2T = etaRel * etaRel * (1.f - cosI * cosI);
	return (sin2T >= 1.f) ? (d - 2.f * dot(d, N) * N)
			: (etaRel * d + (etaRel * cosI - sqrt(1.f - sin2T)) * N);
}

// Shared chain initializer. Returns the capped vertex budget.
OPENCL_FORCE_INLINE int MneeChain_Begin(__global MneeState *mnee, const unsigned int maxSpecular) {
	mnee->chainN = 0;
	mnee->dispersive = 0;
	mnee->chainMaxV = (maxSpecular < MNEE_MS_MAX_VERTICES) ? (int)maxSpecular : MNEE_MS_MAX_VERTICES;
	mnee->chainIdx = 0;
	mnee->chainSub = 0;
	mnee->chainProjected = 1;
	mnee->walkInGlass = 0;
	mnee->chainSpecR = 1.f; mnee->chainSpecG = 1.f; mnee->chainSpecB = 1.f;
	mnee->beta = 1.f;
	mnee->iteration = 0;
	return mnee->chainMaxV;
}

// Initialize chain vertex 0 from the shadow-ray occluder BSDF (CPU
// MneeChainVertexInit port). The caller guarantees a delta specular
// mirror/glass hit. Returns the eta, or -1.f when the vertex is unusable
// (non-spectral dispersive glass keeps ending the chain like the CPU).
OPENCL_FORCE_INLINE float MneeChain_InitVtxZero(
		__global MneeState *mnee,
		__global const BSDF *occlBsdf,
		__global const Material *occlMat
		MATERIALS_PARAM_DECL
		) {
	float etaVertex = 1.f;
	if (occlMat->type == MIRROR)
		etaVertex = 1.f;
	else {
		const float nc = ExtractExteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
		const float nt = ExtractInteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.interiorIorTexIndex TEXTURES_PARAM);
		if ((nt <= 0.f) || (nc <= 0.f))
			return -1.f;
		const float cauchyB = (occlMat->glass.cauchyBTex != NULL_INDEX) ?
				Texture_GetFloatValue(occlMat->glass.cauchyBTex,
					&occlBsdf->hitPoint TEXTURES_PARAM) : 0.f;
		if (cauchyB > 0.f) {
#if defined(SLG_SPECTRAL)
			// Hero-wavelength eta (see Mnee_Start); the chain-level
			// dispersive flag marks the connect for hero collapse.
			mnee->dispersive = 1;
			etaVertex = Spectral_DispersiveIOR(nt, cauchyB,
					&occlBsdf->hitPoint) / nc;
#else
			return -1.f;
#endif
		} else
			etaVertex = nt / nc;
	}

	MneeVtx vv;
	Mnee_InitVtxFromBsdf(occlBsdf, etaVertex, &vv);
	MneeChain_StoreVtx(mnee, 0, &vv);
	mnee->chainMatType[0] = occlMat->type;
	mnee->chainN = 1;
	return etaVertex;
}

// Start the chain from the shadow-ray context (micro kernel, when the single
// vertex solver was not started). Same gates as Mnee_Start except the mirror
// side gate and the Cauchy gate: the CPU multi solver discovers those chains
// too and rejects them at the post-solve checks, so the outcome matches while
// the discovery burns a few traces.
OPENCL_FORCE_NOT_INLINE bool MneeChain_StartFromShadow(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global Ray *ray,
		__global PathVolumeInfo *dlVolInfo,
		__global EyePathInfo *pathInfo
		LIGHTS_PARAM_DECL
		) {
	if (!taskConfig->pathTracer.mnee.enabled)
		return false;
	if (taskConfig->pathTracer.mnee.maxSpecular <= 1)
		return false;

	__global const DirectLightIlluminateInfo *info = &taskDirectLight->illumInfo;
	__global const BSDF *occlBsdf = &task->tmpBsdf;
	__global const Material *occlMat = &mats[occlBsdf->materialIndex];
	__global const LightSource *light = &lights[info->lightIndex];

	const bool lightIsDir = (light->type == TYPE_SHARPDISTANT);
	if ((light->type != TYPE_POINT) && (light->type != TYPE_SPOT) &&
			(light->type != TYPE_MAPPOINT) && !lightIsDir)
		return false;
	if (BSDF_IsShadowCatcher(&taskState->bsdf MATERIALS_PARAM))
		return false;
	if (occlBsdf->isVolume || !occlMat->isDelta || !(occlMat->eventTypes & SPECULAR))
		return false;
	if ((occlMat->type != MIRROR) && (occlMat->type != GLASS))
		return false;

	float3 lightPos;
	if (lightIsDir) {
		// Directional endpoint: the manifold endpoint is the constant light
		// direction (its directPdfW is a solid-angle pdf, not a distance).
		lightPos = normalize(VLOAD3F(&ray->d.x));
	} else {
		const float r12 = sqrt(info->directPdfW);
		if (!isfinite(r12) || (r12 < 1e-3f))
			return false;
		lightPos = VLOAD3F(&ray->o.x) + VLOAD3F(&ray->d.x) * r12;
	}
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	__global MneeState *mnee = &taskDirectLight->mnee;
	mnee->lightIsDir = lightIsDir ? 1 : 0;
	mnee->lightPosX = lightPos.x;
	mnee->lightPosY = lightPos.y;
	mnee->lightPosZ = lightPos.z;
	MneeChain_Begin(mnee, taskConfig->pathTracer.mnee.maxSpecular);

	// Vertex 0 is the shadow-ray occluder itself (CPU MneeChainDiscover
	// takes firstBsdf as given; re-tracing it with a different ray type
	// lands microscopically elsewhere and the Newton starts off-solution).
	const float eta0 = MneeChain_InitVtxZero(mnee, occlBsdf, occlMat MATERIALS_PARAM);
	if (eta0 < 0.f)
		return false;

	// Discover from vertex 1 on walking the physical refraction/reflection
	// at each collected interface (the straight line misses interfaces
	// where the solved path deviates far from it). For a directional
	// endpoint the walk direction is the constant light direction.
	const float3 dIn0 = lightIsDir ? lightPos : normalize(lightPos - x0p);
	const float3 dir = MneeChain_WalkDir(dIn0,
			occlBsdf, eta0, occlMat->type == MIRROR);
	if (occlMat->type == GLASS)
		mnee->walkInGlass = dot(dIn0,
				VLOAD3F(&occlBsdf->hitPoint.geometryN.x)) < 0.f;
	MneeChain_WriteDiscoverRay(occlBsdf, dir, ray, ray->time);
	*dlVolInfo = pathInfo->volume;
	mnee->phase = MNEE_PHASE_MS_DISCOVER;
	mnee->needsTrace = true;
	return true;
}

// Start the chain after a failed single vertex solve (inside the MNEE
// sub-state machine: the shadow context is gone, but the light position
// persists and vertex 0 is re-traced).
OPENCL_FORCE_NOT_INLINE bool MneeChain_StartFromSSFail(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global Ray *ray,
		__global PathVolumeInfo *dlVolInfo,
		__global EyePathInfo *pathInfo
		LIGHTS_PARAM_DECL
		) {
	if (taskConfig->pathTracer.mnee.maxSpecular <= 1)
		return false;

	__global MneeState *mnee = &taskDirectLight->mnee;
	MneeChain_Begin(mnee, taskConfig->pathTracer.mnee.maxSpecular);

	__global const BSDF *occlBsdf = &task->tmpBsdf;
	__global const Material *occlMat = &mats[occlBsdf->materialIndex];
	const float eta0 = MneeChain_InitVtxZero(mnee, occlBsdf, occlMat MATERIALS_PARAM);
	if (eta0 < 0.f)
		return false;

	const float3 lightPos = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY, mnee->lightPosZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);
	// Directional endpoint (lightIsDir set by Mnee_Start): the stored value
	// is the constant light direction, not a position.
	const float3 dIn0 = (mnee->lightIsDir != 0) ? lightPos :
			normalize(lightPos - x0p);
	const float3 dir = MneeChain_WalkDir(dIn0,
			occlBsdf, eta0, occlMat->type == MIRROR);
	if (occlMat->type == GLASS)
		mnee->walkInGlass = dot(dIn0,
				VLOAD3F(&occlBsdf->hitPoint.geometryN.x)) < 0.f;
	MneeChain_WriteDiscoverRay(occlBsdf, dir, ray, ray->time);
	*dlVolInfo = pathInfo->volume;
	mnee->phase = MNEE_PHASE_MS_DISCOVER;
	return true;
}

// Single vertex failure fallback: the CPU tries MNEEMultiDirectSampling
// after every failed MNEEDirectSampling when maxspecular > 1 (different path
// structures, so no double counting). On success the state stays
// MK_MNEE_NEXT_VERTEX with a discovery ray in flight, else exit.
OPENCL_FORCE_INLINE void Mnee_FailToChainOrExit(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global Ray *ray,
		__global PathVolumeInfo *dlVolInfo,
		__global EyePathInfo *pathInfo,
		__global SampleResult *sampleResult,
		__global MneeSeedEntry *mneeSeeds,
		const float worldRadius
		LIGHTS_PARAM_DECL
		) {
	__global MneeState *mnee = &taskDirectLight->mnee;

	// Cold-first seed policy (CPU pathtracer_mnee.cpp parity): a failed
	// single-vertex solve retries once from the cached vertex before
	// escalating to the chain solver. Single-vertex phases only - a chain
	// failure (phase >= MS_DISCOVER) has no second seed to try.
	if (!mnee->seedCacheTried &&
			(mnee->phase < MNEE_PHASE_MS_DISCOVER) &&
			taskConfig->pathTracer.mnee.seedCacheEnable && mneeSeeds) {
		mnee->seedCacheTried = 1;
		const float3 occlP = MAKE_FLOAT3(mnee->occlX, mnee->occlY, mnee->occlZ);
		const float cellSize = fmax(worldRadius / MNEE_SEED_CELL_FRAC, 1e-4f);
		const uint seedMesh = mnee->shadowMeshIndex * 2u + mnee->shadowSide;
		const uint key = Mnee_SeedKey(taskDirectLight->illumInfo.lightIndex,
				seedMesh, occlP, cellSize);
		MneeVtx v;
		Mnee_LoadVtx(mnee, &v);
		if (Mnee_SeedCacheLookup(mneeSeeds, key,
				taskDirectLight->illumInfo.lightIndex, seedMesh,
				mnee->mirrorMode, v.eta, mnee)) {
			const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);
			Mnee_ApplySeedShift(mnee, x0p);
			mnee->phase = MNEE_PHASE_STEP;
			mnee->iteration = 0;
			mnee->beta = 1.f;
			return;
		}
	}

	if (!MneeChain_StartFromSSFail(taskConfig, task, taskDirectLight, taskState,
			ray, dlVolInfo, pathInfo
			LIGHTS_PARAM))
		Mnee_ExitTransition(taskState, sampleResult);
}

// Second segment setup for a solved chain (the Illuminate tail of
// Mnee_SolveEnd, without its single vertex checks): the volume state was
// accumulated over the MS_POST phases and mneeBsdfFinal holds the last
// vertex BSDF.
OPENCL_FORCE_NOT_INLINE bool MneeChain_Seg2Setup(
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global const Ray *ray, const uint taskGid,
		const float worldCenterX, const float worldCenterY,
		const float worldCenterZ, const float worldRadius,
		__global Ray *rayOut
		LIGHTS_PARAM_DECL
		) {
	__global MneeState *mnee = &taskDirectLight->mnee;

	const uint hashBase = SobolSequence_BlueNoiseHash(
			SobolSequence_BlueNoiseHash(taskDirectLight->seedPassThroughEvent.s1) ^
			(taskGid * 0x9E3779B9u + 0x85EBCA6Bu));
	const float uSeg1 = SobolSequence_BlueNoiseHash(hashBase ^ 0x68E31DE4u) * (1.f / 4294967296.f);
	const float uSeg2 = SobolSequence_BlueNoiseHash(hashBase ^ 0xB5AD78CEu) * (1.f / 4294967296.f);
	const float uSeg3 = SobolSequence_BlueNoiseHash(hashBase ^ 0xD6E8FEB8u) * (1.f / 4294967296.f);
	mnee->seg2PassThrough = SobolSequence_BlueNoiseHash(hashBase ^ 0x2EB0D5B5u) * (1.f / 4294967296.f);

	float directPdfW2;
	const float3 lightRadiance2 = Light_Illuminate(
			&lights[taskDirectLight->illumInfo.lightIndex],
			&taskDirectLight->mneeBsdfFinal,
			ray->time, uSeg1, uSeg2, uSeg3,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius,
			&task->tmpHitPoint, rayOut, &directPdfW2, NULL, NULL
			LIGHTS_PARAM);

	if (Spectrum_IsBlack(lightRadiance2) || !isfinite(directPdfW2))
		return false;

	if (mnee->lightIsDir != 0) {
		// Illuminate() resampled a direction inside the emitter lobe; the
		// manifold endpoint is the fixed direction the solve ran for. Rebuild
		// the last-segment ray toward it and extend to the scene bounding
		// sphere (a miss = the directional light is reached). Same
		// construction as the single vertex Mnee_SolveEnd.
		const float3 wo2 = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY,
				mnee->lightPosZ);
		const float3 o2 = BSDF_GetRayOrigin(&taskDirectLight->mneeBsdfFinal, wo2);
		const float3 toCenter = MAKE_FLOAT3(worldCenterX, worldCenterY,
				worldCenterZ) - o2;
		const float approach = dot(toCenter, wo2);
		const float dist = approach + sqrt(fmax(0.f,
				worldRadius * worldRadius - dot(toCenter, toCenter) +
				approach * approach));
		Ray_Init4(rayOut, o2, wo2, 0.f, dist, ray->time);
	}

	mnee->lightRadiance2R = lightRadiance2.x;
	mnee->lightRadiance2G = lightRadiance2.y;
	mnee->lightRadiance2B = lightRadiance2.z;
	mnee->directPdfW2 = directPdfW2;
	mnee->phase = MNEE_PHASE_SEG2_TRACE;
	return true;
}

// Trial re-projection epsilon (CPU trial loop port): scaled by the distance
// from the receiver, unlike the Jacobian epsilon above.
OPENCL_FORCE_INLINE float MneeChain_TrialEps(const float3 x0p, const float3 p) {
	return fmax(1e-5f, 1e-4f * length(x0p - p));
}

// Perturbation ray writer for the FD Jacobian (CPU MneeChainJacobian port).
OPENCL_FORCE_INLINE void MneeChain_WritePerturbRay(__global MneeState *mnee,
		const float3 x0p, const int j, const int k,
		__global Ray *ray, const float time) {
	MneeVtx v;
	MneeChain_LoadVtx(mnee, j, &v);
	const float eps = MneeChain_FdEps(mnee, x0p, j);
	const float3 d = (k == 0) ? v.dpdu : v.dpdv;
	const float3 pPert = MAKE_FLOAT3(v.p.x + eps * d.x, v.p.y + eps * d.y, v.p.z + eps * d.z);
	MneeChain_WriteReprojectRay(pPert, v.gn, .5f * eps, ray, time);
}

// Jacobian pass opener (CPU MneeChainJacobian head port): residuals first,
// always computed even at zero (trap #1), then the first perturb ray.
OPENCL_FORCE_NOT_INLINE bool MneeChain_WriteJacStart(
		__global MneeState *mnee, const float3 x0p, const float3 lightPos,
		const unsigned int maxIterations,
		__global Ray *ray, __global PathVolumeInfo *dlVolInfo,
		__global const PathVolumeInfo *srcVol) {
	if (mnee->iteration >= maxIterations)
		return false;
	float maxRes;
	if (!MneeChain_ResidualsAll(mnee, x0p, lightPos, &maxRes))
		return false;
	mnee->resNorm = maxRes;
	MneeChain_ZeroJacobian(mnee);
	MneeChain_WritePerturbRay(mnee, x0p, 0, 0, ray, ray->time);
	*dlVolInfo = *srcVol;
	mnee->chainIdx = 0;
	mnee->chainSub = 0;
	mnee->phase = MNEE_PHASE_MS_JACPERT;
	return true;
}

// Trial pass opener (accept and retry share it; beta is already updated).
OPENCL_FORCE_INLINE void MneeChain_WriteTrial0(
		__global MneeState *mnee, const float3 x0p,
		__global Ray *ray, __global PathVolumeInfo *dlVolInfo,
		__global const PathVolumeInfo *srcVol) {
	const float3 pProp = MneeChain_TrialPos(mnee, 0);
	MneeVtx v;
	MneeChain_LoadVtx(mnee, 0, &v);
	MneeChain_WriteReprojectRay(pProp, v.gn,
			.5f * MneeChain_TrialEps(x0p, v.p), ray, ray->time);
	*dlVolInfo = *srcVol;
	mnee->chainIdx = 0;
	mnee->chainProjected = 1;
	mnee->phase = MNEE_PHASE_MS_TRIAL;
}

// Multi-specular chain sub-state machine (one trace per launch, same rhythm
// as the single vertex machine above).
OPENCL_FORCE_NOT_INLINE void MneeChain_ProcessState(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global EyePathInfo *pathInfo,
		__global Ray *ray, __global RayHit *rayHit,
		__global PathVolumeInfo *dlVolInfo,
		__global SampleResult *sampleResult, const uint taskGid,
		const float worldCenterX, const float worldCenterY,
		const float worldCenterZ, const float worldRadius
		LIGHTS_PARAM_DECL
		) {
	__global MneeState *mnee = &taskDirectLight->mnee;
	const float3 lightPos = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY, mnee->lightPosZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	//--------------------------------------------------------------------------
	// Trace consumption shared by DISCOVER/JACPERT/TRIAL/COMMIT/POST (all
	// INDIRECT rays; the xN -> y shadow uses the shared SEG2 phase).
	//--------------------------------------------------------------------------
	int throughShadowTransparency = false;
	float3 connectionThroughput;
	const bool continueToTrace = Scene_Intersect(taskConfig,
			EYE_RAY | INDIRECT_RAY,
			// Manifold-walk rays have no path depth/event context (same
			// defaults as the CPU MNEE code)
			NULL, NONE,
			&throughShadowTransparency, dlVolInfo, &task->tmpHitPoint,
			.5f, ray, rayHit, &taskDirectLight->mneeBsdf, &connectionThroughput,
			WHITE, sampleResult, false
			MATERIALS_PARAM);
	if (continueToTrace)
		return;

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_DISCOVER: straight-ray chain topology (CPU
	// MneeChainDiscover port; vertex 0 re-traced).
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_DISCOVER) {
		if (rayHit->meshIndex == NULL_INDEX) {
			// The ray escaped: the collected vertices may still see the
			// light (CPU: break out of the discovery loop).
			if (mnee->chainN >= 2) {
				if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, &pathInfo->volume))
					Mnee_ExitTransition(taskState, sampleResult);
			} else
				Mnee_ExitTransition(taskState, sampleResult);
			return;
		}

		__global const Material *hitMat = &mats[taskDirectLight->mneeBsdf.materialIndex];
		float etaVertex = 1.f;
		bool vertexOk = true;
		if (taskDirectLight->mneeBsdf.isVolume || !hitMat->isDelta ||
				!(hitMat->eventTypes & SPECULAR))
			vertexOk = false;
		else if (hitMat->type == MIRROR)
			etaVertex = 1.f;
		else if (hitMat->type == GLASS) {
			const float nc = ExtractExteriorIors(&taskDirectLight->mneeBsdf.hitPoint,
					hitMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
			const float nt = ExtractInteriorIors(&taskDirectLight->mneeBsdf.hitPoint,
					hitMat->glass.interiorIorTexIndex TEXTURES_PARAM);
			const float cauchyB = (hitMat->glass.cauchyBTex != NULL_INDEX) ?
					Texture_GetFloatValue(hitMat->glass.cauchyBTex,
						&taskDirectLight->mneeBsdf.hitPoint TEXTURES_PARAM) : 0.f;
			if ((nt <= 0.f) || (nc <= 0.f))
				vertexOk = false;
			else if (cauchyB > 0.f) {
#if defined(SLG_SPECTRAL)
				// Hero-wavelength eta: the chain solves for the path hero
				// bin only, the contribution collapses at assembly.
				mnee->dispersive = 1;
				etaVertex = Spectral_DispersiveIOR(nt, cauchyB,
						&taskDirectLight->mneeBsdf.hitPoint) / nc;
#else
				vertexOk = false;
#endif
			} else
				etaVertex = nt / nc;
		} else
			vertexOk = false;

		if (!vertexOk) {
			if (mnee->walkInGlass && (mnee->chainSub < 8)) {
				++mnee->chainSub;   // bounded intrusion skips per walk
				// Opaque intrusion inside the dielectric: step past it
				// along the same direction and keep collecting (CPU
				// MneeChainDiscover parity) - the solver validates the
				// final path, discovery only needs the topology.
				MneeChain_WriteDiscoverRay(&taskDirectLight->mneeBsdf,
						VLOAD3F(&ray->d.x), ray, ray->time);
				return;
			}
			// A non specular surface ends the chain (CPU: break).
			if (mnee->chainN >= 2) {
				if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, &pathInfo->volume))
					Mnee_ExitTransition(taskState, sampleResult);
			} else
				Mnee_ExitTransition(taskState, sampleResult);
			return;
		}

		MneeVtx vv;
		Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf, etaVertex, &vv);
		MneeChain_StoreVtx(mnee, mnee->chainN, &vv);
		mnee->chainMatType[mnee->chainN] = hitMat->type;
		mnee->chainN++;

		if (mnee->chainN >= mnee->chainMaxV) {
			if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, &pathInfo->volume))
				Mnee_ExitTransition(taskState, sampleResult);
			return;
		}

		const float3 dIn = VLOAD3F(&ray->d.x);
		const float3 dir = MneeChain_WalkDir(dIn,
				&taskDirectLight->mneeBsdf,
				etaVertex, hitMat->type == MIRROR);
		if (hitMat->type == GLASS)
			// Entering when the mesh normal faces the incident side; a
			// TIR bounce keeps the walk inside either way.
			mnee->walkInGlass = (dot(dIn,
					VLOAD3F(&taskDirectLight->mneeBsdf.hitPoint.geometryN.x)) < 0.f) ||
					(dot(dIn, dir) < 0.f);
		MneeChain_WriteDiscoverRay(&taskDirectLight->mneeBsdf, dir, ray, ray->time);
		*dlVolInfo = pathInfo->volume;
		return;
	}

	// The phases below only run with a complete chain.
	const int nn = mnee->chainN;

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_JACPERT: FD Jacobian column from perturb(vertex
	// chainIdx, axis chainSub). One perturb measures the affected rows
	// chainIdx-1..chainIdx+1 (CPU MneeChainJacobian port).
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_JACPERT) {
		const int j = mnee->chainIdx;
		const int k = mnee->chainSub;
		if (rayHit->meshIndex == NULL_INDEX) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}

		MneeVtx pertV;
		Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf,
				mnee->chainVtx[j].eta, &pertV);
		const float eps = MneeChain_FdEps(mnee, x0p, j);
		for (int jj = j - 1; jj <= j + 1; ++jj) {
			if ((jj < 0) || (jj >= nn))
				continue;
			const float3 pPrevJJ = (jj == 0) ? x0p :
					((jj - 1 == j) ? pertV.p : MneeChain_VtxPos(mnee, jj - 1));
			const float3 pNextJJ = (jj == nn - 1) ? lightPos :
					((jj + 1 == j) ? pertV.p : MneeChain_VtxPos(mnee, jj + 1));
			MneeVtx vjj;
			if (jj == j)
				vjj = pertV;
			else
				MneeChain_LoadVtx(mnee, jj, &vjj);
			float2 CP;
			const bool woDirJJ = (mnee->lightIsDir != 0) && (jj == nn - 1);
			if (!MneeChain_ResidualAt(pPrevJJ, pNextJJ, woDirJJ, &vjj, vjj.eta, &CP)) {
				Mnee_ExitTransition(taskState, sampleResult);
				return;
			}
			const float dCx = (CP.x - mnee->chainRes[jj].x) / eps;
			const float dCy = (CP.y - mnee->chainRes[jj].y) / eps;
			__global MneeMat2T *blockOut = (jj == j - 1) ? &mnee->chainJacNxt[jj] :
					((jj == j) ? &mnee->chainJacCur[jj] : &mnee->chainJacPrev[jj]);
			float4 b = MneeMat2T_ToFloat4(blockOut);
			if (k == 0) { b.x = dCx; b.z = dCy; } else { b.y = dCx; b.w = dCy; }
			MneeMat2T_FromFloat4(blockOut, b);
		}

		if (k == 0) {
			mnee->chainSub = 1;
			MneeChain_WritePerturbRay(mnee, x0p, j, 1, ray, ray->time);
			*dlVolInfo = pathInfo->volume;
			return;
		}
		if (j + 1 < nn) {
			mnee->chainIdx = j + 1;
			mnee->chainSub = 0;
			MneeChain_WritePerturbRay(mnee, x0p, j + 1, 0, ray, ray->time);
			*dlVolInfo = pathInfo->volume;
			return;
		}

		// Jacobian complete: Newton loop top (CPU MNEEMultiDirectSampling
		// port). Solved check first, the Jacobian was already computed.
		if (mnee->resNorm < 1e-5f) {
			float4 dxFirst;
			if (!MneeChain_ThomasSolveMat(mnee, nn, &dxFirst)) {
				Mnee_ExitTransition(taskState, sampleResult);
				return;
			}
			MneeVtx vlast;
			MneeChain_LoadVtx(mnee, nn - 1, &vlast);
			const float3 pPrevLast = (nn == 1) ? x0p : MneeChain_VtxPos(mnee, nn - 2);
			const float epsLight = fmax(1e-5f, 1e-4f * length(x0p - vlast.p));
			const float4 lightJac = MneeChain_LightJac(pPrevLast, lightPos,
					mnee->lightIsDir != 0, &vlast, vlast.eta, epsLight);
			const float4 dxDy = Mnee44_Mul(dxFirst, lightJac);
			MneeVtx vfirst;
			MneeChain_LoadVtx(mnee, 0, &vfirst);
			const float3 d01 = MAKE_FLOAT3(x0p.x - vfirst.p.x, x0p.y - vfirst.p.y, x0p.z - vfirst.p.z);
			const float r01sq = dot(d01, d01);
			float G = 0.f;
			if (r01sq >= 1e-6f) {
				const float dw0Dx1 = fabs(dot(d01, vfirst.gn)) / (sqrt(r01sq) * r01sq);
				G = dw0Dx1 * fabs(Mnee44_Det(dxDy));
			}
			if (!(G > 0.f) || !isfinite(G)) {
				Mnee_ExitTransition(taskState, sampleResult);
				return;
			}
			mnee->geometricTerm = G;

			// Post-solve pass: one volume reset, then per-vertex updates
			// accumulate (the re-projection rays are too short to cross a
			// boundary; matches the CPU volLast chain in volume-free scenes
			// exactly).
			*dlVolInfo = pathInfo->volume;
			mnee->chainSpecR = 1.f; mnee->chainSpecG = 1.f; mnee->chainSpecB = 1.f;
			mnee->plainHalfVector = 1;
			const float eps0 = MneeChain_TrialEps(x0p, vfirst.p);
			MneeChain_WriteReprojectRay(vfirst.p, vfirst.gn, .5f * eps0, ray, ray->time);
			mnee->chainIdx = 0;
			mnee->phase = MNEE_PHASE_MS_POST;
			return;
		}

		if (!MneeChain_ThomasSolveVec(mnee, nn)) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		MneeChain_WriteTrial0(mnee, x0p, ray, dlVolInfo, &pathInfo->volume);
		return;
	}

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_TRIAL: line search trial of vertex chainIdx.
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_TRIAL) {
		const int i = mnee->chainIdx;
		bool accepted = true;
		if (rayHit->meshIndex == NULL_INDEX)
			accepted = false;
		else {
			MneeVtx trialV;
			Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf,
					mnee->chainVtx[i].eta, &trialV);
			if (mats[taskDirectLight->mneeBsdf.materialIndex].type != mnee->chainMatType[i])
				accepted = false;
			else {
				const float3 pPrev = (i == 0) ? x0p : MneeChain_TrialPos(mnee, i - 1);
				const float3 pNext = (i == nn - 1) ? lightPos : MneeChain_TrialPos(mnee, i + 1);
				const bool woDirT = (mnee->lightIsDir != 0) && (i == nn - 1);
				float2 CT;
				if (!MneeChain_ResidualAt(pPrev, pNext, woDirT, &trialV, trialV.eta, &CT))
					accepted = false;
				else
					MneeVec2T_FromFloat2(&mnee->chainTrialRes[i], CT);
			}
		}
		if (!accepted)
			mnee->chainProjected = 0;

		if (accepted && (i + 1 < nn)) {
			const float3 pProp = MneeChain_TrialPos(mnee, i + 1);
			MneeVtx vNext;
			MneeChain_LoadVtx(mnee, i + 1, &vNext);
			MneeChain_WriteReprojectRay(pProp, vNext.gn,
					.5f * MneeChain_TrialEps(x0p, vNext.p), ray, ray->time);
			*dlVolInfo = pathInfo->volume;
			mnee->chainIdx = i + 1;
			return;
		}

		// Trial complete (or a vertex failed to project): line search
		// decision with the CPU beta/iteration accounting.
		float trialMax = 0.f;
		if (mnee->chainProjected) {
			for (int t = 0; t < nn; ++t)
				trialMax = fmax(trialMax, length(MneeVec2T_ToFloat2(&mnee->chainTrialRes[t])));
		}
		if (mnee->chainProjected && (trialMax < mnee->resNorm)) {
			// Beta doubling only after the commit: TrialPos is recomputed
			// during MS_COMMIT and must still see the beta that produced
			// the validated positions (CPU commits trial[] first, then
			// raises beta).
			const float3 pProp0 = MneeChain_TrialPos(mnee, 0);
			MneeVtx v0;
			MneeChain_LoadVtx(mnee, 0, &v0);
			MneeChain_WriteReprojectRay(pProp0, v0.gn,
					.5f * MneeChain_TrialEps(x0p, v0.p), ray, ray->time);
			*dlVolInfo = pathInfo->volume;
			mnee->chainIdx = 0;
			mnee->phase = MNEE_PHASE_MS_COMMIT;
			return;
		}
		mnee->beta *= .5f;
		mnee->iteration++;
		if (mnee->beta <= 1e-2f) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		MneeChain_WriteTrial0(mnee, x0p, ray, dlVolInfo, &pathInfo->volume);
		return;
	}

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_COMMIT: rebuild the accepted trial vertices (the CPU
	// commits from memory; the kernel re-traces the same deterministic
	// re-projections).
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_COMMIT) {
		const int i = mnee->chainIdx;
		if (rayHit->meshIndex == NULL_INDEX) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		MneeVtx cv;
		Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf,
				mnee->chainVtx[i].eta, &cv);
		MneeChain_StoreVtx(mnee, i, &cv);

		if (i + 1 < nn) {
			const float3 pProp = MneeChain_TrialPos(mnee, i + 1);
			MneeVtx vNext;
			MneeChain_LoadVtx(mnee, i + 1, &vNext);
			MneeChain_WriteReprojectRay(pProp, vNext.gn,
					.5f * MneeChain_TrialEps(x0p, vNext.p), ray, ray->time);
			*dlVolInfo = pathInfo->volume;
			mnee->chainIdx = i + 1;
			return;
		}
		mnee->iteration++;
		// Step accepted: raise the line-search beta for the next Newton
		// iteration (CPU ordering: commit trial positions, then double).
		mnee->beta = fmin(1.f, 2.f * mnee->beta);
		if (mnee->iteration >= taskConfig->pathTracer.mnee.maxIterations) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, &pathInfo->volume))
			Mnee_ExitTransition(taskState, sampleResult);
		return;
	}

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_POST: post-solve mode check, specular factor and volume
	// update per vertex (CPU post-solve validity port).
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_POST) {
		const int k = mnee->chainIdx;
		if (rayHit->meshIndex == NULL_INDEX) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		if (mats[taskDirectLight->mneeBsdf.materialIndex].type != mnee->chainMatType[k]) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}

		MneeVtx vk;
		MneeChain_LoadVtx(mnee, k, &vk);
		const float3 pPrevK = (k == 0) ? x0p : MneeChain_VtxPos(mnee, k - 1);
		const float3 pNextK = (k == nn - 1) ? lightPos : MneeChain_VtxPos(mnee, k + 1);
		const float3 wik = normalize(pPrevK - vk.p);
		// Directional endpoint: the stored value is the unit light
		// direction, not a position.
		const float3 wok = ((mnee->lightIsDir != 0) && (k == nn - 1)) ?
				pNextK : normalize(pNextK - vk.p);
		const float cosI = dot(vk.gn, wik);
		const float cosO = dot(vk.gn, wok);
		if (vk.eta == 1.f) {
			if (cosI * cosO < 0.f) {
				Mnee_ExitTransition(taskState, sampleResult);
				return;
			}
		} else {
			if (cosI * cosO > 0.f) {
				Mnee_ExitTransition(taskState, sampleResult);
				return;
			}
		}

		BSDFEvent specEvent;
		const float3 spec = Mnee_SpecFactor(
				&mats[taskDirectLight->mneeBsdf.materialIndex],
				&taskDirectLight->mneeBsdf.hitPoint,
				&taskDirectLight->mneeBsdf, wik, &specEvent
				MATERIALS_PARAM);
		if (Spectrum_IsBlack(spec)) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		mnee->chainSpecR *= spec.x;
		mnee->chainSpecG *= spec.y;
		mnee->chainSpecB *= spec.z;
		if (vk.eta != 1.f)
			mnee->plainHalfVector = 0;
		mnee->specEvent = specEvent;
		PathVolumeInfo_Update(dlVolInfo, specEvent,
				&taskDirectLight->mneeBsdf
				MATERIALS_PARAM);

		if (k + 1 < nn) {
			MneeVtx vNext;
			MneeChain_LoadVtx(mnee, k + 1, &vNext);
			const float epsNext = MneeChain_TrialEps(x0p, vNext.p);
			MneeChain_WriteReprojectRay(vNext.p, vNext.gn, .5f * epsNext, ray, ray->time);
			mnee->chainIdx = k + 1;
			return;
		}


		taskDirectLight->mneeBsdfFinal = taskDirectLight->mneeBsdf;
		mnee->specFactorR = mnee->chainSpecR;
		mnee->specFactorG = mnee->chainSpecG;
		mnee->specFactorB = mnee->chainSpecB;
		if (!MneeChain_Seg2Setup(task, taskDirectLight, ray, taskGid,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius, ray
				LIGHTS_PARAM)) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		return;
	}

	Mnee_ExitTransition(taskState, sampleResult);
}

// Solve end: post-solve validity checks, specular factor, volume state for
// the second segment and the x1 -> y Illuminate + shadow ray write.
OPENCL_FORCE_NOT_INLINE void Mnee_SolveEnd(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global EyePathInfo *pathInfo,
		__global MneeState *mnee,
		const float3 x0p,
		const float g, const uint taskGid,
		const float worldCenterX, const float worldCenterY,
		const float worldCenterZ, const float worldRadius,
		__global Ray *ray, __global PathVolumeInfo *dlVolInfo,
		__global SampleResult *sampleResult,
		__global MneeSeedEntry *mneeSeeds
		LIGHTS_PARAM_DECL
		) {
	// Post-solve validity check (Zeltner newton_solver tail): the
	// half-vector formulation can converge to a solution of the wrong
	// specular mode
	MneeVtx v;
	Mnee_LoadVtx(mnee, &v);
	const float3 lightPosOrDir = MAKE_FLOAT3(mnee->lightPosX,
			mnee->lightPosY, mnee->lightPosZ);
	const float3 wi = normalize(x0p - v.p);
	// Directional endpoint: the stored value is already the unit light
	// direction; point-like: wo = normalize(lightPos - x1).
	const float3 wo = (mnee->lightIsDir != 0) ? lightPosOrDir :
			normalize(lightPosOrDir - v.p);
	const float cosX = dot(v.gn, wi);
	const float cosY = dot(v.gn, wo);
	const bool refraction = (cosX * cosY < 0.f);
	if (mnee->mirrorMode) {
		// Mirror: only a same-side reflection is physical (see the etaVertex
		// gate in Mnee_Start). Reject solutions with the opposite side
		// relation - the half-vector formulation can converge to such a mode,
		// and accepting it produced light where none exists.
		if (refraction) {
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
			return;
		}
	} else if (!refraction) {
		// Glass: only refraction solutions are supported (Zeltner SS handles
		// dielectric transmission; external dielectric reflection is out of
		// scope)
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
		return;
	}

	// Specular factor at the solved vertex, with LuxCore's own material code
	BSDFEvent specEvent;
	const float3 specFactor = Mnee_SpecFactor(
			&mats[taskDirectLight->mneeBsdfFinal.materialIndex],
			&taskDirectLight->mneeBsdfFinal.hitPoint,
			&taskDirectLight->mneeBsdfFinal, wi, &specEvent
			MATERIALS_PARAM);
	if (Spectrum_IsBlack(specFactor) || !(g > 0.f) || isnan(g) || isinf(g)) {
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
		return;
	}

	mnee->specFactorR = specFactor.x;
	mnee->specFactorG = specFactor.y;
	mnee->specFactorB = specFactor.z;
	mnee->geometricTerm = g;
	mnee->specEvent = specEvent;
	mnee->plainHalfVector = (v.eta == 1.f);

	// Volume state after the specular event at x1 (CPU volSeg2)
	*dlVolInfo = pathInfo->volume;
	PathVolumeInfo_Update(dlVolInfo, specEvent,
			&taskDirectLight->mneeBsdfFinal
			MATERIALS_PARAM);

	// Second segment x1 -> y: Illuminate at the specular vertex. The u's
	// are hashed, decorrelated draws (the pass-through seed state varies
	// per sample; point/spot Illuminate only consumes the pass-through
	// event).
	const uint hashBase = SobolSequence_BlueNoiseHash(
			SobolSequence_BlueNoiseHash(taskDirectLight->seedPassThroughEvent.s1) ^
			(taskGid * 0x9E3779B9u + 0x85EBCA6Bu));
	const float uSeg1 = SobolSequence_BlueNoiseHash(hashBase ^ 0x68E31DE4u) * (1.f / 4294967296.f);
	const float uSeg2 = SobolSequence_BlueNoiseHash(hashBase ^ 0xB5AD78CEu) * (1.f / 4294967296.f);
	const float uSeg3 = SobolSequence_BlueNoiseHash(hashBase ^ 0xD6E8FEB8u) * (1.f / 4294967296.f);
	mnee->seg2PassThrough = SobolSequence_BlueNoiseHash(hashBase ^ 0x2EB0D5B5u) * (1.f / 4294967296.f);

	float directPdfW2;
	const float3 lightRadiance2 = Light_Illuminate(
			&lights[taskDirectLight->illumInfo.lightIndex],
			&taskDirectLight->mneeBsdfFinal,
			ray->time, uSeg1, uSeg2, uSeg3,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius,
			&task->tmpHitPoint, ray, &directPdfW2, NULL, NULL
			LIGHTS_PARAM);

	if (Spectrum_IsBlack(lightRadiance2) || !isfinite(directPdfW2)) {
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
		return;
	}

	if (mnee->lightIsDir != 0) {
		// Illuminate() resampled a direction inside the emitter lobe; the
		// manifold endpoint is the fixed direction the solve ran for. Rebuild
		// the second-segment ray toward wo and extend it to the scene
		// bounding sphere (a miss = the directional light is reached). The
		// emitted radiance is constant across the delta lobe, so
		// lightRadiance2 stays valid; only the ray direction and length must
		// reflect the solved endpoint.
		const float3 wo2 = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY,
				mnee->lightPosZ);
		const float3 o2 = BSDF_GetRayOrigin(&taskDirectLight->mneeBsdfFinal, wo2);
		const float3 toCenter = MAKE_FLOAT3(worldCenterX, worldCenterY,
				worldCenterZ) - o2;
		const float approach = dot(toCenter, wo2);
		const float dist = approach + sqrt(fmax(0.f,
				worldRadius * worldRadius - dot(toCenter, toCenter) +
				approach * approach));
		Ray_Init4(ray, o2, wo2, 0.f, dist, ray->time);
	}

	mnee->lightRadiance2R = lightRadiance2.x;
	mnee->lightRadiance2G = lightRadiance2.y;
	mnee->lightRadiance2B = lightRadiance2.z;
	mnee->directPdfW2 = directPdfW2;
	mnee->phase = MNEE_PHASE_SEG2_TRACE;
}

// The MK_MNEE_NEXT_VERTEX sub-state machine body: consumes the trace result
// of the current MNEE ray and either writes the next trace ray or exits.
OPENCL_FORCE_NOT_INLINE void Mnee_ProcessState(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global EyePathInfo *pathInfo,
		__global Ray *ray, __global RayHit *rayHit,
		__global PathVolumeInfo *dlVolInfo,
		__global SampleResult *sampleResult, const uint taskGid,
		const float worldCenterX, const float worldCenterY,
		const float worldCenterZ, const float worldRadius,
		__global MneeSeedEntry *mneeSeeds
		LIGHTS_PARAM_DECL
		) {
	__global MneeState *mnee = &taskDirectLight->mnee;

	// The trace ray was just written by this same render iteration (by
	// Mnee_Start()): rayHits[] still holds the previous trace result.
	if (mnee->needsTrace) {
		mnee->needsTrace = false;
		return;
	}

	// Multi-specular chain phases (MNEEMultiDirectSampling port, one trace
	// per launch like the single vertex machine below).
	if (mnee->phase >= MNEE_PHASE_MS_DISCOVER) {
		MneeChain_ProcessState(taskConfig, task, taskDirectLight, taskState, pathInfo,
				ray, rayHit, dlVolInfo, sampleResult, taskGid,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius
				LIGHTS_PARAM);
		return;
	}

	const float3 lightPos = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY, mnee->lightPosZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);
	MneeVtx v;
	Mnee_LoadVtx(mnee, &v);

	//--------------------------------------------------------------------------
	// MNEE_PHASE_STEP: no trace to consume. Newton loop top (glass start).
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_STEP) {
		float g;
		const int stepResult = Mnee_StepAndWriteProposal(taskConfig, mnee,
				x0p, lightPos, &v, &taskState->bsdf, ray, &g);
		if (stepResult == 0) {
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
			return;
		}
		if (stepResult == 1) {
			mnee->phase = MNEE_PHASE_PROP_TRACE;
			// Fresh path volume for the new trace (CPU: propVol = volInfo)
			*dlVolInfo = pathInfo->volume;
			return;
		}
		// stepResult == 2: converged, fall through to the solve end
		Mnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, pathInfo,
				mnee, x0p, g, taskGid,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				ray, dlVolInfo, sampleResult, mneeSeeds
				LIGHTS_PARAM);
		return;
	}

	//--------------------------------------------------------------------------
	// Consume the trace result of the current MNEE ray (seed / proposal /
	// x1 -> y shadow). Same volume walk as the other Scene_Intersect()
	// call sites: on continueToTrace the state stays and the continuation
	// ray is traced by the next RT dispatch.
	//--------------------------------------------------------------------------
	int throughShadowTransparency = false;
	float3 connectionThroughput;
	const bool seg2Phase = (mnee->phase == MNEE_PHASE_SEG2_TRACE);
	const bool continueToTrace = Scene_Intersect(taskConfig,
			EYE_RAY | (seg2Phase ? SHADOW_RAY : INDIRECT_RAY),
			// Manifold-walk rays have no path depth/event context (same
			// defaults as the CPU MNEE code)
			NULL, NONE,
			&throughShadowTransparency, dlVolInfo, &task->tmpHitPoint,
			seg2Phase ? mnee->seg2PassThrough : .5f,
			ray, rayHit, &taskDirectLight->mneeBsdf, &connectionThroughput,
			WHITE, sampleResult, seg2Phase
			MATERIALS_PARAM);
	if (continueToTrace)
		return;

	if (mnee->phase == MNEE_PHASE_SEED_TRACE) {
		// Accept the mirrored-light seed only if it lands on the same
		// mesh as the shadow-ray occluder (CPU check)
		if (rayHit->meshIndex == mnee->shadowMeshIndex) {
			MneeVtx vSeed;
			Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf, v.eta, &vSeed);
			Mnee_StoreVtx(mnee, &vSeed);
		}
		Mnee_ApplySeedShift(mnee, x0p);
		Mnee_LoadVtx(mnee, &v);

		float g;
		const int stepResult = Mnee_StepAndWriteProposal(taskConfig, mnee,
				x0p, lightPos, &v, &taskState->bsdf, ray, &g);
		if (stepResult == 0) {
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
			return;
		}
		if (stepResult == 1) {
			mnee->phase = MNEE_PHASE_PROP_TRACE;
			*dlVolInfo = pathInfo->volume;
			return;
		}
		// stepResult == 2: converged
		Mnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, pathInfo,
				mnee, x0p, g, taskGid,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				ray, dlVolInfo, sampleResult, mneeSeeds
				LIGHTS_PARAM);
		return;
	}

	if (mnee->phase == MNEE_PHASE_PROP_TRACE) {
		if (rayHit->meshIndex == NULL_INDEX) {
			// The proposal ray missed everything: the CPU line search
			// breaks out of the Newton loop here (solve failed).
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
			return;
		}

		bool stepRejected;
		bool converged = false;
		float gConverged = 0.f;
		if (rayHit->meshIndex != mnee->shadowMeshIndex) {
			stepRejected = true;
		} else {
			// Evaluate the proposal residual
			MneeVtx vProp;
			Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf, v.eta, &vProp);
			float2 CProp;
			if (!Mnee_Residual(x0p, lightPos, mnee->lightIsDir != 0, &vProp, &CProp)) {
				stepRejected = true;
			} else {
				const float resPropNorm = length(CProp);
				if (resPropNorm < mnee->resNorm) {
					// Accept the step (Zeltner beta backtracking with a
					// residual decrease check)
					mnee->beta = fmin(1.f, 2.f * mnee->beta);
					mnee->iteration++;
					Mnee_StoreVtx(mnee, &vProp);
					taskDirectLight->mneeBsdfFinal = taskDirectLight->mneeBsdf;
					v = vProp;
					stepRejected = false;

					// CPU: after the accept the loop-top bound check runs
					// before the residual convergence check
					if (mnee->iteration >= taskConfig->pathTracer.mnee.maxIterations) {
						Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
						return;
					}
					if (resPropNorm < 3e-4f) {
						// Converged: geometric term at the converged vertex
						converged = true;
						gConverged = Mnee_GeometricTerm(x0p, lightPos,
								mnee->lightIsDir != 0, &vProp, NULL);
					}
				} else
					stepRejected = true;
			}
		}

		if (converged) {
			Mnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, pathInfo,
					mnee, x0p, gConverged, taskGid,
					worldCenterX, worldCenterY, worldCenterZ, worldRadius,
					ray, dlVolInfo, sampleResult, mneeSeeds
					LIGHTS_PARAM);
			return;
		}

		if (stepRejected) {
			// Line search reject: halve beta, count the iteration and
			// retry from the same vertex (CPU: beta *= .5f; ++iteration;
			// continue)
			mnee->beta *= .5f;
			mnee->iteration++;
		}

		// Next Newton step from the current vertex (the CPU loop-top
		// iteration bound and residual checks are inside
		// Mnee_StepAndWriteProposal)
		float g;
		const int stepResult = Mnee_StepAndWriteProposal(taskConfig, mnee,
				x0p, lightPos, &v, &taskState->bsdf, ray, &g);
		if (stepResult == 0) {
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
			return;
		}
		if (stepResult == 1) {
			mnee->phase = MNEE_PHASE_PROP_TRACE;
			*dlVolInfo = pathInfo->volume;
			return;
		}
		// stepResult == 2: converged
		Mnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, pathInfo,
				mnee, x0p, g, taskGid,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				ray, dlVolInfo, sampleResult, mneeSeeds
				LIGHTS_PARAM);
		return;
	}

	// MNEE_PHASE_SEG2_TRACE: the chain is valid only if y is directly
	// visible from x1
	if (rayHit->meshIndex != NULL_INDEX) {
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult, mneeSeeds, worldRadius
			LIGHTS_PARAM);
		return;
	}

	// Contribution assembly (CPU lines 713-752). A solved multi-specular
	// chain aims the receiver at its FIRST vertex, the single vertex
	// solver at its only one.
	const float3 vtxP = (mnee->chainN > 1) ?
			MAKE_FLOAT3(mnee->chainVtx[0].px, mnee->chainVtx[0].py, mnee->chainVtx[0].pz) :
			MAKE_FLOAT3(mnee->vtx.px, mnee->vtx.py, mnee->vtx.pz);
	const float3 receiverDir = normalize(vtxP - x0p);
	BSDFEvent receiverEvent;
	float receiverPdfW;
	const float3 bsdfEval0 = BSDF_Evaluate(&taskState->bsdf, receiverDir,
			&receiverEvent, &receiverPdfW
			MATERIALS_PARAM);

	if (!Spectrum_IsBlack(bsdfEval0)) {
		// MNEE light weight. The r12^2 (directPdfW2) factor belongs to the
		// light weight only when the solved constraint is the plain
		// half-vector (vertex eta == 1, the same-side reflection case). For
		// any eta != 1 (dielectric transmission, opposite-side mirror law)
		// the analytic geometric term already carries the full conversion,
		// and multiplying directPdfW2 as well inflates the estimate by a
		// geometry-dependent factor (up to ~6x in the glass suite; see
		// dev-tools/mnee_glass_pixel_compare.py). risScale keeps ReSTIR RIS
		// unbiased.
		const float3 specFactor = MAKE_FLOAT3(mnee->specFactorR,
				mnee->specFactorG, mnee->specFactorB);
		const float3 lightRadiance2 = MAKE_FLOAT3(mnee->lightRadiance2R,
				mnee->lightRadiance2G, mnee->lightRadiance2B);
		// Directional endpoint: the geometric term is already the
		// direction-space Jacobian (point-light limit lightPos = x0 + wo*R,
		// R -> infinity cancels the perpendicular-frame Jacobian and the
		// r12^2 factor exactly). The only remaining factor is the emitter's
		// direction pdf: 1 for a delta direction (sharpdistant), the uniform
		// cone pdf for a distant light.
		const float weightScale = (mnee->lightIsDir != 0) ?
				(taskDirectLight->illumInfo.risScale /
					(mnee->directPdfW2 * taskDirectLight->illumInfo.pickPdf)) :
				((mnee->plainHalfVector ? mnee->directPdfW2 : 1.f) *
					taskDirectLight->illumInfo.risScale /
					taskDirectLight->illumInfo.pickPdf);
		float3 incomingRadiance = bsdfEval0 * specFactor *
				(mnee->geometricTerm * weightScale) * lightRadiance2 *
				connectionThroughput;
#if defined(SLG_SPECTRAL)
		if (mnee->dispersive)
			// The manifold constraint holds at the hero wavelength only:
			// carry the wavelength-selection weight, drop the dead bins.
			incomingRadiance = Spectral_KeepHeroBins(incomingRadiance,
					sampleResult->spectralHeroAlive);
#endif

		SampleResult_AddDirectLight(&taskConfig->film, sampleResult,
				taskDirectLight->illumInfo.lightID,
				(BSDFEvent)mnee->specEvent,
				VLOAD3F(taskState->throughput.c), incomingRadiance, 1.f);

		// E4: publish the converged vertex as a warm-start seed only now
		// that the full connect validated (seg2 visibility + receiver
		// BSDF): a vertex that solves but fails downstream lands its reuse
		// in the same dead basin, and on multi-root casters those polluted
		// seeds systematically out-compete the cold line seed (CPU
		// pathtracer_mnee.cpp parity).
		if (taskConfig->pathTracer.mnee.seedCacheEnable &&
				(mnee->chainN == 0)) {
			const float3 occlP = MAKE_FLOAT3(mnee->occlX, mnee->occlY,
					mnee->occlZ);
			const float cellSize = fmax(worldRadius / MNEE_SEED_CELL_FRAC,
					1e-4f);
			const float3 vp = MAKE_FLOAT3(mnee->vtx.px, mnee->vtx.py,
					mnee->vtx.pz);
			const float3 vn = MAKE_FLOAT3(mnee->vtx.nX, mnee->vtx.nY,
					mnee->vtx.nZ);
			Mnee_SeedCacheStore(mneeSeeds,
					Mnee_SeedKey(taskDirectLight->illumInfo.lightIndex,
						mnee->shadowMeshIndex * 2u + mnee->shadowSide,
						occlP, cellSize),
					vp, vn, taskDirectLight->illumInfo.lightIndex,
					mnee->shadowMeshIndex * 2u + mnee->shadowSide,
					mnee->mirrorMode);
		}
	}

	Mnee_ExitTransition(taskState, sampleResult);
}


//------------------------------------------------------------------------------
// LMNEE: light -> camera manifold connect (design in
// doc/features/gpu_lighttracing.md). Mirror of the eye-side MNEE with the
// endpoint roles swapped: x0 is the light-path vertex whose straight
// camera connect was blocked by a delta occluder, y is the sampled lens
// point. The Newton solver (Mnee_StepAndWriteProposal, Mnee_Residual,
// Mnee_GeometricTerm) is endpoint-agnostic and reused verbatim; only the
// start gates and the solve-end contribution differ. The solve runs on
// the task's visibility-ray slot (lightVisRayBase tail) while the light
// path stalls on lpi->mneeActive.
//------------------------------------------------------------------------------

// Camera endpoint id for the seed cache (keyed like a light index, but
// the camera is a singleton endpoint)
#define LMNEE_CAMERA_SEED_ID 0xFFFFFFFEu

// Start of the light-side MNEE sub-state machine, called from
// MK_LIGHT_VERTEX when the pending connect ray hit a delta occluder.
// Returns 1 when the solve was started (first trace written into
// visRay), 0 otherwise.
OPENCL_FORCE_NOT_INLINE int LMnee_Start(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global const RayHit *visRayHit, __global Ray *visRay,
		__global LightPathInfo *lpi,
		__global MneeSeedEntry *mneeSeeds,
		const float worldRadius
		MATERIALS_PARAM_DECL
		) {
	if (!taskConfig->pathTracer.mnee.enabled)
		return 0;

	__global MneeState *mnee = &taskDirectLight->mnee;
	__global const BSDF *occlBsdf = &task->tmpBsdf;
	__global const Material *occlMat = &mats[occlBsdf->materialIndex];

	// Same occluder gates as Mnee_Start: delta specular MIRROR or GLASS
	if (occlBsdf->isVolume || !occlMat->isDelta || !(occlMat->eventTypes & SPECULAR))
		return 0;
	if ((occlMat->type != MIRROR) && (occlMat->type != GLASS))
		return 0;

	const float3 lensPoint = MAKE_FLOAT3(lpi->lensPointX, lpi->lensPointY,
			lpi->lensPointZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	// Generalized half-vector IOR ratio of the occluder (same convention
	// as Mnee_Start)
	float etaVertex;
	bool dispersive = false;
	if (occlMat->type == MIRROR) {
		const float3 gn1s = VLOAD3F(&occlBsdf->hitPoint.geometryN.x);
		const float3 x1p = VLOAD3F(&occlBsdf->hitPoint.p.x);
		const float3 toX0 = x0p - x1p;
		const float3 toY = lensPoint - x1p;
		etaVertex = (dot(toX0, gn1s) * dot(toY, gn1s) > 0.f) ? 1.f : -1.f;
		if (etaVertex != 1.f)
			return 0;
	} else {
		const float nc = ExtractExteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
		const float nt = ExtractInteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.interiorIorTexIndex TEXTURES_PARAM);
		if ((nt <= 0.f) || (nc <= 0.f))
			return 0;
		const float cauchyB = (occlMat->glass.cauchyBTex != NULL_INDEX) ?
				Texture_GetFloatValue(occlMat->glass.cauchyBTex,
					&occlBsdf->hitPoint TEXTURES_PARAM) : 0.f;
		if (cauchyB > 0.f) {
#if defined(SLG_SPECTRAL)
			// Hero-wavelength solve (see Mnee_Start); the contribution
			// collapses at assembly via mnee->dispersive.
			dispersive = true;
			etaVertex = Spectral_DispersiveIOR(nt, cauchyB,
					&occlBsdf->hitPoint) / nc;
#else
			return 0;
#endif
		} else
			etaVertex = nt / nc;
	}

	mnee->mirrorMode = (occlMat->type == MIRROR);
	mnee->dispersive = dispersive ? 1 : 0;
	mnee->chainN = 0;
	// The LMNEE endpoint is the camera lens point: always a finite position,
	// never directional.
	mnee->lightIsDir = 0;
	mnee->lightPosX = lensPoint.x;
	mnee->lightPosY = lensPoint.y;
	mnee->lightPosZ = lensPoint.z;
	mnee->shadowMeshIndex = visRayHit->meshIndex;
	mnee->shadowSide = (dot(normalize(VLOAD3F(&visRay->d.x)),
			VLOAD3F(&occlBsdf->hitPoint.geometryN.x)) > 0.f) ? 1u : 0u;
	const float3 occlP = VLOAD3F(&occlBsdf->hitPoint.p.x);
	mnee->occlX = occlP.x;
	mnee->occlY = occlP.y;
	mnee->occlZ = occlP.z;
	mnee->beta = 1.f;
	mnee->iteration = 0;
	mnee->needsTrace = false;
	mnee->seedCacheTried = 0;

	MneeVtx v;
	Mnee_InitVtxFromBsdf(occlBsdf, etaVertex, &v);
	Mnee_StoreVtx(mnee, &v);
	taskDirectLight->mneeBsdfFinal = task->tmpBsdf;

	// Mirror (eta == 1) only: the cold seed costs a mirrored-lens trace,
	// so the cache gets first refusal (camera endpoint id). For glass the
	// free line seed keeps the reference basin; the cache is consulted
	// only as a failure rescue before the chain fallback (eye-side
	// parity, see Mnee_Start).
	if ((etaVertex == 1.f) &&
			taskConfig->pathTracer.mnee.seedCacheEnable && mneeSeeds) {
		const float cellSize = fmax(worldRadius / MNEE_SEED_CELL_FRAC, 1e-4f);
		const uint seedMesh = visRayHit->meshIndex * 2u + mnee->shadowSide;
		const uint key = Mnee_SeedKey(LMNEE_CAMERA_SEED_ID,
				seedMesh, occlP, cellSize);
		if (Mnee_SeedCacheLookup(mneeSeeds, key, LMNEE_CAMERA_SEED_ID,
				seedMesh, mnee->mirrorMode, etaVertex, mnee)) {
			mnee->seedCacheTried = 1;
			Mnee_ApplySeedShift(mnee, x0p);
			mnee->phase = MNEE_PHASE_STEP;
			return 1;
		}
	}

	if (mnee->mirrorMode && (etaVertex == 1.f)) {
		// Mirror: seed from the lens point mirrored across the tangent
		// plane at the occluder hit (same trick as the eye side)
		const float3 gn1 = VLOAD3F(&occlBsdf->hitPoint.geometryN.x);
		const float3 x1Line = VLOAD3F(&occlBsdf->hitPoint.p.x);
		const float3 x1ToLens = lensPoint - x1Line;
		const float proj = 2.f * dot(x1ToLens, gn1);
		const float3 mirroredLens = lensPoint - proj * gn1;
		const float3 dSeed = normalize(mirroredLens - x0p);
		Ray_Init2(visRay, BSDF_GetRayOrigin(&taskState->bsdf, dSeed), dSeed,
				visRay->time);

		mnee->phase = MNEE_PHASE_SEED_TRACE;
	} else {
		Mnee_ApplySeedShift(mnee, x0p);
		mnee->phase = MNEE_PHASE_STEP;
	}

	return 1;
}

// Solve end: mode check, specular factor at the solved vertex, then the
// camera-side endpoint weight and the x1 -> lens visibility ray. The
// pending splat fields carry the pre-visibility radiance for the normal
// Stage A resolution (which multiplies connectionThroughput in).
// Light-path splats project in *film* (camera) coordinates. Under tile
// rendering the film buffer is tile-local while the projection still
// needs the whole camera film: clip to the tile's film-space rect
// (widened by the splat halo so footprint tails are not lost at tile
// edges), then shift back to tile-local coordinates for
// Film_SplatLight. Splats landing outside the halo are dropped; each
// tile keeps an unbiased estimator because its pixels collect the
// light paths generated while it is resident (the same population
// every tile pass draws from).
OPENCL_FORCE_INLINE bool LightPath_ProjectToFilm(
		__global const Camera* restrict camera, __global Ray *visRay,
		float *filmX, float *filmY,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		, __global void *samplerSharedDataBuff
#endif
		) {
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
	__global const TilePathSamplerSharedData *ssd =
			(__global const TilePathSamplerSharedData *)samplerSharedDataBuff;
	const uint tileX = ssd->tileStartX;
	const uint tileY = ssd->tileStartY;
	// Accept centers up to the splat halo past the tile rect: their
	// filter footprint still covers the tile's edge pixels. The rect is
	// clamped to the camera film - Film_SplatLight clips per-pixel.
	const int rx0 = max(0, (int)(tileX + filmSubRegion0) - (int)ssd->splatMarginX);
	const int rx1 = min((int)ssd->cameraFilmWidth - 1,
			(int)(tileX + filmSubRegion1) + (int)ssd->splatMarginX);
	const int ry0 = max(0, (int)(tileY + filmSubRegion2) - (int)ssd->splatMarginY);
	const int ry1 = min((int)ssd->cameraFilmHeight - 1,
			(int)(tileY + filmSubRegion3) + (int)ssd->splatMarginY);
	if (!Camera_GetSamplePosition(camera, visRay, filmX, filmY,
			ssd->cameraFilmWidth, ssd->cameraFilmHeight,
			(uint)rx0, (uint)rx1, (uint)ry0, (uint)ry1))
		return false;
	*filmX -= tileX;
	*filmY -= tileY;
	return true;
#else
	return Camera_GetSamplePosition(camera, visRay, filmX, filmY,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3);
#endif
}

OPENCL_FORCE_NOT_INLINE void LMnee_SolveEnd(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global LightPathInfo *lpi,
		__global MneeState *mnee,
		const float3 x0p, const float g,
		__global Ray *visRay,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3,
		__global MneeSeedEntry *mneeSeeds,
		const float worldRadius
		, __global const Camera* restrict camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		, __global void *samplerSharedDataBuff
#endif
		MATERIALS_PARAM_DECL
		) {
	// Post-solve validity check (same as the eye side): the half-vector
	// formulation can converge to the wrong specular mode
	MneeVtx v;
	Mnee_LoadVtx(mnee, &v);
	const float3 wi = normalize(x0p - v.p);
	const float3 lensPoint = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY,
			mnee->lightPosZ);
	const float3 wo = normalize(lensPoint - v.p);
	const float cosX = dot(v.gn, wi);
	const float cosY = dot(v.gn, wo);
	const bool refraction = (cosX * cosY < 0.f);
	if (mnee->mirrorMode ? refraction : !refraction) {
		lpi->mneeActive = false;
		return;
	}

	BSDFEvent specEvent;
	const float3 specFactor = Mnee_SpecFactor(
			&mats[taskDirectLight->mneeBsdfFinal.materialIndex],
			&taskDirectLight->mneeBsdfFinal.hitPoint,
			&taskDirectLight->mneeBsdfFinal, wi, &specEvent
			MATERIALS_PARAM);
	if (Spectrum_IsBlack(specFactor) || !(g > 0.f) || isnan(g) || isinf(g)) {
		lpi->mneeActive = false;
		return;
	}

	// Publish the converged vertex as a warm-start seed
	// Camera endpoint: project the arriving segment direction to the
	// film and evaluate the camera weight (the light-side analog of
	// Light_Illuminate + directPdfW2: fluxToRadianceFactor is the
	// importance arriving per unit area at the vertex, d2 is the
	// squared endpoint distance)
	const float3 toVtx = v.p - lensPoint;
	const float dSeg2 = length(toVtx);
	if (dSeg2 < 1e-3f) {
		lpi->mneeActive = false;
		return;
	}
	const float time = visRay->time;
	float filmX, filmY;
	if (camera->type == ORTHOGRAPHIC) {
		const float3 orthoDir = normalize(Transform_ApplyVector(
				&camera->base.cameraToWorld, MAKE_FLOAT3(0.f, 0.f, 1.f)));
		Ray_Init3(visRay, v.p, orthoDir, dSeg2, time);
	} else
		Ray_Init3(visRay, lensPoint, toVtx / dSeg2, dSeg2, time);
	if (!LightPath_ProjectToFilm(camera, visRay, &filmX, &filmY,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
			, samplerSharedDataBuff
#endif
			)) {
		lpi->mneeActive = false;
		return;
	}

	float pdfW, fluxToRadianceFactor;
	if (!Camera_GetPDF(camera, visRay, dSeg2, &pdfW, &fluxToRadianceFactor) ||
			(fluxToRadianceFactor <= 0.f)) {
		lpi->mneeActive = false;
		return;
	}

	// Receiver BSDF at x0 toward the solved vertex
	BSDFEvent receiverEvent;
	float receiverPdfW;
	const float3 bsdfEval0 = BSDF_Evaluate(&taskState->bsdf,
			normalize(v.p - x0p), &receiverEvent, &receiverPdfW
			MATERIALS_PARAM);
	if (Spectrum_IsBlack(bsdfEval0)) {
		lpi->mneeActive = false;
		return;
	}

	// Endpoint weight: cameraPdfW is the emitted importance (the
	// light-side analog of lightRadiance2). The manifold geometricTerm
	// already carries the endpoint segment's 1/d^2 measure, so using
	// fluxToRadianceFactor here would double-count the distance falloff.
	// For a plain (mirror) half-vector the r^2 measure rides explicitly
	// (the pinhole lens is a delta, so that factor is dSeg2^2).
	const bool plainHalfVector = (v.eta == 1.f);
	const float camWeight = pdfW *
			(plainHalfVector ? dSeg2 * dSeg2 : 1.f);

	lpi->pendingSplat.filmX = filmX;
	lpi->pendingSplat.filmY = filmY;
	float3 radiance = VLOAD3F(taskState->throughput.c) *
			bsdfEval0 * specFactor * (g * camWeight);
#if defined(SLG_SPECTRAL)
	if (mnee->dispersive)
		// The manifold constraint holds at the hero wavelength only:
		// carry the wavelength-selection weight and drop the dead bins
		// (same as the path-level Spectral_CollapseToHero).
		radiance = Spectral_KeepHeroBins(radiance,
				taskState->bsdf.hitPoint.spectralHeroAlive);
#endif
	lpi->pendingSplat.radianceR = radiance.x;
	lpi->pendingSplat.radianceG = radiance.y;
	lpi->pendingSplat.radianceB = radiance.z;
	lpi->pendingSplat.lightGroupID = lpi->lightGroupID;
	lpi->pendingSplat.isCaustic = true;
	// fromMnee == 1 marks a single-vertex-solved segment: a re-block
	// means a multi-interface occluder, so the chain takes over
	lpi->pendingSplat.fromMnee = 1;
	// Manifold-guided emission: remember the solved receiver
	lpi->pendingSplat.recvPX = x0p.x;
	lpi->pendingSplat.recvPY = x0p.y;
	lpi->pendingSplat.recvPZ = x0p.z;
	// The splat's visible surface is the x0 receiver (taskState->bsdf)
	lpi->pendingSplat.cryptoObjectID = BSDF_GetCryptoObjectID(&taskState->bsdf);
	lpi->pendingSplat.cryptoMaterialID = BSDF_GetCryptoMaterialID(&taskState->bsdf
			MATERIALS_PARAM);

	// Volume state after the specular event at the vertex (the seg2
	// march runs through it), same convention as the eye side
	lpi->connectVolInfo = lpi->volume;
	PathVolumeInfo_Update(&lpi->connectVolInfo, specEvent,
			&taskDirectLight->mneeBsdfFinal
			MATERIALS_PARAM);
	lpi->connectDepth = lpi->depth;
	lpi->connectThroughShadow = false;

	// Queue the x1 -> lens shadow segment into the visibility slot; the
	// normal Stage A march resolves it next iteration
	const float3 segDir = -toVtx / dSeg2;
	const float3 origin = BSDF_GetRayOrigin(&taskDirectLight->mneeBsdfFinal,
			segDir);
	Ray_Init4(visRay, origin, segDir, 0.f, dSeg2 * (1.f - 1e-4f), time);
	lpi->pendingSplat.valid = true;

	// Publish the converged vertex as a warm-start seed only after the
	// full connect validated (eye-side parity: solved-but-unusable roots
	// pollute the cache and drain the caustic on multi-root casters).
	if (taskConfig->pathTracer.mnee.seedCacheEnable && mneeSeeds) {
		const float3 occlP = MAKE_FLOAT3(mnee->occlX, mnee->occlY, mnee->occlZ);
		const float cellSize = fmax(worldRadius / MNEE_SEED_CELL_FRAC, 1e-4f);
		Mnee_SeedCacheStore(mneeSeeds,
				Mnee_SeedKey(LMNEE_CAMERA_SEED_ID,
					mnee->shadowMeshIndex * 2u + mnee->shadowSide,
					occlP, cellSize),
				v.p, v.n, LMNEE_CAMERA_SEED_ID,
				mnee->shadowMeshIndex * 2u + mnee->shadowSide,
				mnee->mirrorMode);
	}

	lpi->mneeActive = false;
}

//------------------------------------------------------------------------------
// LMNEE multi-specular chain (light-side port of the eye MneeChain_*
// driver): needed when the occluder is a closed dielectric - a glass slab,
// sphere or lens element has TWO refracting faces, so a single-vertex solve
// converges on the near face and its endpoint segment re-blocks on the far
// one. The chain discovers the full x0 -> v0 -> ... -> vN -> lens topology
// along the straight connect ray and Newton-solves all vertices jointly.
//
// The driver mirrors MneeChain_ProcessState with the endpoint roles
// swapped: the endpoint is the sampled lens point stored in mnee->lightPos,
// the source volume is lpi->volume, the accumulating walk volume is
// lpi->connectVolInfo, and every failure drops the connect by clearing
// lpi->mneeActive (the light path then simply continues). All numeric
// helpers (residuals, FD Jacobian, Thomas solve, re-projection rays,
// MneeChain_LightJac) are endpoint-agnostic and reused verbatim.
//------------------------------------------------------------------------------

// Chain solve end: MS_POST already validated every vertex and accumulated
// the specular factor. Project the LAST chain vertex (the far interface
// for a slab) to the film, evaluate the camera endpoint weight with the
// chain geometric term, and queue the xN -> lens segment for the normal
// Stage A visibility resolution. Mirror of the eye-side SEG2 contribution
// assembly (pathtracer_mnee.cpp) mapped onto the camera endpoint.
OPENCL_FORCE_NOT_INLINE void LMneeChain_SolveEnd(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global LightPathInfo *lpi,
		__global MneeState *mnee,
		const float3 x0p,
		__global Ray *visRay,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		, __global const Camera* restrict camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		, __global void *samplerSharedDataBuff
#endif
		MATERIALS_PARAM_DECL
		) {
	const int nn = mnee->chainN;
	const float3 lensPoint = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY,
			mnee->lightPosZ);

	// Receiver BSDF: for a chain the receiver aims at the FIRST vertex
	// (pathtracer_mnee.cpp SEG2 assembly, vtxP = chainVtx[0])
	MneeVtx vFirst;
	MneeChain_LoadVtx(mnee, 0, &vFirst);
	const float3 receiverDir = normalize(vFirst.p - x0p);
	BSDFEvent receiverEvent;
	float receiverPdfW;
	const float3 bsdfEval0 = BSDF_Evaluate(&taskState->bsdf, receiverDir,
			&receiverEvent, &receiverPdfW
			MATERIALS_PARAM);
	if (Spectrum_IsBlack(bsdfEval0)) {
		lpi->mneeActive = false;
		return;
	}

	// Camera endpoint: project the arriving segment (last vertex -> lens)
	// to the film and evaluate the importance arriving per unit area
	const float3 lastP = MneeChain_VtxPos(mnee, nn - 1);
	const float3 toVtx = lastP - lensPoint;
	const float dSeg2 = length(toVtx);
	if (dSeg2 < 1e-3f) {
		lpi->mneeActive = false;
		return;
	}
	const float time = visRay->time;
	float filmX, filmY;
	if (camera->type == ORTHOGRAPHIC) {
		const float3 orthoDir = normalize(Transform_ApplyVector(
				&camera->base.cameraToWorld, MAKE_FLOAT3(0.f, 0.f, 1.f)));
		Ray_Init3(visRay, lastP, orthoDir, dSeg2, time);
	} else
		Ray_Init3(visRay, lensPoint, toVtx / dSeg2, dSeg2, time);
	if (!LightPath_ProjectToFilm(camera, visRay, &filmX, &filmY,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
			, samplerSharedDataBuff
#endif
			)) {
		lpi->mneeActive = false;
		return;
	}
	float pdfW, fluxToRadianceFactor;
	if (!Camera_GetPDF(camera, visRay, dSeg2, &pdfW, &fluxToRadianceFactor) ||
			(fluxToRadianceFactor <= 0.f)) {
		lpi->mneeActive = false;
		return;
	}

	// Endpoint weight: the camera's emitted importance is cameraPdfW (the
	// light-side analog of lightRadiance2 - a pure per-solid-angle
	// emission). The manifold geometricTerm already carries the endpoint
	// segment's 1/d^2 measure, so fluxToRadianceFactor's extra 1/dSeg2^2
	// would double-count it. For a plain (mirror) half-vector the chain
	// instead carries the r^2 measure factor like the eye-side
	// directPdfW2 - the pinhole lens is a delta so that factor is dSeg2^2.
	const float camWeight = pdfW *
			(mnee->plainHalfVector ? dSeg2 * dSeg2 : 1.f);
	const float3 specFactor = MAKE_FLOAT3(mnee->specFactorR,
			mnee->specFactorG, mnee->specFactorB);

	lpi->pendingSplat.filmX = filmX;
	lpi->pendingSplat.filmY = filmY;
	float3 radiance = VLOAD3F(taskState->throughput.c) *
			bsdfEval0 * specFactor * (mnee->geometricTerm * camWeight);
#if defined(SLG_SPECTRAL)
	if (mnee->dispersive)
		// The manifold constraint holds at the hero wavelength only:
		// carry the wavelength-selection weight, drop the dead bins.
		radiance = Spectral_KeepHeroBins(radiance,
				taskState->bsdf.hitPoint.spectralHeroAlive);
#endif
	lpi->pendingSplat.radianceR = radiance.x;
	lpi->pendingSplat.radianceG = radiance.y;
	lpi->pendingSplat.radianceB = radiance.z;
	lpi->pendingSplat.lightGroupID = lpi->lightGroupID;
	lpi->pendingSplat.isCaustic = true;
	// fromMnee == 2 marks a chain-solved segment: a re-block then drops
	// (the occluder needs more interfaces than maxSpecular models) rather
	// than restarting the chain forever
	lpi->pendingSplat.fromMnee = 2;
	// Manifold-guided emission: remember the solved receiver
	lpi->pendingSplat.recvPX = x0p.x;
	lpi->pendingSplat.recvPY = x0p.y;
	lpi->pendingSplat.recvPZ = x0p.z;
	// The splat's visible surface is the x0 receiver (taskState->bsdf)
	lpi->pendingSplat.cryptoObjectID = BSDF_GetCryptoObjectID(&taskState->bsdf);
	lpi->pendingSplat.cryptoMaterialID = BSDF_GetCryptoMaterialID(&taskState->bsdf
			MATERIALS_PARAM);

	lpi->connectDepth = lpi->depth;
	lpi->connectThroughShadow = false;

	// Queue the xN -> lens shadow segment; the far interface is the last
	// chain vertex so the segment travels in air and resolves clean
	const float3 segDir = -toVtx / dSeg2;
	const float3 origin = BSDF_GetRayOrigin(&taskDirectLight->mneeBsdfFinal,
			segDir);
	Ray_Init4(visRay, origin, segDir, 0.f, dSeg2 * (1.f - 1e-4f), time);
	lpi->pendingSplat.valid = true;
	lpi->mneeActive = false;
}

// One launch of the light-side chain sub-state machine (mirror of
// MneeChain_ProcessState; see the LMNEE block comment for the mapping).
OPENCL_FORCE_NOT_INLINE void LMneeChain_ProcessState(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global LightPathInfo *lpi,
		__global Ray *ray, __global RayHit *rayHit,
		__global SampleResult *sampleResult,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3,
		__global const PathVolumeInfo *srcVol,
		__global PathVolumeInfo *dlVolInfo
		, __global const Camera* restrict camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		, __global void *samplerSharedDataBuff
#endif
		MATERIALS_PARAM_DECL
		) {
	__global MneeState *mnee = &taskDirectLight->mnee;
	// The endpoint is the sampled lens point (stored in lightPos)
	const float3 lightPos = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY, mnee->lightPosZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	// Trace consumption shared by DISCOVER/JACPERT/TRIAL/COMMIT/POST
	int throughShadowTransparency = false;
	float3 connectionThroughput;
	const bool continueToTrace = Scene_Intersect(taskConfig,
			LIGHT_RAY | INDIRECT_RAY,
			NULL, NONE,
			&throughShadowTransparency, dlVolInfo, &task->tmpHitPoint,
			.5f, ray, rayHit, &taskDirectLight->mneeBsdf, &connectionThroughput,
			WHITE, sampleResult, false
			MATERIALS_PARAM);
	if (continueToTrace)
		return;

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_DISCOVER: straight-ray chain topology
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_DISCOVER) {
		if (rayHit->meshIndex == NULL_INDEX) {
			if (mnee->chainN >= 2) {
				if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, srcVol))
					lpi->mneeActive = false;
			} else
				lpi->mneeActive = false;
			return;
		}

		__global const Material *hitMat = &mats[taskDirectLight->mneeBsdf.materialIndex];
		float etaVertex = 1.f;
		bool vertexOk = true;
		if (taskDirectLight->mneeBsdf.isVolume || !hitMat->isDelta ||
				!(hitMat->eventTypes & SPECULAR))
			vertexOk = false;
		else if (hitMat->type == MIRROR)
			etaVertex = 1.f;
		else if (hitMat->type == GLASS) {
			const float nc = ExtractExteriorIors(&taskDirectLight->mneeBsdf.hitPoint,
					hitMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
			const float nt = ExtractInteriorIors(&taskDirectLight->mneeBsdf.hitPoint,
					hitMat->glass.interiorIorTexIndex TEXTURES_PARAM);
			const float cauchyB = (hitMat->glass.cauchyBTex != NULL_INDEX) ?
					Texture_GetFloatValue(hitMat->glass.cauchyBTex,
						&taskDirectLight->mneeBsdf.hitPoint TEXTURES_PARAM) : 0.f;
			if ((nt <= 0.f) || (nc <= 0.f))
				vertexOk = false;
			else if (cauchyB > 0.f) {
#if defined(SLG_SPECTRAL)
				// Hero-wavelength eta: the chain solves for the path hero
				// bin only, the contribution collapses at assembly.
				mnee->dispersive = 1;
				etaVertex = Spectral_DispersiveIOR(nt, cauchyB,
						&taskDirectLight->mneeBsdf.hitPoint) / nc;
#else
				vertexOk = false;
#endif
			} else
				etaVertex = nt / nc;
		} else
			vertexOk = false;

		if (!vertexOk) {
			if (mnee->walkInGlass && (mnee->chainSub < 8)) {
				++mnee->chainSub;   // bounded intrusion skips per walk
				// Opaque intrusion inside the dielectric: step past it
				// along the same direction and keep collecting (CPU
				// MneeChainDiscover parity).
				MneeChain_WriteDiscoverRay(&taskDirectLight->mneeBsdf,
						VLOAD3F(&ray->d.x), ray, ray->time);
				return;
			}
			if (mnee->chainN >= 2) {
				if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
						taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, srcVol))
					lpi->mneeActive = false;
			} else
				lpi->mneeActive = false;
			return;
		}

		MneeVtx vv;
		Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf, etaVertex, &vv);
		MneeChain_StoreVtx(mnee, mnee->chainN, &vv);
		mnee->chainMatType[mnee->chainN] = hitMat->type;
		mnee->chainN++;

		if (mnee->chainN >= mnee->chainMaxV) {
			if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, srcVol))
				lpi->mneeActive = false;
			return;
		}

		const float3 dIn = VLOAD3F(&ray->d.x);
		const float3 dir = MneeChain_WalkDir(dIn,
				&taskDirectLight->mneeBsdf,
				etaVertex, hitMat->type == MIRROR);
		if (hitMat->type == GLASS)
			// Entering when the mesh normal faces the incident side; a
			// TIR bounce keeps the walk inside either way.
			mnee->walkInGlass = (dot(dIn,
					VLOAD3F(&taskDirectLight->mneeBsdf.hitPoint.geometryN.x)) < 0.f) ||
					(dot(dIn, dir) < 0.f);
		MneeChain_WriteDiscoverRay(&taskDirectLight->mneeBsdf, dir, ray, ray->time);
		*dlVolInfo = *srcVol;
		return;
	}

	// The phases below only run with a complete chain.
	const int nn = mnee->chainN;

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_JACPERT: FD Jacobian column + Newton loop top
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_JACPERT) {
		const int j = mnee->chainIdx;
		const int k = mnee->chainSub;
		if (rayHit->meshIndex == NULL_INDEX) {
			lpi->mneeActive = false;
			return;
		}

		MneeVtx pertV;
		Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf,
				mnee->chainVtx[j].eta, &pertV);
		const float eps = MneeChain_FdEps(mnee, x0p, j);
		for (int jj = j - 1; jj <= j + 1; ++jj) {
			if ((jj < 0) || (jj >= nn))
				continue;
			const float3 pPrevJJ = (jj == 0) ? x0p :
					((jj - 1 == j) ? pertV.p : MneeChain_VtxPos(mnee, jj - 1));
			const float3 pNextJJ = (jj == nn - 1) ? lightPos :
					((jj + 1 == j) ? pertV.p : MneeChain_VtxPos(mnee, jj + 1));
			MneeVtx vjj;
			if (jj == j)
				vjj = pertV;
			else
				MneeChain_LoadVtx(mnee, jj, &vjj);
			float2 CP;
			// The LMNEE endpoint is the finite lens point: never directional
			// (mnee->lightIsDir is 0 in the light-side context).
			const bool woDirJJ = (mnee->lightIsDir != 0) && (jj == nn - 1);
			if (!MneeChain_ResidualAt(pPrevJJ, pNextJJ, woDirJJ, &vjj, vjj.eta, &CP)) {
				lpi->mneeActive = false;
				return;
			}
			const float dCx = (CP.x - mnee->chainRes[jj].x) / eps;
			const float dCy = (CP.y - mnee->chainRes[jj].y) / eps;
			__global MneeMat2T *blockOut = (jj == j - 1) ? &mnee->chainJacNxt[jj] :
					((jj == j) ? &mnee->chainJacCur[jj] : &mnee->chainJacPrev[jj]);
			float4 b = MneeMat2T_ToFloat4(blockOut);
			if (k == 0) { b.x = dCx; b.z = dCy; } else { b.y = dCx; b.w = dCy; }
			MneeMat2T_FromFloat4(blockOut, b);
		}

		if (k == 0) {
			mnee->chainSub = 1;
			MneeChain_WritePerturbRay(mnee, x0p, j, 1, ray, ray->time);
			*dlVolInfo = *srcVol;
			return;
		}
		if (j + 1 < nn) {
			mnee->chainIdx = j + 1;
			mnee->chainSub = 0;
			MneeChain_WritePerturbRay(mnee, x0p, j + 1, 0, ray, ray->time);
			*dlVolInfo = *srcVol;
			return;
		}

		// Jacobian complete: Newton loop top, solved check first
		if (mnee->resNorm < 1e-5f) {
			float4 dxFirst;
			if (!MneeChain_ThomasSolveMat(mnee, nn, &dxFirst)) {
				lpi->mneeActive = false;
				return;
			}
			MneeVtx vlast;
			MneeChain_LoadVtx(mnee, nn - 1, &vlast);
			const float3 pPrevLast = (nn == 1) ? x0p : MneeChain_VtxPos(mnee, nn - 2);
			const float epsLight = fmax(1e-5f, 1e-4f * length(x0p - vlast.p));
			const float4 lightJac = MneeChain_LightJac(pPrevLast, lightPos,
					mnee->lightIsDir != 0, &vlast, vlast.eta, epsLight);
			const float4 dxDy = Mnee44_Mul(dxFirst, lightJac);
			MneeVtx vfirst;
			MneeChain_LoadVtx(mnee, 0, &vfirst);
			const float3 d01 = MAKE_FLOAT3(x0p.x - vfirst.p.x, x0p.y - vfirst.p.y, x0p.z - vfirst.p.z);
			const float r01sq = dot(d01, d01);
			float G = 0.f;
			if (r01sq >= 1e-6f) {
				const float dw0Dx1 = fabs(dot(d01, vfirst.gn)) / (sqrt(r01sq) * r01sq);
				G = dw0Dx1 * fabs(Mnee44_Det(dxDy));
			}
			if (!(G > 0.f) || !isfinite(G)) {
				lpi->mneeActive = false;
				return;
			}
			mnee->geometricTerm = G;

			*dlVolInfo = *srcVol;
			mnee->chainSpecR = 1.f; mnee->chainSpecG = 1.f; mnee->chainSpecB = 1.f;
			mnee->plainHalfVector = 1;
			const float eps0 = MneeChain_TrialEps(x0p, vfirst.p);
			MneeChain_WriteReprojectRay(vfirst.p, vfirst.gn, .5f * eps0, ray, ray->time);
			mnee->chainIdx = 0;
			mnee->phase = MNEE_PHASE_MS_POST;
			return;
		}

		if (!MneeChain_ThomasSolveVec(mnee, nn)) {
			lpi->mneeActive = false;
			return;
		}
		MneeChain_WriteTrial0(mnee, x0p, ray, dlVolInfo, srcVol);
		return;
	}

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_TRIAL: line search trial of vertex chainIdx
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_TRIAL) {
		const int i = mnee->chainIdx;
		bool accepted = true;
		if (rayHit->meshIndex == NULL_INDEX)
			accepted = false;
		else {
			MneeVtx trialV;
			Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf,
					mnee->chainVtx[i].eta, &trialV);
			if (mats[taskDirectLight->mneeBsdf.materialIndex].type != mnee->chainMatType[i])
				accepted = false;
			else {
				const float3 pPrev = (i == 0) ? x0p : MneeChain_TrialPos(mnee, i - 1);
				const float3 pNext = (i == nn - 1) ? lightPos : MneeChain_TrialPos(mnee, i + 1);
				const bool woDirT = (mnee->lightIsDir != 0) && (i == nn - 1);
				float2 CT;
				if (!MneeChain_ResidualAt(pPrev, pNext, woDirT, &trialV, trialV.eta, &CT))
					accepted = false;
				else
					MneeVec2T_FromFloat2(&mnee->chainTrialRes[i], CT);
			}
		}
		if (!accepted)
			mnee->chainProjected = 0;

		if (accepted && (i + 1 < nn)) {
			const float3 pProp = MneeChain_TrialPos(mnee, i + 1);
			MneeVtx vNext;
			MneeChain_LoadVtx(mnee, i + 1, &vNext);
			MneeChain_WriteReprojectRay(pProp, vNext.gn,
					.5f * MneeChain_TrialEps(x0p, vNext.p), ray, ray->time);
			*dlVolInfo = *srcVol;
			mnee->chainIdx = i + 1;
			return;
		}

		float trialMax = 0.f;
		if (mnee->chainProjected) {
			for (int t = 0; t < nn; ++t)
				trialMax = fmax(trialMax, length(MneeVec2T_ToFloat2(&mnee->chainTrialRes[t])));
		}
		if (mnee->chainProjected && (trialMax < mnee->resNorm)) {
			// Beta doubling only after the commit: TrialPos is recomputed
			// during MS_COMMIT and must still see the beta that produced
			// the validated positions (CPU commits trial[] first, then
			// raises beta).
			const float3 pProp0 = MneeChain_TrialPos(mnee, 0);
			MneeVtx v0;
			MneeChain_LoadVtx(mnee, 0, &v0);
			MneeChain_WriteReprojectRay(pProp0, v0.gn,
					.5f * MneeChain_TrialEps(x0p, v0.p), ray, ray->time);
			*dlVolInfo = *srcVol;
			mnee->chainIdx = 0;
			mnee->phase = MNEE_PHASE_MS_COMMIT;
			return;
		}
		mnee->beta *= .5f;
		mnee->iteration++;
		if (mnee->beta <= 1e-2f) {
			lpi->mneeActive = false;
			return;
		}
		MneeChain_WriteTrial0(mnee, x0p, ray, dlVolInfo, srcVol);
		return;
	}

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_COMMIT: rebuild the accepted trial vertices
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_COMMIT) {
		const int i = mnee->chainIdx;
		if (rayHit->meshIndex == NULL_INDEX) {
			lpi->mneeActive = false;
			return;
		}
		MneeVtx cv;
		Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf,
				mnee->chainVtx[i].eta, &cv);
		MneeChain_StoreVtx(mnee, i, &cv);

		if (i + 1 < nn) {
			const float3 pProp = MneeChain_TrialPos(mnee, i + 1);
			MneeVtx vNext;
			MneeChain_LoadVtx(mnee, i + 1, &vNext);
			MneeChain_WriteReprojectRay(pProp, vNext.gn,
					.5f * MneeChain_TrialEps(x0p, vNext.p), ray, ray->time);
			*dlVolInfo = *srcVol;
			mnee->chainIdx = i + 1;
			return;
		}
		mnee->iteration++;
		// Step accepted: raise the line-search beta for the next Newton
		// iteration (CPU ordering: commit trial positions, then double).
		mnee->beta = fmin(1.f, 2.f * mnee->beta);
		if (mnee->iteration >= taskConfig->pathTracer.mnee.maxIterations) {
			lpi->mneeActive = false;
			return;
		}
		if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, srcVol)) {
			lpi->mneeActive = false;
		}
		return;
	}

	//--------------------------------------------------------------------------
	// MNEE_PHASE_MS_POST: per-vertex mode check + spec factor + volume
	//--------------------------------------------------------------------------
	if (mnee->phase == MNEE_PHASE_MS_POST) {
		const int k = mnee->chainIdx;
		if (rayHit->meshIndex == NULL_INDEX) {
			lpi->mneeActive = false;
			return;
		}
		if (mats[taskDirectLight->mneeBsdf.materialIndex].type != mnee->chainMatType[k]) {
			lpi->mneeActive = false;
			return;
		}

		MneeVtx vk;
		MneeChain_LoadVtx(mnee, k, &vk);
		const float3 pPrevK = (k == 0) ? x0p : MneeChain_VtxPos(mnee, k - 1);
		const float3 pNextK = (k == nn - 1) ? lightPos : MneeChain_VtxPos(mnee, k + 1);
		const float3 wik = normalize(pPrevK - vk.p);
		// LMNEE endpoints are finite lens points: never directional.
		const float3 wok = ((mnee->lightIsDir != 0) && (k == nn - 1)) ?
				pNextK : normalize(pNextK - vk.p);
		const float cosI = dot(vk.gn, wik);
		const float cosO = dot(vk.gn, wok);
		if (vk.eta == 1.f) {
			if (cosI * cosO < 0.f) {
				lpi->mneeActive = false;
				return;
			}
		} else {
			if (cosI * cosO > 0.f) {
				lpi->mneeActive = false;
				return;
			}
		}

		BSDFEvent specEvent;
		const float3 spec = Mnee_SpecFactor(
				&mats[taskDirectLight->mneeBsdf.materialIndex],
				&taskDirectLight->mneeBsdf.hitPoint,
				&taskDirectLight->mneeBsdf, wik, &specEvent
				MATERIALS_PARAM);
		if (Spectrum_IsBlack(spec)) {
			lpi->mneeActive = false;
			return;
		}
		mnee->chainSpecR *= spec.x;
		mnee->chainSpecG *= spec.y;
		mnee->chainSpecB *= spec.z;
		if (vk.eta != 1.f)
			mnee->plainHalfVector = 0;
		mnee->specEvent = specEvent;
		PathVolumeInfo_Update(dlVolInfo, specEvent,
				&taskDirectLight->mneeBsdf
				MATERIALS_PARAM);

		if (k + 1 < nn) {
			MneeVtx vNext;
			MneeChain_LoadVtx(mnee, k + 1, &vNext);
			const float epsNext = MneeChain_TrialEps(x0p, vNext.p);
			MneeChain_WriteReprojectRay(vNext.p, vNext.gn, .5f * epsNext, ray, ray->time);
			mnee->chainIdx = k + 1;
			return;
		}

		taskDirectLight->mneeBsdfFinal = taskDirectLight->mneeBsdf;
		mnee->specFactorR = mnee->chainSpecR;
		mnee->specFactorG = mnee->chainSpecG;
		mnee->specFactorB = mnee->chainSpecB;
		// Light side: the endpoint contribution is a camera splat, queued
		// through pendingSplat for the normal Stage A visibility resolve
		LMneeChain_SolveEnd(taskConfig, task, taskDirectLight, taskState, lpi,
				mnee, x0p, ray, filmWidth, filmHeight,
				filmSubRegion0, filmSubRegion1, filmSubRegion2,
				filmSubRegion3, camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
				, samplerSharedDataBuff
#endif
				MATERIALS_PARAM);
		return;
	}

	lpi->mneeActive = false;
}

// Start the light-side chain from a blocked connect context (mirror of
// MneeChain_StartFromSSFail, with one difference): the discovery ray is
// cast from the RECEIVER x0 toward the lens so the first specular hit
// becomes vertex 0. Re-walking the straight segment rebuilds the true
// occluder topology even when the triggering hit was a re-blocked
// manifold segment (a slab's far face), where the connect-side BSDF in
// task->tmpBsdf would seed the wrong interface.
OPENCL_FORCE_NOT_INLINE bool LMneeChain_Start(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global Ray *ray,
		__global LightPathInfo *lpi
		MATERIALS_PARAM_DECL
		) {
	if (!taskConfig->pathTracer.mnee.enabled)
		return false;
	if (taskConfig->pathTracer.mnee.maxSpecular <= 1)
		return false;

	__global MneeState *mnee = &taskDirectLight->mnee;
	const float3 lensPoint = MAKE_FLOAT3(lpi->lensPointX, lpi->lensPointY,
			lpi->lensPointZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	mnee->lightPosX = lensPoint.x;
	mnee->lightPosY = lensPoint.y;
	mnee->lightPosZ = lensPoint.z;
	mnee->mirrorMode = false;
	MneeChain_Begin(mnee, taskConfig->pathTracer.mnee.maxSpecular);

	// chainN stays 0: MS_DISCOVER stores the first specular hit as vertex 0
	const float3 dir = normalize(lensPoint - x0p);
	MneeChain_WriteDiscoverRay(&taskState->bsdf, dir, ray, ray->time);
	lpi->connectVolInfo = lpi->volume;
	mnee->phase = MNEE_PHASE_MS_DISCOVER;
	lpi->mneeActive = true;
	return true;
}

// Cold-first seed policy (eye-side Mnee_FailToChainOrExit parity): a
// failed single-vertex solve retries once from the cached vertex before
// escalating to the chain solver. The cache is a rescue only, never the
// default glass seed.
OPENCL_FORCE_NOT_INLINE void LMnee_FailToChainOrSeed(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global Ray *visRay,
		__global LightPathInfo *lpi,
		__global MneeSeedEntry *mneeSeeds,
		const float worldRadius
		MATERIALS_PARAM_DECL
		) {
	__global MneeState *mnee = &taskDirectLight->mnee;

	if (!mnee->seedCacheTried &&
			(mnee->phase < MNEE_PHASE_MS_DISCOVER) &&
			taskConfig->pathTracer.mnee.seedCacheEnable && mneeSeeds) {
		mnee->seedCacheTried = 1;
		const float3 occlP = MAKE_FLOAT3(mnee->occlX, mnee->occlY, mnee->occlZ);
		const float cellSize = fmax(worldRadius / MNEE_SEED_CELL_FRAC, 1e-4f);
		const uint seedMesh = mnee->shadowMeshIndex * 2u + mnee->shadowSide;
		const uint key = Mnee_SeedKey(LMNEE_CAMERA_SEED_ID,
				seedMesh, occlP, cellSize);
		MneeVtx v;
		Mnee_LoadVtx(mnee, &v);
		if (Mnee_SeedCacheLookup(mneeSeeds, key, LMNEE_CAMERA_SEED_ID,
				seedMesh, mnee->mirrorMode, v.eta, mnee)) {
			const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);
			Mnee_ApplySeedShift(mnee, x0p);
			mnee->phase = MNEE_PHASE_STEP;
			mnee->iteration = 0;
			mnee->beta = 1.f;
			return;
		}
	}

	if (!LMneeChain_Start(taskConfig, task, taskDirectLight,
			taskState, visRay, lpi MATERIALS_PARAM))
		lpi->mneeActive = false;
}

// One launch of the light-side MNEE sub-state machine: consumes the trace
// result sitting in the visibility slot and either writes the next trace
// or exits (success -> pendingSplat queued for Stage A, failure -> drop).
OPENCL_FORCE_NOT_INLINE void LMnee_ProcessState(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *task,
		__global GPUTaskDirectLight *taskDirectLight,
		__global GPUTaskState *taskState,
		__global LightPathInfo *lpi,
		__global Ray *visRay, __global RayHit *visRayHit,
		__global SampleResult *sampleResult,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3,
		const float worldRadius,
		__global MneeSeedEntry *mneeSeeds
		, __global const Camera* restrict camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		, __global void *samplerSharedDataBuff
#endif
		MATERIALS_PARAM_DECL
		) {
	__global MneeState *mnee = &taskDirectLight->mnee;
	const float3 lensPoint = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY,
			mnee->lightPosZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	// Multi-specular chain phases (>= MS_DISCOVER): a closed dielectric
	// needs more than one refracting vertex, so the single-vertex machine
	// hands off here once a chain was started (see LMneeChain_Start).
	if (mnee->phase >= MNEE_PHASE_MS_DISCOVER) {
		LMneeChain_ProcessState(taskConfig, task, taskDirectLight, taskState,
				lpi, visRay, visRayHit, sampleResult,
				filmWidth, filmHeight,
				filmSubRegion0, filmSubRegion1,
				filmSubRegion2, filmSubRegion3,
				&lpi->volume, &lpi->connectVolInfo,
				camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
				, samplerSharedDataBuff
#endif
				MATERIALS_PARAM);
		return;
	}

	MneeVtx v;
	Mnee_LoadVtx(mnee, &v);

	// MNEE_PHASE_STEP: Newton loop top, no trace to consume
	if (mnee->phase == MNEE_PHASE_STEP) {
		float g;
		const int stepResult = Mnee_StepAndWriteProposal(taskConfig, mnee,
				x0p, lensPoint, &v, &taskState->bsdf, visRay, &g);
		if (stepResult == 1) {
			mnee->phase = MNEE_PHASE_PROP_TRACE;
			lpi->connectVolInfo = lpi->volume;
			return;
		}
		if (stepResult == 2) {
			LMnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, lpi,
					mnee, x0p, g, visRay, filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1, filmSubRegion2,
					filmSubRegion3, mneeSeeds, worldRadius,
					camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
					, samplerSharedDataBuff
#endif
					MATERIALS_PARAM);
			return;
		}
		// Single-vertex solve failed: hand off to the chain (CPU
		// LMNEEMultiConnectToEye fallback after LMNEEConnectToEye)
		// Single-vertex solve failed: retry once from the seed cache,
		// else hand off to the chain (CPU LMNEEMultiConnectToEye parity)
		LMnee_FailToChainOrSeed(taskConfig, task, taskDirectLight,
				taskState, visRay, lpi, mneeSeeds, worldRadius
				MATERIALS_PARAM);
		return;
	}

	// Consume the trace result of the current LMNEE ray (seed / proposal).
	// Same volume-walk convention as the eye side: no depth context.
	int throughShadowTransparency = false;
	float3 connectionThroughput;
	const bool continueToTrace = Scene_Intersect(taskConfig,
			LIGHT_RAY | INDIRECT_RAY,
			NULL, NONE,
			&throughShadowTransparency, &lpi->connectVolInfo,
			&task->tmpHitPoint,
			.5f,
			visRay, visRayHit, &taskDirectLight->mneeBsdf,
			&connectionThroughput,
			WHITE, sampleResult, false
			MATERIALS_PARAM);
	if (continueToTrace)
		return;

	if (mnee->phase == MNEE_PHASE_SEED_TRACE) {
		if (visRayHit->meshIndex == mnee->shadowMeshIndex) {
			MneeVtx vSeed;
			Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf, v.eta, &vSeed);
			Mnee_StoreVtx(mnee, &vSeed);
		}
		Mnee_ApplySeedShift(mnee, x0p);
		Mnee_LoadVtx(mnee, &v);
		mnee->phase = MNEE_PHASE_STEP;
		return;
	}

	// MNEE_PHASE_PROP_TRACE
	if (visRayHit->meshIndex == NULL_INDEX) {
		LMnee_FailToChainOrSeed(taskConfig, task, taskDirectLight,
				taskState, visRay, lpi, mneeSeeds, worldRadius
				MATERIALS_PARAM);
		return;
	}

	bool stepRejected;
	bool converged = false;
	float gConverged = 0.f;
	if (visRayHit->meshIndex != mnee->shadowMeshIndex) {
		stepRejected = true;
	} else {
		MneeVtx vProp;
		Mnee_InitVtxFromBsdf(&taskDirectLight->mneeBsdf, v.eta, &vProp);
		float2 CProp;
		if (!Mnee_Residual(x0p, lensPoint, mnee->lightIsDir != 0, &vProp, &CProp)) {
			stepRejected = true;
		} else {
			const float resPropNorm = length(CProp);
			if (resPropNorm < mnee->resNorm) {
				mnee->beta = fmin(1.f, 2.f * mnee->beta);
				mnee->iteration++;
				Mnee_StoreVtx(mnee, &vProp);
				taskDirectLight->mneeBsdfFinal = taskDirectLight->mneeBsdf;
				v = vProp;
				stepRejected = false;

				if (mnee->iteration >= taskConfig->pathTracer.mnee.maxIterations) {
					LMnee_FailToChainOrSeed(taskConfig, task,
							taskDirectLight, taskState, visRay, lpi,
							mneeSeeds, worldRadius MATERIALS_PARAM);
					return;
				}
				if (resPropNorm < 3e-4f) {
					converged = true;
					gConverged = Mnee_GeometricTerm(x0p, lensPoint,
							mnee->lightIsDir != 0, &vProp, NULL);
				}
			} else
				stepRejected = true;
		}
	}

	if (converged) {
		LMnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, lpi,
				mnee, x0p, gConverged, visRay, filmWidth, filmHeight,
				filmSubRegion0, filmSubRegion1, filmSubRegion2,
				filmSubRegion3, mneeSeeds, worldRadius,
				camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
				, samplerSharedDataBuff
#endif
				MATERIALS_PARAM);
		return;
	}

	if (stepRejected) {
		mnee->beta *= .5f;
		mnee->iteration++;
	}

	float g;
	const int stepResult = Mnee_StepAndWriteProposal(taskConfig, mnee,
			x0p, lensPoint, &v, &taskState->bsdf, visRay, &g);
	if (stepResult == 1) {
		mnee->phase = MNEE_PHASE_PROP_TRACE;
		lpi->connectVolInfo = lpi->volume;
		return;
	}
	if (stepResult == 2) {
		LMnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, lpi,
				mnee, x0p, g, visRay, filmWidth, filmHeight,
				filmSubRegion0, filmSubRegion1, filmSubRegion2,
				filmSubRegion3, mneeSeeds, worldRadius,
				camera
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
				, samplerSharedDataBuff
#endif
				MATERIALS_PARAM);
		return;
	}
	LMnee_FailToChainOrSeed(taskConfig, task, taskDirectLight,
			taskState, visRay, lpi, mneeSeeds, worldRadius
			MATERIALS_PARAM);
	return;
}


//------------------------------------------------------------------------------
// Kernel parameters
//------------------------------------------------------------------------------

#define KERNEL_ARGS_VOLUMES \
		, __global PathVolumeInfo *directLightVolInfos

#define KERNEL_ARGS_INFINITELIGHTS \
		, const float worldCenterX \
		, const float worldCenterY \
		, const float worldCenterZ \
		, const float worldRadius

#define KERNEL_ARGS_NORMALS_BUFFER \
		, __global const Normal* restrict vertNormals
#define KERNEL_ARGS_TRINORMALS_BUFFER \
		, __global const Normal* restrict triNormals
#define KERNEL_ARGS_UVS_BUFFER \
		, __global const UV* restrict vertUVs
#define KERNEL_ARGS_COLS_BUFFER \
		, __global const Spectrum* restrict vertCols
#define KERNEL_ARGS_ALPHAS_BUFFER \
		, __global const float* restrict vertAlphas
#define KERNEL_ARGS_VERTEXAOVS_BUFFER \
		, __global const float* restrict vertexAOVs
#define KERNEL_ARGS_TRIAOVS_BUFFER \
		, __global const float* restrict triAOVs

#define KERNEL_ARGS_ENVLIGHTS \
		, __global const uint* restrict envLightIndices \
		, const uint envLightCount

#define KERNEL_ARGS_INFINITELIGHT \
		, __global const float* restrict envLightDistribution

#define KERNEL_ARGS_IMAGEMAPS_PAGES \
		, __global const ImageMap* restrict imageMapDescs \
		, __global const float* restrict imageMapBuff0 \
		, __global const float* restrict imageMapBuff1 \
		, __global const float* restrict imageMapBuff2 \
		, __global const float* restrict imageMapBuff3 \
		, __global const float* restrict imageMapBuff4 \
		, __global const float* restrict imageMapBuff5 \
		, __global const float* restrict imageMapBuff6 \
		, __global const float* restrict imageMapBuff7

#define KERNEL_ARGS_FAST_PIXEL_FILTER \
		, __global float *pixelFilterDistribution

#define KERNEL_ARGS_PHOTONGI \
		, __global const RadiancePhoton* restrict pgicRadiancePhotons \
		, uint pgicLightGroupCounts \
		, __global const Spectrum* restrict pgicRadiancePhotonsValues \
		, __global const IndexBVHArrayNode* restrict pgicRadiancePhotonsBVHNodes \
		, __global const Photon* restrict pgicCausticPhotons \
		, __global const IndexBVHArrayNode* restrict pgicCausticPhotonsBVHNodes

#define KERNEL_ARGS \
		__constant const GPUTaskConfiguration* restrict taskConfig \
		, __global GPUTask *tasks \
		, __global GPUTaskDirectLight *tasksDirectLight \
		, __global GPUTaskState *tasksState \
		, __global GPUTaskStats *taskStats \
		KERNEL_ARGS_FAST_PIXEL_FILTER \
		, __global void *samplerSharedDataBuff \
		, __global void *samplesBuff \
		, __global float *samplesDataBuff \
		, __global SampleResult *sampleResultsBuff \
		, __global EyePathInfo *eyePathInfos \
		, __global RestirReservoir *restirReservoirs \
		, __global MneeSeedEntry *mneeSeeds \
		KERNEL_ARGS_VOLUMES \
		, __global Ray *rays \
		, __global RayHit *rayHits \
		/* Film parameters */ \
		KERNEL_ARGS_FILM \
		/* Scene parameters */ \
		KERNEL_ARGS_INFINITELIGHTS \
		, __global const Material* restrict mats \
		, __global const MaterialEvalOp* restrict matEvalOps \
		, __global float *matEvalStacks \
		, const uint maxMaterialEvalStackSize \
		, __global const Texture* restrict texs \
		, __global const TextureEvalOp* restrict texEvalOps \
		, __global float *texEvalStacks \
		, const uint maxTextureEvalStackSize \
		, __global const SceneObject* restrict sceneObjs \
		, __global const ExtMesh* restrict meshDescs \
		, __global const Point* restrict vertices \
		KERNEL_ARGS_NORMALS_BUFFER \
		KERNEL_ARGS_TRINORMALS_BUFFER \
		KERNEL_ARGS_UVS_BUFFER \
		KERNEL_ARGS_COLS_BUFFER \
		KERNEL_ARGS_ALPHAS_BUFFER \
		KERNEL_ARGS_VERTEXAOVS_BUFFER \
		KERNEL_ARGS_TRIAOVS_BUFFER \
		, __global const Triangle* restrict triangles \
		, __global const InterpolatedTransform* restrict interpolatedTransforms \
		, __global const Camera* restrict camera \
		, __global const float* restrict cameraBokehDistribution \
		/* Lights */ \
		, __global const LightSource* restrict lights \
		KERNEL_ARGS_ENVLIGHTS \
		, __global const uint* restrict lightIndexOffsetByMeshIndex \
		, __global const uint* restrict lightIndexByTriIndex \
		KERNEL_ARGS_INFINITELIGHT \
		, __global const float* restrict lightsDistribution \
		, __global const float* restrict infiniteLightSourcesDistribution \
		, __global const DLSCacheEntry* restrict dlscAllEntries \
		, __global const float* restrict dlscDistributions \
		, __global const IndexBVHArrayNode* restrict dlscBVHNodes \
		, const float dlscRadius2 \
		, const float dlscNormalCosAngle \
		/* Light BVH strategy (E&K'18): node array + per-light leaf \
		 * index table. Null for the other strategies; gated like \
		 * dlscAllEntries at the call sites */ \
		, __global const LightBVHNode* restrict lightBVHNodes \
		, __global const uint* restrict lightBVHLightToLeaf \
		, const float lightBVHMinDist2 \
		, __global const ELVCacheEntry* restrict elvcAllEntries \
		, __global const float* restrict elvcDistributions \
		, __global const uint* restrict elvcTileDistributionOffsets \
		, __global const IndexBVHArrayNode* restrict elvcBVHNodes \
		, const float elvcRadius2 \
		, const float elvcNormalCosAngle \
		, const uint elvcTilesXCount \
		, const uint elvcTilesYCount \
		/* Images */ \
		KERNEL_ARGS_IMAGEMAPS_PAGES \
		KERNEL_ARGS_PHOTONGI \
		/* Path guiding (P1-3 M4e): flattened SD-tree (uint4 per node,
		 * root at index 0) + per-leaf vMF mixture records (24 floats
		 * per leaf). Null when unguided; gated on guidingEnable. */ \
		, __global const uint4* restrict guideNodes \
		, __global const float* restrict guideLeaves \
		, const uint guidingEnable \
		/* Guiding stats: [0]=tryGuide hits, [1]=guide-ok */ \
		, __global uint* restrict guideDbgBuff \
		/* Path guiding (P1-3 M2b-2): 16 training-record buffers (8KB
	 * each, 256 float4 records). Task t writes buffer (t&15), slot (t>>5)&255. */ \
		, __global float4* restrict guideRec0 \
		, __global float4* restrict guideRec1 \
		, __global float4* restrict guideRec2 \
		, __global float4* restrict guideRec3 \
		, __global float4* restrict guideRec4 \
		, __global float4* restrict guideRec5 \
		, __global float4* restrict guideRec6 \
		, __global float4* restrict guideRec7 \
		, __global float4* restrict guideRec8 \
		, __global float4* restrict guideRec9 \
		, __global float4* restrict guideRec10 \
		, __global float4* restrict guideRec11 \
		, __global float4* restrict guideRec12 \
		, __global float4* restrict guideRec13 \
		, __global float4* restrict guideRec14 \
		, __global float4* restrict guideRec15 \
		/* Portal-guided bounce sampling (M5): 4 float4 records per
		 * aperture rect (layout in Portal_RectPdfW). Null when
		 * portalCount == 0; gated on taskConfig->pathTracer.portalCount. */ \
		, __global const float4* restrict portalRects \
		/* Native curve primitives (Metal HWRT): control points (float4
		 * xyz+radius), global per-segment start indices, per-cp attrs
		 * (2 float4/cp). Null when no mesh carries curve data; only
		 * dereferenced under RAYHIT_CURVE_FLAG hits. */ \
		, __global const float4* restrict curveCps \
		, __global const uint* restrict curveSegIndices \
		, __global const float4* restrict curveCpAttrs \
		/* Wavefront per-state task queues (B2/E3): when
		 * wavefrontEnable != 0, lane gid maps to
		 * taskQueueBuf[taskQueueState * taskQueueStride + gid]
		 * instead of indexing task arrays directly. A per-iteration
		 * BuildQueues kernel refills the queues from the
		 * authoritative taskState->state. taskQueueCount holds
		 * NUM_STATES * SLG_SPECTRAL_BINS histogram counters
		 * (per-state totals are their sums; M2 lambda bucketing). */ \
		, __global const uint* restrict taskQueueBuf \
		, __global const uint* restrict taskQueueCount \
		, const uint taskQueueStride \
		, const uint taskQueueState \
		, const uint wavefrontEnable \
		/* JH2019 spectral upsampling table (TEXTURES_PARAM tail): \
		 * NULL unless path.spectral.upsampling=jh2019 */ \
		, __global const float* restrict spectralUpsamplingTable \
		/* Heterogeneous volume majorant cells (TEXTURES_PARAM tail): \
		 * NULL when no volume uses delta tracking */ \
		, __global const float* restrict volMajorants \
		/* Point-ish light positions for equiangular distance sampling \
		 * (TEXTURES_PARAM tail): NULL/0 when none */ \
		, __global const float4* restrict eqLightPoints \
		, const uint eqLightCount

// GPU light tracing (doc/features/gpu_lighttracing.md): extra args of
// the light-path kernels only. Keeping them out of KERNEL_ARGS avoids
// growing the parameter list of every other kernel (Apple's
// OpenCL-on-Metal translator has a low buffer-argument limit - see the
// WAVEFRONT_GID comment). The screen-normalized radiance groups are
// passed here (not via KERNEL_ARGS_FILM) for the same reason.
#define KERNEL_ARGS_LIGHT \
		, __global LightPathInfo *lightPathInfos \
		, __global const float* restrict emitLightsDistribution \
		, __global float4 *lightFocus \
		, __global uint *lightFocusCount \
		, __global float *filmScreenRadianceGroup0 \
		, __global float *filmScreenRadianceGroup1 \
		, __global float *filmScreenRadianceGroup2 \
		, __global float *filmScreenRadianceGroup3 \
		, __global float *filmScreenRadianceGroup4 \
		, __global float *filmScreenRadianceGroup5 \
		, __global float *filmScreenRadianceGroup6 \
		, __global float *filmScreenRadianceGroup7 \
		, __global const float* restrict lightFilterLUTs \
		/* Vertex connection (M6): the light vertex cache written by \
		 * MK_LIGHT_VERTEX. NULL when vertexConnect is disabled */ \
		, __global VCLightVertex *lightVertices

// Vertex connection (M6): eye-side kernels need the emit light strategy
// distribution for the CPU DirectHitLight weightCamera pick pdf (the
// light sub-path was picked with it in MK_LIGHT_INIT). Kept out of
// KERNEL_ARGS for the same Apple argument-limit reason as
// KERNEL_ARGS_LIGHT. Bound to emitLightsDistributionBuff (NULL-safe:
// gated on vertexConnect.enabled).
#define KERNEL_ARGS_VC \
		, __global const float* restrict emitLightsDistribution

// Wavefront lane -> task index mapping. Under wavefrontEnable, lane
// gid indexes this kernel's state queue; otherwise the dense mapping
// (gid == task index) is used. Task data keeps being indexed by the
// returned value, so downstream code is unchanged.
//
// The mapping is selected at kernel-compile time through the
// PATHOCL_WAVEFRONT_QUEUES define (host: wavefrontQueues). A runtime
// flag would work too, but keeping the queue dereferences in the IR
// of the dense-mode kernels pushes the heaviest AdvancePaths_MK_*
// kernels past the buffer-argument limit of Apple's OpenCL-on-Metal
// translator (dispatch crashed inside AGX::ComputeContext::
// prepareForEnqueue on MK_DL_ILLUMINATE / MK_DL_SAMPLE_BSDF).
#if defined(PATHOCL_WAVEFRONT_QUEUES)
#define WAVEFRONT_GID \
	taskQueueBuf[taskQueueState * taskQueueStride + get_global_id(0)]

// Wavefront lane bounds check + gid mapping. Per-state launches are
// rounded up to the workgroup size (OpenCL requires global size to be
// a multiple of it); lanes beyond the compacted queue length exit
// before dereferencing the queue, whose tail slots hold stale task
// indices from the previous iteration. The state launch covers the
// sum of the per-(state, lambda) histogram counters (M2 lambda
// bucketing keeps the queue layout flat). Must be the first statement
// of every AdvancePaths_MK_* kernel.
#define WAVEFRONT_GUARD \
	if (get_global_id(0) >= \
			taskQueueCount[taskQueueState * SLG_SPECTRAL_BINS] + \
			taskQueueCount[taskQueueState * SLG_SPECTRAL_BINS + 1] + \
			taskQueueCount[taskQueueState * SLG_SPECTRAL_BINS + 2]) \
		return; \
	const size_t gid = WAVEFRONT_GID;
#else
#define WAVEFRONT_GUARD \
	const size_t gid = get_global_id(0);
#endif


//------------------------------------------------------------------------------
// To initialize image maps page pointer table
//------------------------------------------------------------------------------

#define INIT_IMAGEMAPS_PAGES \
	__global const float* restrict imageMapBuff[8]; \
	imageMapBuff[0] = imageMapBuff0; \
	imageMapBuff[1] = imageMapBuff1; \
	imageMapBuff[2] = imageMapBuff2; \
	imageMapBuff[3] = imageMapBuff3; \
	imageMapBuff[4] = imageMapBuff4; \
	imageMapBuff[5] = imageMapBuff5; \
	imageMapBuff[6] = imageMapBuff6; \
	imageMapBuff[7] = imageMapBuff7;

//------------------------------------------------------------------------------
// Init Kernels
//------------------------------------------------------------------------------

__kernel void InitSeed(__global GPUTask *tasks,
		const uint seedBase) {
	const size_t gid = get_global_id(0);

	// Initialize random number generator

	Seed seed;
	Rnd_Init(seedBase + gid, &seed);

	// Save the seed
	__global GPUTask *task = &tasks[gid];
	task->seed = seed;
}

__kernel void Init(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global GPUTask *tasks,
		__global GPUTaskDirectLight *tasksDirectLight,
		__global GPUTaskState *tasksState,
		__global GPUTaskStats *taskStats,
		__global void *samplerSharedDataBuff,
		__global void *samplesBuff,
		__global float *samplesDataBuff,
		__global SampleResult *sampleResultsBuff,
		__global EyePathInfo *eyePathInfos,
		__global RestirReservoir *restirReservoirs,
		__global float *pixelFilterDistribution,
		__global Ray *rays,
		__global Camera *camera,
		__global const float* restrict cameraBokehDistribution
		KERNEL_ARGS_FILM
		) {
	const size_t gid = get_global_id(0);

	// ReSTIR visibility (E2a): mark the candidate shadow-ray slots in
	// the rays[] tail as masked so the first trace pass skips them
	// (they are written by MK_DL_ILLUMINATE before they are ever
	// consumed, but the buffer starts uninitialized).
	const uint visCandCount = taskConfig->pathTracer.restir.visCandCount;
	for (uint i = 0; i < visCandCount; ++i)
		rays[taskConfig->pathTracer.restir.visCandRayBase +
				gid * visCandCount + i].flags = RAY_FLAGS_MASKED;

	__global GPUTaskState *taskState = &tasksState[gid];

	// GPU light tracing (doc/features/gpu_lighttracing.md): tasks
	// [eyeTaskCount, taskCount) are light-path tasks cycling
	// MK_LIGHT_INIT <-> MK_LIGHT_VERTEX.
	if (taskConfig->pathTracer.lightTracing.enabled &&
			gid >= taskConfig->pathTracer.lightTracing.eyeTaskCount) {
		// Read the seed (required by SAMPLER_PARAM)
		Seed ltSeedValue = tasks[gid].seed;
		Seed *ltSeed = &ltSeedValue;

		Sampler_LightTaskInit(taskConfig,
				gid - taskConfig->pathTracer.lightTracing.eyeTaskCount,
				filmWidth, filmHeight
				, ltSeed
				, samplerSharedDataBuff
				, samplesBuff
				, samplesDataBuff
				, sampleResultsBuff
				, gid);

		// Mask both ray slots: the path ray and the camera-visibility
		// ray tail slot
		rays[gid].flags = RAY_FLAGS_MASKED;
		rays[taskConfig->pathTracer.lightTracing.lightVisRayBase +
				gid - taskConfig->pathTracer.lightTracing.eyeTaskCount].flags =
				RAY_FLAGS_MASKED;

		taskStats[gid].sampleCount = 0;
		taskState->state = MK_LIGHT_INIT;

		tasks[gid].seed = ltSeedValue;
		return;
	}

#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
	__global TilePathSamplerSharedData *samplerSharedData = (__global TilePathSamplerSharedData *)samplerSharedDataBuff;

	if (gid >= filmWidth * filmHeight * Sqr(samplerSharedData->aaSamples)) {
		taskState->state = MK_DONE;
		// Mark the ray like like one to NOT trace
		rays[gid].flags = RAY_FLAGS_MASKED;

		return;
	}
#endif

	// Initialize the task
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskDirectLight *taskDirectLight = &tasksDirectLight[gid];

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by Sampler_GetSample() macro
	Seed *seed = &seedValue;

	// Initialize the sample and path
	const bool validSample = Sampler_Init(taskConfig,
			filmNoise,
			filmUserImportance,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3
			SAMPLER_PARAM);

	if (validSample) {
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		__global TilePathSamplerSharedData *samplerSharedData = (__global TilePathSamplerSharedData *)samplerSharedDataBuff;
		const uint cameraFilmWidth = samplerSharedData->cameraFilmWidth;
		const uint cameraFilmHeight = samplerSharedData->cameraFilmHeight;
		const uint tileStartX = samplerSharedData->tileStartX;
		const uint tileStartY =  samplerSharedData->tileStartY;
#endif

		// Generate the eye path
		GenerateEyePath(taskConfig,
				taskDirectLight, taskState,
				camera,
				cameraBokehDistribution,
				filmWidth, filmHeight,
				filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3,
				pixelFilterDistribution,
				&rays[gid],
				&eyePathInfos[gid]
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
				, cameraFilmWidth, cameraFilmHeight,
				tileStartX, tileStartY
#endif
				SAMPLER_PARAM);
	} else {
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
		taskState->state = MK_DONE;
#else
		taskState->state = MK_GENERATE_CAMERA_RAY;
#endif
		// Mark the ray like like one to NOT trace
		rays[gid].flags = RAY_FLAGS_MASKED;
	}

	// Save the seed
	task->seed = seedValue;

	__global GPUTaskStats *taskStat = &taskStats[gid];
	taskStat->sampleCount = 0;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
