#line 2 "sampler_pmj02_funcs.cl"

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
// PMJ02 Sampler Kernel
//
// Progressive multi-jittered (0,2) sequences (Christensen, Kensler and
// Kilpatrick 2018). The 2D tables are generated on the host at init
// (pmj-cpp generator) and uploaded in the sampler shared data buffer after
// the per-pixel pass array (same header layout as SobolSamplerSharedData).
// Every 2D dimension pair consumes consecutive table points; each point gets
// a per-pixel Cranley-Patterson rotation, rotated again per wrap cycle, so
// any spp is unbiased. The bucket cursor mechanics are the Sobol ones.
//------------------------------------------------------------------------------

#define PMJ02SAMPLER_TOTAL_U_SIZE 2

OPENCL_FORCE_INLINE __global const float *PMJ02Sampler_GetTablesPtr(__global SobolSamplerSharedData *samplerSharedData) {
	// PMJ02 tables are appended at the end of the shared header + all pass values
	return (__global const float *)(
			(__global char *)samplerSharedData +
			sizeof(SobolSamplerSharedData) +
			sizeof(uint) * samplerSharedData->filmRegionPixelCount);
}

OPENCL_FORCE_INLINE float PMJ02Sampler_GetSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const uint index
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)

	switch (index) {
		case IDX_SCREEN_X: {
			__global float *samplesData = &samplesDataBuff[gid * PMJ02SAMPLER_TOTAL_U_SIZE];
			return samplesData[IDX_SCREEN_X];
		}
		case IDX_SCREEN_Y: {
			__global float *samplesData = &samplesDataBuff[gid * PMJ02SAMPLER_TOTAL_U_SIZE];
			return samplesData[IDX_SCREEN_Y];
		}
		default: {
			__global SobolSamplerSharedData *samplerSharedData = (__global SobolSamplerSharedData *)samplerSharedDataBuff;
			__global const float *pmjTables = PMJ02Sampler_GetTablesPtr(samplerSharedData);

			__global RandomSample *samples = (__global RandomSample *)samplesBuff;
			__global RandomSample *sample = &samples[gid];
			__global SampleResult *sampleResult = &sampleResultsBuff[gid];

			__constant const Sampler *sampler = &taskConfig->sampler;
			const uint tableSamples = sampler->pmj02.tableSamples;
			const uint tablePairs = sampler->pmj02.tablePairs;

			// Beyond tablePairs the tables wrap around BUT the scramble is
			// keyed on the unclamped pair index so over-range dims still
			// decorrelate (a clamped index would make all high dims return
			// identical values).
			const uint pairIdx = index >> 1;
			const uint pair = pairIdx % tablePairs;
			const uint idx = sample->pass % tableSamples;
			const uint cycle = sample->pass / tableSamples;
			// Scalar loads: the table base is only 4-byte aligned, so a
			// float2 load would be misaligned for half the entries.
			const uint te = (pair * tableSamples + idx) * 2u;
			const float2 pt = MAKE_FLOAT2(pmjTables[te], pmjTables[te + 1u]);

			const uint px = sampleResult->pixelX;
			const uint py = sampleResult->pixelY;
			const float sx = SobolSequence_BlueNoiseHash(
					px + py * 0x9e3779b9u + pairIdx * 0x85ebca6bu +
					cycle * 0xc2b2ae35u) * (1.f / 4294967296.f);
			const float sy = SobolSequence_BlueNoiseHash(
					py + px * 0x9e3779b9u + pairIdx * 0x85ebca6bu + 0x27d4eb2du +
					cycle * 0xc2b2ae35u) * (1.f / 4294967296.f);
			float u = pt.x + sx;
			float v = pt.y + sy;
			if (u >= 1.f)
				u -= 1.f;
			if (v >= 1.f)
				v -= 1.f;
			return ((index & 1u) == 0u) ? u : v;
		}
	}
}

OPENCL_FORCE_INLINE void PMJ02Sampler_SplatSample(
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

OPENCL_FORCE_INLINE void PMJ02SamplerSharedData_GetNewBucket(__global SobolSamplerSharedData *samplerSharedData,
		const uint bucketCount, uint *newBucketIndex) {
	*newBucketIndex = atomic_inc(&samplerSharedData->bucketIndex) % bucketCount;
}

OPENCL_FORCE_INLINE void PMJ02Sampler_InitNewSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float * filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)
	__constant const Sampler *sampler = &taskConfig->sampler;
	__global SobolSamplerSharedData *samplerSharedData = (__global SobolSamplerSharedData *)samplerSharedDataBuff;
	__global RandomSample *samples = (__global RandomSample *)samplesBuff;
	__global RandomSample *sample = &samples[gid];
	__global float *samplesData = &samplesDataBuff[gid * PMJ02SAMPLER_TOTAL_U_SIZE];

	const uint bucketSize = sampler->pmj02.bucketSize;
	const uint tileSize = sampler->pmj02.tileSize;
	const uint superSampling = sampler->pmj02.superSampling;
	const uint overlapping = sampler->pmj02.overlapping;

	const uint subRegionWidth = filmSubRegion1 - filmSubRegion0 + 1;
	const uint subRegionHeight = filmSubRegion3 - filmSubRegion2 + 1;

	const uint tiletWidthCount = (subRegionWidth + tileSize - 1) / tileSize;
	const uint tileHeightCount = (subRegionHeight + tileSize - 1) / tileSize;

	const uint bucketCount = overlapping * (tiletWidthCount * tileSize * tileHeightCount * tileSize + bucketSize - 1) / bucketSize;

	// Pick the pixel to render

	uint bucketIndex = sample->bucketIndex;
	uint pixelOffset = sample->pixelOffset;
	uint passOffset = sample->passOffset;

	for (;;) {
		passOffset++;
		if (passOffset >= superSampling) {
			pixelOffset++;
			passOffset = 0;

			if (pixelOffset >= bucketSize) {
				// Ask for a new bucket
				PMJ02SamplerSharedData_GetNewBucket(samplerSharedData, bucketCount,
						&bucketIndex);

				sample->bucketIndex = bucketIndex;
				pixelOffset = 0;
				passOffset = 0;
			}
		}

		// Transform the bucket index in a pixel coordinate

		const uint pixelBucketIndex = (bucketIndex / overlapping) * bucketSize + pixelOffset;
		const uint mortonCurveOffset = pixelBucketIndex % (tileSize * tileSize);
		const uint pixelTileIndex = pixelBucketIndex / (tileSize * tileSize);

		const uint subRegionPixelX = (pixelTileIndex % tiletWidthCount) * tileSize + DecodeMorton2X(mortonCurveOffset);
		const uint subRegionPixelY = (pixelTileIndex / tiletWidthCount) * tileSize + DecodeMorton2Y(mortonCurveOffset);
		if ((subRegionPixelX >= subRegionWidth) || (subRegionPixelY >= subRegionHeight)) {
			// Skip the pixels out of the film sub region
			continue;
		}

		const uint pixelX = filmSubRegion0 + subRegionPixelX;
		const uint pixelY = filmSubRegion2 + subRegionPixelY;

		if (filmNoise) {
			const float adaptiveStrength = sampler->pmj02.adaptiveStrength;

			if (adaptiveStrength > 0.f) {
				// Pixels are sampled in accordance with how far from convergence they are
				const float noise = filmNoise[pixelX + pixelY * filmWidth];

				// Factor user driven importance sampling too
				float threshold;
				if (filmUserImportance) {
					const float userImportance = filmUserImportance[pixelX + pixelY * filmWidth];

					// Noise is initialized to INFINITY at start
					if (isinf(noise))
						threshold = userImportance;
					else
						threshold = (userImportance > 0.f) ? Lerp(sampler->pmj02.adaptiveUserImportanceWeight, noise, userImportance) : 0.f;
				} else
					threshold = noise;

				// The floor for the pixel importance is given by the adaptiveness strength
				threshold = fmax(threshold, 1.f - adaptiveStrength);

				if (Rnd_FloatValue(seed) > threshold) {
					// Skip this pixel and try the next one
					continue;
				}
			}
		}

		__global uint *pixelPasses = SobolSampler_GetPassesPtr(samplerSharedData);
		// Get the pass to do
		sample->pass = atomic_inc(&pixelPasses[subRegionPixelX + subRegionPixelY * subRegionWidth]);

		// Screen sample from PMJ02 pair 0 with the per-pixel rotation
		__global const float *pmjTables = PMJ02Sampler_GetTablesPtr(samplerSharedData);
		const uint tableSamples = sampler->pmj02.tableSamples;
		const uint idx = sample->pass % tableSamples;
		const uint cycle = sample->pass / tableSamples;
		// Scalar loads (see GetSample: table base alignment).
		const float2 pt = MAKE_FLOAT2(pmjTables[idx * 2u],
				pmjTables[idx * 2u + 1u]);

		const float sx = SobolSequence_BlueNoiseHash(
				pixelX + pixelY * 0x9e3779b9u + cycle * 0xc2b2ae35u) * (1.f / 4294967296.f);
		const float sy = SobolSequence_BlueNoiseHash(
				pixelY + pixelX * 0x9e3779b9u + 0x85ebca6bu + cycle * 0xc2b2ae35u) * (1.f / 4294967296.f);
		float fx = pt.x + sx;
		float fy = pt.y + sy;
		if (fx >= 1.f)
			fx -= 1.f;
		if (fy >= 1.f)
			fy -= 1.f;
		samplesData[IDX_SCREEN_X] = pixelX + fx;
		samplesData[IDX_SCREEN_Y] = pixelY + fy;
		break;
	}

	// Save the new values
	sample->pixelOffset = pixelOffset;
	sample->passOffset = passOffset;
}

OPENCL_FORCE_INLINE void PMJ02Sampler_NextSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	PMJ02Sampler_InitNewSample(taskConfig,
			filmNoise,
			filmUserImportance,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1,
			filmSubRegion2, filmSubRegion3
			SAMPLER_PARAM);
}

OPENCL_FORCE_INLINE bool PMJ02Sampler_Init(__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)
	__constant const Sampler *sampler = &taskConfig->sampler;
	__global RandomSample *samples = (__global RandomSample *)samplesBuff;
	__global RandomSample *sample = &samples[gid];

	const uint bucketSize = sampler->pmj02.bucketSize;
	sample->pixelOffset = bucketSize * bucketSize;
	sample->passOffset = sampler->pmj02.superSampling;

	PMJ02Sampler_NextSample(taskConfig,
			filmNoise,
			filmUserImportance,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1,
			filmSubRegion2, filmSubRegion3
			SAMPLER_PARAM);

	return true;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
