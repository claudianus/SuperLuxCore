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
		__global EyePathInfo *pathInfo, __global const Spectrum* restrict pathThroughput,
		const __global Ray *ray, __global const BSDF *bsdf, __global SampleResult *sampleResult
		LIGHTS_PARAM_DECL) {
	// If the material is shadow transparent, Direct Light sampling
	// will take care of transporting all emitted light
	if (bsdf && bsdf->hitPoint.throughShadowTransparency)
		return;

	const float3 throughput = VLOAD3F(pathThroughput->c);

	for (uint i = 0; i < envLightCount; ++i) {
		__global const LightSource* restrict light = &lights[envLightIndices[i]];

		// Check if the light source is visible according the settings
		if (!CheckDirectHitVisibilityFlags(light, &pathInfo->depth, pathInfo->lastBSDFEvent))
			continue;

		float directPdfW;
		const float3 envRadianceRGB = EnvLight_GetRadiance(light, bsdf,
				-VLOAD3F(&ray->d.x), &directPdfW
				LIGHTS_PARAM);
#if defined(SLG_SPECTRAL)
		// Env lights carry baked RGB radiance: upsample to the path bins
		// with the illuminant basis at this funnel (bsdf may be a miss
		// path, so the wavelengths come from the SampleResult).
		const float3 envRadiance = Spectral_Upsample(envRadianceRGB,
				sampleResult->spectralW, sampleResult->spectralHeroAlive, true);
#else
		const float3 envRadiance = envRadianceRGB;
#endif

		if (!Spectrum_IsBlack(envRadiance)) {
			float weight;
			if (!(pathInfo->lastBSDFEvent & SPECULAR)) {
				const float lightPickProb = LightStrategy_SampleLightPdf(lightsDistribution,
						dlscAllEntries,
						dlscDistributions, dlscBVHNodes,
						dlscRadius2, dlscNormalCosAngle,
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
		__global EyePathInfo *pathInfo,
		__global const Spectrum* restrict pathThroughput, const __global Ray *ray,
		const float distance, __global const BSDF *bsdf,
		__global SampleResult *sampleResult
		LIGHTS_PARAM_DECL) {
	__global const LightSource* restrict light = &lights[bsdf->triangleLightSourceIndex];

	// Check if the light source is visible according the settings
	if (!CheckDirectHitVisibilityFlags(light, &pathInfo->depth, pathInfo->lastBSDFEvent) ||
			// If the material is shadow transparent, Direct Light sampling
			// will take care of transporting all emitted light
			bsdf->hitPoint.throughShadowTransparency)
		return;
	
	float directPdfA;
	const float3 emittedRadiance = BSDF_GetEmittedRadiance(bsdf, &directPdfA
			LIGHTS_PARAM);

	if (!Spectrum_IsBlack(emittedRadiance)) {
		// Add emitted radiance
		float weight = 1.f;
		if (!(pathInfo->lastBSDFEvent & SPECULAR)) {
			const float lightPickProb = LightStrategy_SampleLightPdf(lightsDistribution,
					dlscAllEntries,
					dlscDistributions, dlscBVHNodes,
					dlscRadius2, dlscNormalCosAngle,
					VLOAD3F(&ray->o.x), VLOAD3F(&pathInfo->lastShadeN.x),
					pathInfo->lastFromVolume,
					light->lightSceneIndex);

#if !defined(RENDER_ENGINE_RTPATHOCL)
			// This is a specific check to avoid fireflies with DLSC
			if ((lightPickProb == 0.f) && light->isDirectLightSamplingEnabled && dlscAllEntries)
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
		__global DirectLightIlluminateInfo *info
		LIGHTS_PARAM_DECL) {
	// Select the light strategy to use
	__global const float* restrict lightDist = BSDF_IsShadowCatcherOnlyInfiniteLights(bsdf MATERIALS_PARAM) ?
		infiniteLightSourcesDistribution : lightsDistribution;

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

		for (uint i = 0; i < M; ++i) {
			const float u_i = fmod(u0 + i * (1.f / M), 1.f);

			float candPickPdf;
			const uint candIndex = LightStrategy_SampleLights(lightDist,
					dlscAllEntries,
					dlscDistributions, dlscBVHNodes,
					dlscRadius2, dlscNormalCosAngle,
					VLOAD3F(&bsdf->hitPoint.p.x), BSDF_GetLandingGeometryN(bsdf),
					bsdf->isVolume,
					u_i, &candPickPdf);
			if ((candIndex == NULL_INDEX) || (candPickPdf <= 0.f))
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
					shadowRay, &candPdfW
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
			}
		}

		// Empty reservoir (no contributing candidate)
		if (resLightIndex == NULL_INDEX)
			return false;

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
		float wSumTotal = wSum;
		uint MTotal = M;
		float curTarget = resTarget;
		uint curLightIndex = resLightIndex;
		float curPickPdf = resPickPdf;

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
					}
				}
			}

			if (restirStoreEnable) {
				// Store the merged reservoir for the next pass (only
				// depth-0 vertices refresh the cell, so it always
				// represents a primary-hit reservoir; deeper vertices
				// still merge with it, they just do not overwrite it).
				reservoir->lightIndex = curLightIndex;
				reservoir->wSum = wSumTotal;
				reservoir->M = MTotal;
				reservoir->target = curTarget;
			}
		}

		lightIndex = curLightIndex;
		lightPickPdf = curPickPdf;
		risScale = wSumTotal / (MTotal * curTarget);
	} else {
		lightIndex = LightStrategy_SampleLights(lightDist,
				dlscAllEntries,
				dlscDistributions, dlscBVHNodes,
				dlscRadius2, dlscNormalCosAngle,
				VLOAD3F(&bsdf->hitPoint.p.x), BSDF_GetLandingGeometryN(bsdf),
				bsdf->isVolume,
				u0, &lightPickPdf);
		if ((lightIndex == NULL_INDEX) || (lightPickPdf <= 0.f))
			return false;
	}

	__global const LightSource* restrict light = &lights[lightIndex];

	info->lightIndex = lightIndex;
	info->lightID = light->lightID;
	info->pickPdf = lightPickPdf;

	// Illuminate the point
	float directPdfW;
	const float3 lightRadiance = Light_Illuminate(
			&lights[lightIndex],
			bsdf,
			time, u1, u2,
			lightPassThroughEvent,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius,
			tmpHitPoint,		
			shadowRay, &directPdfW
			LIGHTS_PARAM);
	
	if (Spectrum_IsBlack(lightRadiance))
		return false;
	else {
		info->directPdfW = directPdfW;
		info->risScale = risScale;
		VSTORE3F(lightRadiance, info->lightRadiance.c);
		VSTORE3F(lightRadiance, info->lightIrradiance.c);
		return true;
	}
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Path guiding (P1-3 M2b): frozen coarse-grid directional guide sampling.
//
// Ports PathGuidingCache::Sample/Pdf/CosineSample (CPU reference) at the
// coarse GPU resolution: 8^3 spatial cells, 8x4 directional bins (equal
// area), cell-major 33 floats per cell (32 bins + total). The host
// downsamples the CPU field (mass-preserving pooling) and uploads 16
// chunks of 32 cells (4224B each): this Metal backend silently drops
// host-to-device uploads above ~8KB, so one big 2MB buffer never lands.
// Training records flow device-to-host through 16 small record buffers
// (M2b-2); sampling queries still read the frozen chunks, so all queries
// agree by construction within a round.
//------------------------------------------------------------------------------

#define GUIDE_GRID_RES 8u
#define GUIDE_DIR_PHI 8u
#define GUIDE_DIR_THETA 4u
#define GUIDE_DIR_BINS 32u
#define GUIDE_WARMUP_RECORDS 256.f
#define GUIDE_BIN_OMEGA ((2.f * M_PI_F / 8.f) * (2.f / 4.f))
#define GUIDE_CHUNK_CELLS 32u
#define GUIDE_CELL_FLOATS 33u

// Chunk select: 16 frozen chunk buffers, chunk = coarseCell >> 5
// Chunk select: 16 frozen chunk buffers, chunk = coarseCell >> 5
OPENCL_FORCE_INLINE __global const float *Guide_Chunk(uint ch,
		__global const float *c0, __global const float *c1,
		__global const float *c2, __global const float *c3,
		__global const float *c4, __global const float *c5,
		__global const float *c6, __global const float *c7,
		__global const float *c8, __global const float *c9,
		__global const float *c10, __global const float *c11,
		__global const float *c12, __global const float *c13,
		__global const float *c14, __global const float *c15) {
	switch (ch) {
		case 0u: return c0;
		case 1u: return c1;
		case 2u: return c2;
		case 3u: return c3;
		case 4u: return c4;
		case 5u: return c5;
		case 6u: return c6;
		case 7u: return c7;
		case 8u: return c8;
		case 9u: return c9;
		case 10u: return c10;
		case 11u: return c11;
		case 12u: return c12;
		case 13u: return c13;
		case 14u: return c14;
		default: return c15;
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

OPENCL_FORCE_INLINE uint Guide_CellIndex(float3 p,
		float minX, float minY, float minZ, float invSize) {
	const uint ix = (uint)(clamp((p.x - minX) * invSize, 0.f, 0.99999994f) * 8.f);
	const uint iy = (uint)(clamp((p.y - minY) * invSize, 0.f, 0.99999994f) * 8.f);
	const uint iz = (uint)(clamp((p.z - minZ) * invSize, 0.f, 0.99999994f) * 8.f);
	return min(ix + iy * 8u + iz * 64u, 511u);
}

// Fine 16^3 cell index for M2b-2 training records (matches the CPU
// PathGuidingCache layout that the host drain fills directly)
OPENCL_FORCE_INLINE uint Guide_CellIndex16(float3 p,
		float minX, float minY, float minZ, float invSize) {
	const uint ix = (uint)(clamp((p.x - minX) * invSize, 0.f, 0.99999994f) * 16.f);
	const uint iy = (uint)(clamp((p.y - minY) * invSize, 0.f, 0.99999994f) * 16.f);
	const uint iz = (uint)(clamp((p.z - minZ) * invSize, 0.f, 0.99999994f) * 16.f);
	return min(ix + iy * 16u + iz * 256u, 4095u);
}

OPENCL_FORCE_INLINE uint Guide_DirBin(float3 dir) {
	const float phi = atan2(dir.y, dir.x);
	const float c = clamp(dir.z, -1.f, 1.f);
	const uint pi = min((uint)((phi + M_PI_F) / (2.f * M_PI_F) * 8.f), 7u);
	const uint ti = min((uint)((c * .5f + .5f) * 4.f), 3u);
	return ti * 8u + pi;
}

// Fine 16x8 directional bin for M2b-2 training records
OPENCL_FORCE_INLINE uint Guide_DirBin16(float3 dir) {
	const float phi = atan2(dir.y, dir.x);
	const float c = clamp(dir.z, -1.f, 1.f);
	const uint pi = min((uint)((phi + M_PI_F) / (2.f * M_PI_F) * 16.f), 15u);
	const uint ti = min((uint)((c * .5f + .5f) * 8.f), 7u);
	return ti * 16u + pi;
}

OPENCL_FORCE_INLINE float3 Guide_BinDir(uint bin, float u0, float u1) {
	const uint pi = bin % 8u;
	const uint ti = bin / 8u;
	const float phi = (((float)pi + u0) / 8.f) * 2.f * M_PI_F - M_PI_F;
	const float c = (((float)ti + u1) / 4.f) * 2.f - 1.f;
	const float s = sqrt(max(0.f, 1.f - c * c));
	// NOTE: MAKE_FLOAT3 (not raw (float3)(...) which this Metal backend
	// lowers as a comma expression + scalar splat, nor bare float3(...)
	// which Apple OpenCL rejects) — valid on both backends.
	return MAKE_FLOAT3(s * cos(phi), s * sin(phi), c);
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

// M2c: adaptive mixture weight from the (frozen-in-round) cell total.
// Mirrors PathGuidingCache::MixWeight on the CPU.
OPENCL_FORCE_INLINE float Guide_MixWeight(float total) {
	return min(total / (total + 1024.f), .75f);
}

OPENCL_FORCE_INLINE bool Guide_Sample(__global const float *tb, uint localCell,
		float3 n, float uBin, float uDir0, float uDir1,
		float3 *sampledDir, float *pdfW) {
	__global const float *cb = tb + localCell * 33u;
	float total = cb[32];
	if (!(total > 0.f))
		return false;

	// M2c smoothing (mirrors the CPU GuideWeights): beta flattens
	// noise-dominated bins toward uniform within the valid hemisphere.
	const float beta = .1f * total / 32.f;
	float weights[32];
	float wSum = 0.f;
	for (uint i = 0u; i < GUIDE_DIR_BINS; ++i) {
		const uint pi = i % 8u;
		const uint ti = i / 8u;
		const float phi = (((float)pi + .5f) / 8.f) * 2.f * M_PI_F - M_PI_F;
		const float c = (((float)ti + .5f) / 4.f) * 2.f - 1.f;
		const float s = sqrt(max(0.f, 1.f - c * c));
		const float cosB = s * cos(phi) * n.x + s * sin(phi) * n.y + c * n.z;
		weights[i] = (cosB > 0.f) ? cb[i] * cosB + beta : 0.f;
		wSum += weights[i];
	}
	if (!(wSum > 0.f))
		return Guide_CosineSample(n, uDir0, uDir1, sampledDir, pdfW);

	float pick = uBin * wSum;
	uint bin = 0u;
	for (; bin < GUIDE_DIR_BINS - 1u; ++bin) {
		pick -= weights[bin];
		if (pick <= 0.f)
			break;
	}
	if (!(weights[bin] > 0.f))
		return Guide_CosineSample(n, uDir0, uDir1, sampledDir, pdfW);

	*sampledDir = Guide_BinDir(bin, uDir0, uDir1);
	*pdfW = (weights[bin] / wSum) / GUIDE_BIN_OMEGA;
	return true;
}

OPENCL_FORCE_INLINE float Guide_Pdf(__global const float *tb, uint localCell,
		float3 n, float3 dir) {
	__global const float *cb = tb + localCell * 33u;
	float total = cb[32];
	if (!(total > 0.f))
		return 0.f;

	const float beta = .1f * total / 32.f;
	float wSum = 0.f;
	for (uint i = 0u; i < GUIDE_DIR_BINS; ++i) {
		const uint pi = i % 8u;
		const uint ti = i / 8u;
		const float phi = (((float)pi + .5f) / 8.f) * 2.f * M_PI_F - M_PI_F;
		const float c = (((float)ti + .5f) / 4.f) * 2.f - 1.f;
		const float s = sqrt(max(0.f, 1.f - c * c));
		const float cosB = s * cos(phi) * n.x + s * sin(phi) * n.y + c * n.z;
		wSum += (cosB > 0.f) ? cb[i] * cosB + beta : 0.f;
	}
	if (!(wSum > 0.f))
		return 0.f;

	const uint bin = Guide_DirBin(dir);
	const uint pi = bin % 8u;
	const uint ti = bin / 8u;
	const float phi = (((float)pi + .5f) / 8.f) * 2.f * M_PI_F - M_PI_F;
	const float c = (((float)ti + .5f) / 4.f) * 2.f - 1.f;
	const float s = sqrt(max(0.f, 1.f - c * c));
	const float cosB = s * cos(phi) * n.x + s * sin(phi) * n.y + c * n.z;
	const float w = (cosB > 0.f) ? cb[bin] * cosB + beta : 0.f;
	return (w / wSum) / GUIDE_BIN_OMEGA;
}

// Per-sample pass for the guide bin-pick hash (mirrors Sampler::GetPass):
// RANDOM/SOBOL/PMJ02 share the leading (bucketIndex, pixelOffset,
// passOffset, pass) per-work-item layout; TilePath has its own.
// Record-buffer select (M2b-2): task t writes buffer (t&15),
// slot (t>>5)&255 (no atomics; overwrites on collision are valid data)
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
	if (taskConfig->sampler.type == TILEPATHSAMPLER)
		return ((__global TilePathSample *)samplesBuff)[gid].pass;
	return ((__global RandomSample *)samplesBuff)[gid].pass;
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
		__global const float* restrict guideChunk0,
		__global const float* restrict guideChunk1,
		__global const float* restrict guideChunk2,
		__global const float* restrict guideChunk3,
		__global const float* restrict guideChunk4,
		__global const float* restrict guideChunk5,
		__global const float* restrict guideChunk6,
		__global const float* restrict guideChunk7,
		__global const float* restrict guideChunk8,
		__global const float* restrict guideChunk9,
		__global const float* restrict guideChunk10,
		__global const float* restrict guideChunk11,
		__global const float* restrict guideChunk12,
		__global const float* restrict guideChunk13,
		__global const float* restrict guideChunk14,
		__global const float* restrict guideChunk15,
		const uint guidingEnable,
		const float guideCubeMinX,
		const float guideCubeMinY,
		const float guideCubeMinZ,
		const float guideCubeSize) {
	// Sample the BSDF
	BSDFEvent event;
	float bsdfPdfW;
	const float3 bsdfEval = BSDF_Evaluate(bsdf,
			shadowRayDir, &event, &bsdfPdfW
			MATERIALS_PARAM);

	if (Spectrum_IsBlack(bsdfEval) ||
			(taskConfig->pathTracer.hybridBackForward.enabled &&
			EyePathInfo_IsCausticPathWithEvent(pathInfo, event,
				BSDF_GetGlossiness(bsdf MATERIALS_PARAM),
				taskConfig->pathTracer.hybridBackForward.glossinessThreshold))
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

	// Path guiding (P1-3 M2b): same mixture competitor as the CPU side
	// (see PathTracer::DirectLightSampling)
	float bouncePdfW = bsdfPdfW;
	{
		const BSDFEvent eventTypes = BSDF_GetEventTypes(bsdf MATERIALS_PARAM);
		const float invGuideSize = 1.f / guideCubeSize;
		const uint guideCell = Guide_CellIndex(
				VLOAD3F(&bsdf->hitPoint.p.x),
				guideCubeMinX, guideCubeMinY, guideCubeMinZ, invGuideSize);
		__global const float *guideTb = Guide_Chunk(guideCell >> 5,
				guideChunk0, guideChunk1, guideChunk2, guideChunk3,
				guideChunk4, guideChunk5, guideChunk6, guideChunk7,
				guideChunk8, guideChunk9, guideChunk10, guideChunk11,
				guideChunk12, guideChunk13, guideChunk14, guideChunk15);
		const uint guideLocal = guideCell & 31u;
		if ((guidingEnable != 0u) && !BSDF_IsDelta(bsdf MATERIALS_PARAM) &&
				((eventTypes & GLOSSY) != 0u) &&
				(BSDF_GetGlossiness(bsdf MATERIALS_PARAM) >= .3f) &&
				(pathInfo->depth.depth >= 2u) &&
				(guideTb[guideLocal * 33u + 32u] >= GUIDE_WARMUP_RECORDS)) {
			const float3 shadeN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
			const float wDl = Guide_MixWeight(guideTb[guideLocal * 33u + 32u]);
			bouncePdfW = (1.f - wDl) * bsdfPdfW + wDl * Guide_Pdf(guideTb, guideLocal,
					shadeN, shadowRayDir);
		}
	}

	// Russian Roulette
	bouncePdfW *= (PathDepthInfo_GetRRDepth(tmpDepthInfo) >= taskConfig->pathTracer.rrDepth) ?
		RussianRouletteProb(taskConfig->pathTracer.rrImportanceCap, bsdfEval) :
		1.f;

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

	const float weight = misEnabled ? PowerHeuristic(directLightSamplingPdfW, bouncePdfW) : 1.f;

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
OPENCL_FORCE_INLINE bool Mnee_Residual(const float3 x0p, const float3 lightPos,
		const MneeVtx *v, float2 *C) {
	*C = MAKE_FLOAT2(0.f, 0.f);

	float3 wi = x0p - v->p;
	const float r01 = length(wi);
	if (r01 < 1e-3f)
		return false;
	wi /= r01;

	float3 wo = lightPos - v->p;
	const float r12 = length(wo);
	if (r12 < 1e-3f)
		return false;
	wo /= r12;

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
		const MneeVtx *v, float4 *j1Out) {
	if (j1Out)
		*j1Out = MAKE_FLOAT4(0.f, 0.f, 0.f, 0.f);

	float3 wi = x0p - v->p;
	const float r01 = length(wi);
	if (r01 < 1e-3f)
		return 0.f;
	wi /= r01;

	float3 wo = lightPos - v->p;
	const float r12 = length(wo);
	if (r12 < 1e-3f)
		return 0.f;
	wo /= r12;

	float eta = v->eta;
	if (dot(wi, v->gn) < 0.f)
		eta = 1.f / eta;
	float3 h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	const float ilh = 1.f / length(h);
	h *= ilh;
	const float ilo = (1.f / r12) * eta * ilh;
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

	// dC/dx2 (fake light frame, from the light toward the vertex)
	float3 dLight = v->p - lightPos;
	const float rl = length(dLight);
	if (rl < 1e-3f)
		return 0.f;
	dLight /= rl;
	float3 s2, t2;
	Mnee_CoordinateSystem(dLight, &s2, &t2);
	float3 dhDdu2 = ilo * (s2 - wo * dot(wo, s2));
	float3 dhDdv2 = ilo * (t2 - wo * dot(wo, t2));
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
	return GlassMaterial_EvalSpecularTransmission(hitPoint, localFixedDir, 0.f,
			kt, nc, nt, 0.f, &localSampledDir);
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

	float2 C;
	if (!Mnee_Residual(x0p, lightPos, v, &C))
		return 0;
	const float resNorm = length(C);
	mnee->resNorm = resNorm;

	float4 jac;
	const float g = Mnee_GeometricTerm(x0p, lightPos, v, &jac);
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
		__global const RayHit *rayHit, __global Ray *ray
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
	// delta light, delta specular occluder, mirror or glass material.
	if ((light->type != TYPE_POINT) && (light->type != TYPE_SPOT) &&
			(light->type != TYPE_MAPPOINT))
		return 0;
	if (BSDF_IsShadowCatcher(&taskState->bsdf MATERIALS_PARAM))
		return 0;
	if (occlBsdf->isVolume || !occlMat->isDelta || !(occlMat->eventTypes & SPECULAR))
		return 0;
	if ((occlMat->type != MIRROR) && (occlMat->type != GLASS))
		return 0;

	// Light position: the shadow ray maxt has been rewritten by the trace to
	// the occluder distance, so recover the light distance from directPdfW
	// (= squared distance to the light, preserved by Illuminate).
	const float r12 = sqrt(info->directPdfW);
	if (!isfinite(r12) || (r12 < 1e-3f))
		return 0;
	const float3 lightPos = VLOAD3F(&ray->o.x) + VLOAD3F(&ray->d.x) * r12;

	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	// Generalized half-vector IOR ratio of the occluder (CPU MNEEDirectSampling)
	float etaVertex;
	if (occlMat->type == MIRROR) {
		const float3 gn1s = VLOAD3F(&occlBsdf->hitPoint.geometryN.x);
		const float3 x1p = VLOAD3F(&occlBsdf->hitPoint.p.x);
		const float3 toX0 = x0p - x1p;
		const float3 toY = lightPos - x1p;
		// +1 when the two endpoints are on the same side of the surface
		// (h = wi + wo), the only physical reflection case: the reflected ray
		// leaves with the normal component of its direction flipped, so the
		// receiver is on the same side of the tangent plane as the light. The
		// opposite-side case would need the surface to transmit, which a
		// mirror does not do (matches Mnee_Start in the CPU solver; see
		// dev-tools/sota_p1_mnee_mirror_physics_test.py).
		etaVertex = (dot(toX0, gn1s) * dot(toY, gn1s) > 0.f) ? 1.f : -1.f;
		if (etaVertex != 1.f)
			return 2;
	} else {
		// Dispersive glass: the manifold uses a single IOR ratio, skip
		if ((occlMat->glass.cauchyBTex != NULL_INDEX) &&
				(Texture_GetFloatValue(occlMat->glass.cauchyBTex,
					&occlBsdf->hitPoint TEXTURES_PARAM) > 0.f))
			return 0;

		const float nc = ExtractExteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
		const float nt = ExtractInteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.interiorIorTexIndex TEXTURES_PARAM);
		if ((nt <= 0.f) || (nc <= 0.f))
			return 0;
		etaVertex = nt / nc;
	}

	mnee->mirrorMode = (occlMat->type == MIRROR);
	mnee->chainN = 0;
	mnee->lightPosX = lightPos.x;
	mnee->lightPosY = lightPos.y;
	mnee->lightPosZ = lightPos.z;
	mnee->shadowMeshIndex = rayHit->meshIndex;
	mnee->beta = 1.f;
	mnee->iteration = 0;
	mnee->needsTrace = false;

	MneeVtx v;
	Mnee_InitVtxFromBsdf(occlBsdf, etaVertex, &v);
	Mnee_StoreVtx(mnee, &v);
	// The current chain vertex BSDF starts as the shadow-ray occluder
	taskDirectLight->mneeBsdfFinal = task->tmpBsdf;

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
		const float3 x1Line = VLOAD3F(&occlBsdf->hitPoint.p.x);
		const float3 x1ToLight = lightPos - x1Line;
		const float proj = 2.f * dot(x1ToLight, gn1);
		const float3 mirroredLight = lightPos - proj * gn1;
		const float3 dSeed = normalize(mirroredLight - x0p);
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
OPENCL_FORCE_INLINE bool MneeChain_ResidualAt(const float3 pPrev, const float3 pNext,
		const MneeVtx *v, const float etaVertex, float2 *C) {
	float3 wi = pPrev - v->p;
	const float r0 = length(wi);
	if (r0 < 1e-4f)
		return false;
	wi /= r0;

	float3 wo = pNext - v->p;
	const float r1 = length(wo);
	if (r1 < 1e-4f)
		return false;
	wo /= r1;

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
		float2 C;
		if (!MneeChain_ResidualAt(pPrev, pNext, &v, v.eta, &C))
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

// dC_last/dy (CPU MneeChainLightJacobian port).
OPENCL_FORCE_INLINE float4 MneeChain_LightJac(const float3 pPrev, const float3 lightPos,
		const MneeVtx *v, const float etaVertex, const float eps) {
	const float4 zero = MAKE_FLOAT4(0.f, 0.f, 0.f, 0.f);

	float3 dLight = v->p - lightPos;
	const float rl = length(dLight);
	if (rl < 1e-3f)
		return zero;
	dLight /= rl;
	float3 s2, t2;
	Mnee_CoordinateSystem(dLight, &s2, &t2);

	const float3 wi = normalize(pPrev - v->p);
	float eta = etaVertex;
	if (dot(wi, v->gn) < 0.f)
		eta = 1.f / eta;

	float3 h = wi + eta * normalize(lightPos - v->p);
	if (eta != 1.f)
		h = -h;
	const float l = length(h);
	if (l < 1e-6f)
		return zero;
	h *= 1.f / l;
	const float C0x = dot(v->s, h), C0y = dot(v->t, h);

	float CPx[2], CPy[2];
	for (int k = 0; k < 2; ++k) {
		const float3 lightPosP = lightPos + ((k == 0) ? eps * s2 : eps * t2);
		float3 hP = wi + eta * normalize(lightPosP - v->p);
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

// Shared chain initializer. Returns the capped vertex budget.
OPENCL_FORCE_INLINE int MneeChain_Begin(__global MneeState *mnee, const unsigned int maxSpecular) {
	mnee->chainN = 0;
	mnee->chainMaxV = (maxSpecular < MNEE_MS_MAX_VERTICES) ? (int)maxSpecular : MNEE_MS_MAX_VERTICES;
	mnee->chainIdx = 0;
	mnee->chainSub = 0;
	mnee->chainProjected = 1;
	mnee->chainSpecR = 1.f; mnee->chainSpecG = 1.f; mnee->chainSpecB = 1.f;
	mnee->beta = 1.f;
	mnee->iteration = 0;
	return mnee->chainMaxV;
}

// Initialize chain vertex 0 from the shadow-ray occluder BSDF (CPU
// MneeChainVertexInit port). The caller guarantees a delta specular
// mirror/glass hit. Returns the eta, or -1.f when the vertex is unusable
// (dispersive glass: the CPU discovery ends the chain there too).
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
		if ((occlMat->glass.cauchyBTex != NULL_INDEX) &&
				(Texture_GetFloatValue(occlMat->glass.cauchyBTex,
					&occlBsdf->hitPoint TEXTURES_PARAM) > 0.f))
			return -1.f;
		const float nc = ExtractExteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
		const float nt = ExtractInteriorIors(&occlBsdf->hitPoint,
				occlMat->glass.interiorIorTexIndex TEXTURES_PARAM);
		if ((nt <= 0.f) || (nc <= 0.f))
			return -1.f;
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

	if ((light->type != TYPE_POINT) && (light->type != TYPE_SPOT) &&
			(light->type != TYPE_MAPPOINT))
		return false;
	if (BSDF_IsShadowCatcher(&taskState->bsdf MATERIALS_PARAM))
		return false;
	if (occlBsdf->isVolume || !occlMat->isDelta || !(occlMat->eventTypes & SPECULAR))
		return false;
	if ((occlMat->type != MIRROR) && (occlMat->type != GLASS))
		return false;

	const float r12 = sqrt(info->directPdfW);
	if (!isfinite(r12) || (r12 < 1e-3f))
		return false;
	const float3 lightPos = VLOAD3F(&ray->o.x) + VLOAD3F(&ray->d.x) * r12;
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);

	__global MneeState *mnee = &taskDirectLight->mnee;
	mnee->lightPosX = lightPos.x;
	mnee->lightPosY = lightPos.y;
	mnee->lightPosZ = lightPos.z;
	MneeChain_Begin(mnee, taskConfig->pathTracer.mnee.maxSpecular);

	// Vertex 0 is the shadow-ray occluder itself (CPU MneeChainDiscover
	// takes firstBsdf as given; re-tracing it with a different ray type
	// lands microscopically elsewhere and the Newton starts off-solution).
	if (MneeChain_InitVtxZero(mnee, occlBsdf, occlMat MATERIALS_PARAM) < 0.f)
		return false;

	// Discover from vertex 1 on along the straight ray.
	const float3 dir = normalize(lightPos - x0p);
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
	if (MneeChain_InitVtxZero(mnee, occlBsdf, occlMat MATERIALS_PARAM) < 0.f)
		return false;

	const float3 lightPos = MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY, mnee->lightPosZ);
	const float3 x0p = VLOAD3F(&taskState->bsdf.hitPoint.p.x);
	const float3 dir = normalize(lightPos - x0p);
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
		__global SampleResult *sampleResult
		LIGHTS_PARAM_DECL
		) {
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
			&task->tmpHitPoint, rayOut, &directPdfW2
			LIGHTS_PARAM);

	if (Spectrum_IsBlack(lightRadiance2) || !isfinite(directPdfW2))
		return false;

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
		__global EyePathInfo *pathInfo) {
	if (mnee->iteration >= maxIterations)
		return false;
	float maxRes;
	if (!MneeChain_ResidualsAll(mnee, x0p, lightPos, &maxRes))
		return false;
	mnee->resNorm = maxRes;
	MneeChain_ZeroJacobian(mnee);
	MneeChain_WritePerturbRay(mnee, x0p, 0, 0, ray, ray->time);
	*dlVolInfo = pathInfo->volume;
	mnee->chainIdx = 0;
	mnee->chainSub = 0;
	mnee->phase = MNEE_PHASE_MS_JACPERT;
	return true;
}

// Trial pass opener (accept and retry share it; beta is already updated).
OPENCL_FORCE_INLINE void MneeChain_WriteTrial0(
		__global MneeState *mnee, const float3 x0p,
		__global Ray *ray, __global PathVolumeInfo *dlVolInfo,
		__global EyePathInfo *pathInfo) {
	const float3 pProp = MneeChain_TrialPos(mnee, 0);
	MneeVtx v;
	MneeChain_LoadVtx(mnee, 0, &v);
	MneeChain_WriteReprojectRay(pProp, v.gn,
			.5f * MneeChain_TrialEps(x0p, v.p), ray, ray->time);
	*dlVolInfo = pathInfo->volume;
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
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, pathInfo))
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
			if ((hitMat->glass.cauchyBTex != NULL_INDEX) &&
					(Texture_GetFloatValue(hitMat->glass.cauchyBTex,
						&taskDirectLight->mneeBsdf.hitPoint TEXTURES_PARAM) > 0.f))
				vertexOk = false;
			else {
				const float nc = ExtractExteriorIors(&taskDirectLight->mneeBsdf.hitPoint,
						hitMat->glass.exteriorIorTexIndex TEXTURES_PARAM);
				const float nt = ExtractInteriorIors(&taskDirectLight->mneeBsdf.hitPoint,
						hitMat->glass.interiorIorTexIndex TEXTURES_PARAM);
				if ((nt <= 0.f) || (nc <= 0.f))
					vertexOk = false;
				else
					etaVertex = nt / nc;
			}
		} else
			vertexOk = false;

		if (!vertexOk) {
			// A non specular surface ends the chain (CPU: break).
			if (mnee->chainN >= 2) {
				if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, pathInfo))
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
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, pathInfo))
				Mnee_ExitTransition(taskState, sampleResult);
			return;
		}

		const float3 dir = normalize(lightPos - x0p);
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
			if (!MneeChain_ResidualAt(pPrevJJ, pNextJJ, &vjj, vjj.eta, &CP)) {
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
					&vlast, vlast.eta, epsLight);
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
		MneeChain_WriteTrial0(mnee, x0p, ray, dlVolInfo, pathInfo);
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
				float2 CT;
				if (!MneeChain_ResidualAt(pPrev, pNext, &trialV, trialV.eta, &CT))
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
			mnee->beta = fmin(1.f, 2.f * mnee->beta);
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
		MneeChain_WriteTrial0(mnee, x0p, ray, dlVolInfo, pathInfo);
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
		if (mnee->iteration >= taskConfig->pathTracer.mnee.maxIterations) {
			Mnee_ExitTransition(taskState, sampleResult);
			return;
		}
		if (!MneeChain_WriteJacStart(mnee, x0p, lightPos,
					taskConfig->pathTracer.mnee.maxIterations, ray, dlVolInfo, pathInfo))
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
		const float3 wok = normalize(pNextK - vk.p);
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
		__global SampleResult *sampleResult
		LIGHTS_PARAM_DECL
		) {
	// Post-solve validity check (Zeltner newton_solver tail): the
	// half-vector formulation can converge to a solution of the wrong
	// specular mode
	MneeVtx v;
	Mnee_LoadVtx(mnee, &v);
	const float3 wi = normalize(x0p - v.p);
	const float3 wo = normalize(MAKE_FLOAT3(mnee->lightPosX, mnee->lightPosY,
				mnee->lightPosZ) - v.p);
	const float cosX = dot(v.gn, wi);
	const float cosY = dot(v.gn, wo);
	const bool refraction = (cosX * cosY < 0.f);
	if (mnee->mirrorMode) {
		// Mirror: only a same-side reflection is physical (see the etaVertex
		// gate in Mnee_Start). Reject solutions with the opposite side
		// relation - the half-vector formulation can converge to such a mode,
		// and accepting it produced light where none exists.
		if (refraction) {
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
			LIGHTS_PARAM);
			return;
		}
	} else if (!refraction) {
		// Glass: only refraction solutions are supported (Zeltner SS handles
		// dielectric transmission; external dielectric reflection is out of
		// scope)
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
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
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
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
			&task->tmpHitPoint, ray, &directPdfW2
			LIGHTS_PARAM);

	if (Spectrum_IsBlack(lightRadiance2) || !isfinite(directPdfW2)) {
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
			LIGHTS_PARAM);
		return;
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
		const float worldCenterZ, const float worldRadius
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
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
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
				ray, dlVolInfo, sampleResult
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
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
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
				ray, dlVolInfo, sampleResult
				LIGHTS_PARAM);
		return;
	}

	if (mnee->phase == MNEE_PHASE_PROP_TRACE) {
		if (rayHit->meshIndex == NULL_INDEX) {
			// The proposal ray missed everything: the CPU line search
			// breaks out of the Newton loop here (solve failed).
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
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
			if (!Mnee_Residual(x0p, lightPos, &vProp, &CProp)) {
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
						Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
			LIGHTS_PARAM);
						return;
					}
					if (resPropNorm < 3e-4f) {
						// Converged: geometric term at the converged vertex
						converged = true;
						gConverged = Mnee_GeometricTerm(x0p, lightPos, &vProp,
								NULL);
					}
				} else
					stepRejected = true;
			}
		}

		if (converged) {
			Mnee_SolveEnd(taskConfig, task, taskDirectLight, taskState, pathInfo,
					mnee, x0p, gConverged, taskGid,
					worldCenterX, worldCenterY, worldCenterZ, worldRadius,
					ray, dlVolInfo, sampleResult
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
			Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
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
				ray, dlVolInfo, sampleResult
				LIGHTS_PARAM);
		return;
	}

	// MNEE_PHASE_SEG2_TRACE: the chain is valid only if y is directly
	// visible from x1
	if (rayHit->meshIndex != NULL_INDEX) {
		Mnee_FailToChainOrExit(taskConfig, task, taskDirectLight, taskState, ray, dlVolInfo, pathInfo, sampleResult
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
		const float weightScale = (mnee->plainHalfVector ? mnee->directPdfW2 : 1.f) *
				taskDirectLight->illumInfo.risScale /
				taskDirectLight->illumInfo.pickPdf;
		const float3 incomingRadiance = bsdfEval0 * specFactor *
				(mnee->geometricTerm * weightScale) * lightRadiance2 *
				connectionThroughput;

		SampleResult_AddDirectLight(&taskConfig->film, sampleResult,
				taskDirectLight->illumInfo.lightID,
				(BSDFEvent)mnee->specEvent,
				VLOAD3F(taskState->throughput.c), incomingRadiance, 1.f);
	}

	Mnee_ExitTransition(taskState, sampleResult);
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
		/* Path guiding (P1-3 M2b): 16 frozen coarse-table chunks
		 * (4224B each) + field bounds + enable */ \
		, __global const float* restrict guideChunk0 \
		, __global const float* restrict guideChunk1 \
		, __global const float* restrict guideChunk2 \
		, __global const float* restrict guideChunk3 \
		, __global const float* restrict guideChunk4 \
		, __global const float* restrict guideChunk5 \
		, __global const float* restrict guideChunk6 \
		, __global const float* restrict guideChunk7 \
		, __global const float* restrict guideChunk8 \
		, __global const float* restrict guideChunk9 \
		, __global const float* restrict guideChunk10 \
		, __global const float* restrict guideChunk11 \
		, __global const float* restrict guideChunk12 \
		, __global const float* restrict guideChunk13 \
		, __global const float* restrict guideChunk14 \
		, __global const float* restrict guideChunk15 \
		, const uint guidingEnable \
		, const float guideCubeMinX \
		, const float guideCubeMinY \
		, const float guideCubeMinZ \
		, const float guideCubeSize \
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
		, const uint wavefrontEnable

// Wavefront lane -> task index mapping. Under wavefrontEnable, lane
// gid indexes this kernel's state queue; otherwise the dense mapping
// (gid == task index) is used. Task data keeps being indexed by the
// returned value, so downstream code is unchanged.
#define WAVEFRONT_GID \
	(wavefrontEnable ? \
		taskQueueBuf[taskQueueState * taskQueueStride + get_global_id(0)] : \
		get_global_id(0))

// Wavefront lane bounds check + gid mapping. Per-state launches are
// rounded up to the workgroup size (OpenCL requires global size to be
// a multiple of it); lanes beyond the compacted queue length exit
// before dereferencing the queue, whose tail slots hold stale task
// indices from the previous iteration. The state launch covers the
// sum of the per-(state, lambda) histogram counters (M2 lambda
// bucketing keeps the queue layout flat). Must be the first statement
// of every AdvancePaths_MK_* kernel.
#define WAVEFRONT_GUARD \
	if (wavefrontEnable && \
			get_global_id(0) >= \
				taskQueueCount[taskQueueState * SLG_SPECTRAL_BINS] + \
				taskQueueCount[taskQueueState * SLG_SPECTRAL_BINS + 1] + \
				taskQueueCount[taskQueueState * SLG_SPECTRAL_BINS + 2]) \
		return; \
	const size_t gid = WAVEFRONT_GID;


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

	__global GPUTaskState *taskState = &tasksState[gid];

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
