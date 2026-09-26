#line 2 "sampler_sobol_funcs.cl"

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
// Sobol Sequence
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE uint SobolSequence_SobolDimension(
		__global const uint* restrict sobolDirections,
		const uint index, const uint dimension) {
	const uint offset = dimension * SOBOL_BITS;
	uint result = 0;
	uint i = index;

	for (uint j = 0; i; i >>= 1, j++) {
		if (i & 1)
			result ^= sobolDirections[offset + j];
	}

	return result;
}

OPENCL_FORCE_INLINE uint SobolSequence_BlueNoiseHash(uint x) {
	// murmur3 32-bit finalizer (must match the CPU version)
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

//------------------------------------------------------------------------------
// Hash-based Owen scrambling
//
// Burley 2020, "Practical Hash-based Owen Scrambling" (JCGT 9(4)), using
// Cessen's improved Laine-Karras hash
// (https://psychopath.io/post/2021_01_30_building_a_better_lk_hash) - the
// same construction used by Blender Cycles' sobol_burley sampler.
//
// The scramble is a bijection where output bit j depends only on input
// bits at positions <= j in the reversed representation, i.e. digit j of
// the base-2 fraction is permuted by a hash of the digits a1..a_j - this
// is Owen's nested uniform scrambling, which maximally randomizes the
// sequence while preserving its (t,s)-sequence stratification.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE uint SobolSequence_ReverseBits(uint x) {
	x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
	x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
	x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
	x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
	return (x >> 16) | (x << 16);
}

OPENCL_FORCE_INLINE uint SobolSequence_ReversedBitOwen(uint n, const uint seed) {
	n ^= n * 0x3d20adeau;
	n += seed;
	n *= (seed >> 16) | 1u;
	n ^= n * 0x05526c56u;
	n ^= n * 0x53a22864u;
	return n;
}

// Nested uniform scramble of a normal-order value: usable both to Owen
// scramble a direction-number product (MSB = first digit a1) and to
// shuffle a sequence index while preserving nested prefix structure.
OPENCL_FORCE_INLINE uint SobolSequence_NestedUniformScramble(const uint i, const uint seed) {
	return SobolSequence_ReverseBits(SobolSequence_ReversedBitOwen(SobolSequence_ReverseBits(i), seed));
}

OPENCL_FORCE_INLINE float SobolSequence_GetSample(
		__global const uint* restrict sobolDirections,
		const uint pass, const uint rngPass, const float rng0, const float rng1,
		const uint index, const bool blueNoiseEnable, const bool owenEnable) {
	uint iResult;
	float shift;

	if (owenEnable) {
		// Owen-scrambled Sobol: rngPass carries the constant per-pixel
		// seed. The sequence index is shuffled by a nested-uniform scramble
		// (decorrelates the sample order across pixels and permits unbiased
		// progressive sampling), then each dimension is scrambled with a
		// per-pixel, per-dimension seed - no Cranley-Patterson rotation is
		// needed because Owen scrambling already provides the randomization.
		const uint shuffleSeed = SobolSequence_BlueNoiseHash(rngPass ^ 0x70efbc49u);
		const uint dimSeed = SobolSequence_BlueNoiseHash(rngPass ^ (index * 0x9e3779b9u + 0x85ebca6bu));
		const uint i = SobolSequence_NestedUniformScramble(pass, shuffleSeed);
		iResult = SobolSequence_NestedUniformScramble(
				SobolSequence_SobolDimension(sobolDirections, i, index), dimSeed);
		// Blue-noise Cranley-Patterson offset from the rank tile (rng0
		// carries the per-pixel offset, < 0 when the tile is disabled);
		// staggered per dimension by an irrational stride
		shift = (rng0 >= 0.f) ? rng0 + index * 0.6180339887f : 0.f;
	} else if (blueNoiseEnable) {
		// Blue-noise dithered sampling (Heitz et al. 2019): per-pixel
		// constant, per-dimension hashed digital shift + Cranley-Patterson
		// offset (must match the CPU version)
		const uint dimSeed = SobolSequence_BlueNoiseHash(rngPass ^ (index * 0x9e3779b9u + 0x85ebca6bu));
		iResult = SobolSequence_SobolDimension(sobolDirections, pass, index) ^ dimSeed;
		shift = SobolSequence_BlueNoiseHash(dimSeed ^ 0xc2b2ae35u) * (1.f / 4294967296.f);
	} else {
		// I scramble pass too in order avoid correlations visible with LIGHTCPU and PATHCPU
		iResult = SobolSequence_SobolDimension(sobolDirections, pass + rngPass, index);

		// Cranley-Patterson rotation to reduce visible regular patterns
		shift = (index & 1) ? rng0 : rng1;
	}

	const float fResult = iResult * (1.f / 0xffffffffu);
	const float val = fResult + shift;

	return val - floor(val);
}

//------------------------------------------------------------------------------
// Sobol Sampler Kernel
//------------------------------------------------------------------------------

#define SOBOLSAMPLER_TOTAL_U_SIZE 2

OPENCL_FORCE_INLINE __global uint *SobolSampler_GetPassesPtr(__global SobolSamplerSharedData *samplerSharedData) {
	// Sobol pass array is appended at the end of slg::ocl::SobolSamplerSharedData
	return (__global uint *)(
			(__global char *)samplerSharedData +
			sizeof(SobolSamplerSharedData));
}

OPENCL_FORCE_INLINE __global const uint* restrict SobolSampler_GetSobolDirectionsPtr(__global SobolSamplerSharedData *samplerSharedData) {
	// Sobol directions array is appended at the end of slg::ocl::SobolSamplerSharedData + all pass values
	return (__global uint *)(
			(__global char *)samplerSharedData +
			sizeof(SobolSamplerSharedData) +
			sizeof(uint) * samplerSharedData->filmRegionPixelCount);
}

OPENCL_FORCE_INLINE __global const uint* restrict SobolSampler_GetScrambleTilePtr(__global SobolSamplerSharedData *samplerSharedData) {
	// The Owen scramble rank tile is appended right after the directions
	return SobolSampler_GetSobolDirectionsPtr(samplerSharedData) +
			samplerSharedData->sobolDimensions * SOBOL_BITS;
}

OPENCL_FORCE_INLINE __global float* restrict SobolSampler_GetLumaMomentsPtr(__global SobolSamplerSharedData *samplerSharedData) {
	// The per-pixel luma moments (2 floats per film region pixel: sum and
	// sum of squares) are appended right after the Owen scramble tile
	return (__global float* restrict)(SobolSampler_GetScrambleTilePtr(samplerSharedData) +
			SOBOL_OWEN_TILE_SIZE * SOBOL_OWEN_TILE_SIZE);
}

OPENCL_FORCE_INLINE float SobolSampler_GetSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const uint index
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)

	switch (index) {
		case IDX_SCREEN_X: {
			__global float *samplesData = &samplesDataBuff[gid * SOBOLSAMPLER_TOTAL_U_SIZE];
			return samplesData[IDX_SCREEN_X];
		}
		case IDX_SCREEN_Y: {
			__global float *samplesData = &samplesDataBuff[gid * SOBOLSAMPLER_TOTAL_U_SIZE];
			return samplesData[IDX_SCREEN_Y];
		}
		default: {
			__global SobolSamplerSharedData *samplerSharedData = (__global SobolSamplerSharedData *)samplerSharedDataBuff;
			__global const uint* restrict sobolDirections = SobolSampler_GetSobolDirectionsPtr(samplerSharedData);

			__global SobolSample *samples = (__global SobolSample *)samplesBuff;
			__global SobolSample *sample = &samples[gid];

			__constant const Sampler *sampler = &taskConfig->sampler;

			return SobolSequence_GetSample(sobolDirections, sample->pass, sample->rngPass, sample->rng0, sample->rng1, index,
					sampler->sobol.bluenoiseEnable != 0u, sampler->sobol.owenEnable != 0u);
		}
	}
}

OPENCL_FORCE_INLINE void SobolSampler_SplatSample(
		__constant const GPUTaskConfiguration* restrict taskConfig
		SAMPLER_PARAM_DECL
		FILM_PARAM_DECL
		) {
	// gid: task index supplied by the caller (wavefront-safe)
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	Film_AddSample(sampleResult->pixelX, sampleResult->pixelY,
			sampleResult, 1.f
			FILM_PARAM);

	// Accumulate the sample luminance first and second moments for the
	// second-moment adaptive sampling estimate (used in InitNewSample)
	__constant const Sampler *sampler = &taskConfig->sampler;
	if (sampler->sobol.adaptiveMomentsEnable != 0u) {
		__global SobolSamplerSharedData *samplerSharedData = (__global SobolSamplerSharedData *)samplerSharedDataBuff;
		__global float *lumaMoments = SobolSampler_GetLumaMomentsPtr(samplerSharedData);

		const uint subRegionWidth = filmSubRegion1 - filmSubRegion0 + 1;
		const uint midx = ((sampleResult->pixelX - filmSubRegion0) +
				(sampleResult->pixelY - filmSubRegion2) * subRegionWidth) * 2;

		const float3 r = VLOAD3F(sampleResult->radiancePerPixelNormalized[0].c);
		const float luma = 0.2126f * r.x + 0.7152f * r.y + 0.0722f * r.z;
		if (!isnan(luma) && !isinf(luma)) {
			AtomicAdd(&lumaMoments[midx], luma);
			AtomicAdd(&lumaMoments[midx + 1], luma * luma);
		}
	}
}

OPENCL_FORCE_INLINE void SobolSamplerSharedData_GetNewBucket(__global SobolSamplerSharedData *samplerSharedData,
		const uint bucketCount, uint *newBucketIndex, uint *seed) {
	*newBucketIndex = atomic_inc(&samplerSharedData->bucketIndex) % bucketCount;

    *seed = (samplerSharedData->seedBase + *newBucketIndex) % (0xFFFFFFFFu - 1u) + 1u;
}

OPENCL_FORCE_INLINE void SobolSampler_InitNewSample(
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
	__global SobolSample *samples = (__global SobolSample *)samplesBuff;
	__global SobolSample *sample = &samples[gid];
	__global float *samplesData = &samplesDataBuff[gid * SOBOLSAMPLER_TOTAL_U_SIZE];

	const uint bucketSize = sampler->sobol.bucketSize;
	const uint tileSize = sampler->sobol.tileSize;
	const uint superSampling = sampler->sobol.superSampling;
	const uint overlapping = sampler->sobol.overlapping;
	
	const uint subRegionWidth = filmSubRegion1 - filmSubRegion0 + 1;
	const uint subRegionHeight = filmSubRegion3 - filmSubRegion2 + 1;

	const uint tiletWidthCount = (subRegionWidth + tileSize - 1) / tileSize;
	const uint tileHeightCount = (subRegionHeight + tileSize - 1) / tileSize;

	const uint bucketCount = overlapping * (tiletWidthCount * tileSize * tileHeightCount * tileSize + bucketSize - 1) / bucketSize;

	// Pick the pixel to render

	uint bucketIndex = sample->bucketIndex;
	uint pixelOffset = sample->pixelOffset;
	uint passOffset = sample->passOffset;
	const uint bucketCycleStart = sample->bucketCycleStart;

	Seed rngGeneratorSeed = sample->rngGeneratorSeed;

	// Bound for the adaptive re-pick loop below: bucketSize * superSampling
	// iterations visit every pixelOffset of the current bucket once. Without
	// the cap a fully-converged frame (all pixels below the relErr target)
	// would spin here forever
	uint skipAttempts = 0;

	for (;;) {
		passOffset++;
		if (passOffset >= superSampling) {
			pixelOffset++;
			if (pixelOffset >= bucketSize)
				pixelOffset = 0;
			passOffset = 0;

			if (pixelOffset == bucketCycleStart) {
				// The task completed a full cyclic sweep of the bucket:
				// ask for a new bucket (see RandomSample::bucketCycleStart)
				uint bucketSeed;
				SobolSamplerSharedData_GetNewBucket(samplerSharedData, bucketCount,
						&bucketIndex, &bucketSeed);

				sample->bucketIndex = bucketIndex;

				Rnd_Init(bucketSeed, &rngGeneratorSeed);
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

		if (filmNoise || (sampler->sobol.adaptiveMomentsEnable != 0u)) {
			const float adaptiveStrength = sampler->sobol.adaptiveStrength;

			if (adaptiveStrength > 0.f) {
				// Pixels are sampled in accordance with how far from convergence they are
				float noise = INFINITY;
				bool noiseValid = false;

				// Second-moment estimate: the relative standard error of
				// the pixel mean is a per-pixel absolute convergence
				// measure (unlike the film NOISE channel which is a
				// min-max normalized image-difference heuristic updated
				// only every test step on the host)
				if (sampler->sobol.adaptiveMomentsEnable != 0u) {
					const uint subIdx = subRegionPixelX + subRegionPixelY * subRegionWidth;
					__global uint *pixelPasses = SobolSampler_GetPassesPtr(samplerSharedData);
					const uint curPass = pixelPasses[subIdx];
					if (curPass >= SOBOL_STARTOFFSET + SOBOL_ADAPTIVE_MOMENTS_MIN_SAMPLES) {
						const float n = (float)(curPass - SOBOL_STARTOFFSET);
						__global const float *lumaMoments = SobolSampler_GetLumaMomentsPtr(samplerSharedData) + subIdx * 2;
						const float mean = lumaMoments[0] / n;
						const float var = fmax(lumaMoments[1] / n - mean * mean, 0.f);
						// std. error of the mean, relative to the mean
						const float relErr = native_sqrt(var / n) / (fabs(mean) + 1e-6f);
						noise = fmin(relErr / sampler->sobol.adaptiveRelErrTarget, 1.f);
						noiseValid = true;
					}
				}

				// Fall back to the host-computed film NOISE channel when
				// the moments estimate is not yet valid; INFINITY (never
				// skipped) when neither source is available
				if (!noiseValid && filmNoise)
					noise = filmNoise[pixelX + pixelY * filmWidth];

				// Factor user driven importance sampling too
				float threshold;
				if (filmUserImportance) {
					const float userImportance = filmUserImportance[pixelX + pixelY * filmWidth];

					// Noise is initialized to INFINITY at start
					if (isinf(noise))
						threshold = userImportance;
					else
						threshold = (userImportance > 0.f) ? Lerp(sampler->sobol.adaptiveUserImportanceWeight, noise, userImportance) : 0.f;
				} else
					threshold = noise;

				// The floor for the pixel importance is given by the adaptiveness strength
				threshold = fmax(threshold, 1.f - adaptiveStrength);

				if (Rnd_FloatValue(seed) > threshold) {
					// Skip this pixel and try the next one; after a full
					// bucket sweep accept it anyway (bounded loop)
					if (++skipAttempts < bucketSize * superSampling) {
						// Workaround for preserving random number distribution behavior
						Rnd_UintValue(&rngGeneratorSeed);
						Rnd_FloatValue(&rngGeneratorSeed);
						Rnd_FloatValue(&rngGeneratorSeed);

						continue;
					}
				}
			}
		}

		//----------------------------------------------------------------------
		// This code crashes the AMD OpenCL compiler:
		//
		// The array of fields is attached to the SamplerSharedData structure
#if !defined(LUXCORE_AMD_OPENCL)
		__global uint *pixelPasses = SobolSampler_GetPassesPtr(samplerSharedData);
		// Get the pass to do
		sample->pass = atomic_inc(&pixelPasses[subRegionPixelX + subRegionPixelY * subRegionWidth]);
#else   //----------------------------------------------------------------------
		// This one works:
		//
		// The array of fields is attached to the SamplerSharedData structure
		__global uint *pixelPass = SobolSampler_GetPassesPtr(samplerSharedData) + (subRegionPixelX + subRegionPixelY * subRegionWidth);
		// Get the pass to do
		uint oldVal, newVal;
		do {
				oldVal = *pixelPass;
				newVal = oldVal + 1;
		} while (atomic_cmpxchg(pixelPass, oldVal, newVal) != oldVal);
		sample->pass = oldVal;
#endif
		//----------------------------------------------------------------------

		// Initialize rng0 and rng1

		if (sampler->sobol.owenEnable != 0u) {
			// Owen-scrambled Sobol: constant per-pixel scramble seed, plus
			// an optional blue-noise rank-tile offset for the pixel's
			// Cranley-Patterson shift (stored in rng0, < 0 = disabled)
			sample->rngPass = SobolSequence_BlueNoiseHash(pixelX + pixelY * 0x9e3779b9u) ^ samplerSharedData->seedBase;
			if (sampler->sobol.owenTileEnable != 0u) {
				__global const uint* restrict scrambleTile = SobolSampler_GetScrambleTilePtr(samplerSharedData);
				const uint tileIdx = (pixelY % SOBOL_OWEN_TILE_SIZE) * SOBOL_OWEN_TILE_SIZE +
						(pixelX % SOBOL_OWEN_TILE_SIZE);
				sample->rng0 = (scrambleTile[tileIdx] + 0.5f) *
						(1.f / (float)(SOBOL_OWEN_TILE_SIZE * SOBOL_OWEN_TILE_SIZE));
			} else
				sample->rng0 = -1.f;
			sample->rng1 = 0.f;
		} else if (sampler->sobol.bluenoiseEnable != 0u) {
			// Blue-noise dithered sampling (Heitz et al. 2019): the dither
			// seed is constant per pixel (across passes); the per-dimension
			// shifts are derived inside SobolSequence_GetSample()
			sample->rngPass = SobolSequence_BlueNoiseHash(pixelX + pixelY * 0x9e3779b9u) ^ samplerSharedData->seedBase;
			sample->rng0 = 0.f;
			sample->rng1 = 0.f;
		} else {
			// Limit the number of pass skipped
			sample->rngPass = Rnd_UintValue(&rngGeneratorSeed);
			sample->rng0 = Rnd_FloatValue(&rngGeneratorSeed);
			sample->rng1 = Rnd_FloatValue(&rngGeneratorSeed);
		}

		// Initialize IDX_SCREEN_X and IDX_SCREEN_Y sample

		__global const uint* restrict sobolDirections = SobolSampler_GetSobolDirectionsPtr(samplerSharedData);
		const bool blueNoiseEnable = (sampler->sobol.bluenoiseEnable != 0u);
		const bool owenEnable = (sampler->sobol.owenEnable != 0u);
		samplesData[IDX_SCREEN_X] = pixelX + SobolSequence_GetSample(sobolDirections, sample->pass, sample->rngPass, sample->rng0, sample->rng1, IDX_SCREEN_X, blueNoiseEnable, owenEnable);
		samplesData[IDX_SCREEN_Y] = pixelY + SobolSequence_GetSample(sobolDirections, sample->pass, sample->rngPass, sample->rng0, sample->rng1, IDX_SCREEN_Y, blueNoiseEnable, owenEnable);
		break;
	}
	
	sample->rngGeneratorSeed = rngGeneratorSeed;

	// Save the new values
	sample->pixelOffset = pixelOffset;
	sample->passOffset = passOffset;
}

OPENCL_FORCE_INLINE void SobolSampler_NextSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	SobolSampler_InitNewSample(taskConfig,
			filmNoise,
			filmUserImportance,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3
			SAMPLER_PARAM);
}

OPENCL_FORCE_INLINE bool SobolSampler_Init(__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	// gid: task index supplied by the caller (wavefront-safe)
	__constant const Sampler *sampler = &taskConfig->sampler;
	__global SobolSample *samples = (__global SobolSample *)samplesBuff;
	__global SobolSample *sample = &samples[gid];

	const uint bucketSize = sampler->sobol.bucketSize;
	// Staggered cyclic sweep (see RandomSample::bucketCycleStart)
	sample->bucketCycleStart = gid % bucketSize;
	sample->pixelOffset = sample->bucketCycleStart - 1;
	sample->passOffset = sampler->sobol.superSampling;

	SobolSampler_NextSample(taskConfig,
			filmNoise,
			filmUserImportance,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3
			SAMPLER_PARAM);

	return true;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
