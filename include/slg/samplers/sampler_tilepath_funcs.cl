#line 2 "sampler_tilepath_funcs.cl"

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
// TilePath Sampler Kernel
//------------------------------------------------------------------------------

#define TILEPATHSAMPLER_TOTAL_U_SIZE 2

OPENCL_FORCE_INLINE __global const uint* restrict TilePathSampler_GetSobolDirectionsPtr(
		__global TilePathSamplerSharedData *samplerSharedData) {
	// Sobol directions array is appended at the end of slg::ocl::TilePathSamplerSharedData
	return (__global uint *)(
			(__global char *)samplerSharedData +
			sizeof(TilePathSamplerSharedData));
}

OPENCL_FORCE_INLINE float TilePathSampler_GetSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const uint index
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)
	__global float *samplesData = &samplesDataBuff[gid * TILEPATHSAMPLER_TOTAL_U_SIZE];

	switch (index) {
		case IDX_SCREEN_X:
			return samplesData[IDX_SCREEN_X];
		case IDX_SCREEN_Y:
			return samplesData[IDX_SCREEN_Y];
		default: {
#if defined(RENDER_ENGINE_RTPATHOCL)
			return Rnd_FloatValue(seed);
#else
			__global TilePathSamplerSharedData *samplerSharedData = (__global TilePathSamplerSharedData *)samplerSharedDataBuff;
			__global const uint* restrict sobolDirections = TilePathSampler_GetSobolDirectionsPtr(samplerSharedData);

			__global TilePathSample *samples = (__global TilePathSample *)samplesBuff;
			__global TilePathSample *sample = &samples[gid];

			return SobolSequence_GetSample(sobolDirections, sample->pass + SOBOL_STARTOFFSET,
					sample->rngPass, sample->rng0, sample->rng1, index, false, false);
#endif
		}
	}
}

OPENCL_FORCE_INLINE void TilePathSampler_SplatSample(
		__constant const GPUTaskConfiguration* restrict taskConfig
		SAMPLER_PARAM_DECL
		FILM_PARAM_DECL
		) {
	// gid: task index supplied by the caller (wavefront-safe)
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	Film_AddSample(sampleResult->pixelX, sampleResult->pixelY,
			sampleResult, 1.f
			FILM_PARAM);
}

OPENCL_FORCE_INLINE void TilePathSampler_NextSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	// TilePathSampler_NextSample() is not used in TILEPATHSAMPLER
}

OPENCL_FORCE_INLINE bool TilePathSampler_Init(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)
	__global TilePathSamplerSharedData *samplerSharedData = (__global TilePathSamplerSharedData *)samplerSharedDataBuff;
	__global TilePathSample *samples = (__global TilePathSample *)samplesBuff;
	__global TilePathSample *sample = &samples[gid];
	__global float *samplesData = &samplesDataBuff[gid * TILEPATHSAMPLER_TOTAL_U_SIZE];

#if defined(RENDER_ENGINE_RTPATHOCL)
	// 1 thread for each pixel

	// Viewport progressive coverage: each pass walks a rank-1 lattice
	// over the whole tile instead of a stride-R grid refined by Morton
	// order inside R*R cells. The old scheme left R*R stale blocks on
	// screen until coverage completed (~R^2 passes); the lattice lands
	// pixelCount/R^2 maximally-scattered real samples per pass, so the
	// display-side VIEWPORT_INFILL reconstructs a coherent image that
	// dissolves in instead of forming block ghosts.
	const uint pixelCount = samplerSharedData->tileWidth * samplerSharedData->tileHeight;
	const uint pass = samplerSharedData->tilePass;
	const uint step = taskConfig->renderEngine.rtpathocl.previewResolutionReductionStep;
	const uint previewRR = max(1u, taskConfig->renderEngine.rtpathocl.previewResolutionReduction);
	const uint steadyRR = max(1u, taskConfig->renderEngine.rtpathocl.resolutionReduction);
	const uint rr = (pass < step) ? previewRR : steadyRR;
	const uint activeCount = max(1u, pixelCount / (rr * rr));
	if (gid >= activeCount)
		return false;

	// Cumulative lattice index across variable-rate passes: the preview
	// phase runs previewRR^2-sparse for "step" passes, then the steady
	// sequence continues where it left off.
	const uint previewActive = max(1u, pixelCount / (previewRR * previewRR));
	const uint steadyActive = max(1u, pixelCount / (steadyRR * steadyRR));
	const ulong seqIdx = (ulong)min(pass, step) * previewActive +
			(pass > step ? (ulong)(pass - step) * steadyActive : 0ul) + gid;
	const uint i = (uint)(seqIdx % pixelCount);
	const uint epoch = (uint)(seqIdx / pixelCount);
	// Knuth golden-ratio multiplier: the lattice covers a single residue
	// coset when gcd(A, pixelCount) > 1, so the epoch offset shifts the
	// coset once per full cycle - every pixel is reached within gcd
	// cycles. 64-bit math keeps i*A exact.
	uint pix = (uint)(((ulong)i * 0x9E3779B1ul + epoch) % pixelCount);

	// Viewport adaptive sampling: walk the lattice forward while the
	// picked pixel looks converged. Same acceptance semantics as the
	// sobol adaptive scheme - noisy pixels are picked with probability
	// ~1, converged ones with floor (1 - adaptiveStrength) - so the
	// unbiased every-pixel-coverage property is preserved while compute
	// concentrates on variance (glass, caustics, glossy).
	__constant const Sampler *sampler = &taskConfig->sampler;
	const float adaptiveStrength = sampler->tilepath.adaptiveStrength;
	if (filmNoise && (adaptiveStrength > 0.f)) {
		uint px = pix % samplerSharedData->tileWidth;
		uint py = pix / samplerSharedData->tileWidth;
		for (uint tries = 0; tries < 8; ++tries) {
			const uint fx = samplerSharedData->tileStartX + px;
			const uint fy = samplerSharedData->tileStartY + py;
			float noise = filmNoise[fx + fy * filmWidth];
			float threshold = isinf(noise) ? 1.f : noise;
			if (filmUserImportance) {
				const float ui = filmUserImportance[fx + fy * filmWidth];
				threshold = (ui > 0.f) ?
						mix(threshold, ui, sampler->tilepath.adaptiveUserImportanceWeight) : 0.f;
			}
			threshold = fmax(threshold, 1.f - adaptiveStrength);
			if (Rnd_FloatValue(seed) <= threshold)
				break;
			// Step to the next lattice element (scattered position)
			pix = (pix + 0x9E3779B1u) % pixelCount;
			px = pix % samplerSharedData->tileWidth;
			py = pix / samplerSharedData->tileWidth;
		}
	}

	const uint pixelX = pix % samplerSharedData->tileWidth;
	const uint pixelY = pix / samplerSharedData->tileWidth;

	sample->pass = samplerSharedData->tilePass;

	samplesData[IDX_SCREEN_X] = pixelX + Rnd_FloatValue(seed);
	samplesData[IDX_SCREEN_Y] = pixelY + Rnd_FloatValue(seed);
#else
	// aaSamples * aaSamples threads for each pixel

	const uint aaSamples2 = Sqr(samplerSharedData->aaSamples);

	if (gid >= filmWidth * filmHeight * aaSamples2)
		return false;

	const uint pixelIndex = gid / aaSamples2;

	const uint pixelX = pixelIndex % filmWidth;
	const uint pixelY = pixelIndex / filmWidth;
	if ((pixelX >= samplerSharedData->tileWidth) || (pixelY >= samplerSharedData->tileHeight))
		return false;

	// Initialize rng0, rng1 and rngPass
	const uint pixelRngGenSeed = (samplerSharedData->tileStartX + pixelX + (samplerSharedData->tileStartY + pixelY) * samplerSharedData->cameraFilmWidth + 1) *
			(samplerSharedData->multipassIndexToRender + 1);
	Seed pixelSeed;
	Rnd_Init(pixelRngGenSeed, &pixelSeed);

	sample->rngPass = Rnd_UintValue(&pixelSeed);
	sample->rng0 = Rnd_FloatValue(&pixelSeed);
	sample->rng1 = Rnd_FloatValue(&pixelSeed);

	sample->pass = samplerSharedData->tilePass * aaSamples2 + gid % aaSamples2;

	__global const uint* restrict sobolDirections = TilePathSampler_GetSobolDirectionsPtr(samplerSharedData);

	samplesData[IDX_SCREEN_X] = pixelX + SobolSequence_GetSample(sobolDirections, sample->pass + SOBOL_STARTOFFSET,
			sample->rngPass, sample->rng0, sample->rng1, IDX_SCREEN_X, false, false);
	samplesData[IDX_SCREEN_Y] = pixelY + SobolSequence_GetSample(sobolDirections, sample->pass + SOBOL_STARTOFFSET,
			sample->rngPass, sample->rng0, sample->rng1, IDX_SCREEN_Y, false, false);
#endif

	return true;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
