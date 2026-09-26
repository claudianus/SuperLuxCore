#line 2 "sampler_types.cl"

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
// Indices of Sample related u[] array
//------------------------------------------------------------------------------

#if defined(SLG_OPENCL_KERNEL)

#define IDX_SCREEN_X 0
#define IDX_SCREEN_Y 1
#define IDX_EYE_TIME 2
#define IDX_DOF_X 3
#define IDX_DOF_Y 4
#if defined(SLG_SPECTRAL)
// Hero-wavelength spectral transport: one extra boot dimension for the
// shared wavelength offset (mirrors CPU eyeSampleBootSize = 6). RGB mode
// keeps the historical layout so the sample sequence is unchanged.
#define IDX_WAVELENGTH 5
#define IDX_BSDF_OFFSET 6
#else
#define IDX_BSDF_OFFSET 5
#endif

// Relative to IDX_BSDF_OFFSET + PathDepth * VERTEX_SAMPLE_SIZE
#define IDX_PASSTHROUGH 0
#define IDX_BSDF_X 1
#define IDX_BSDF_Y 2
#define IDX_DIRECTLIGHT_X 3
#define IDX_DIRECTLIGHT_Y 4
#define IDX_DIRECTLIGHT_Z 5
#define IDX_DIRECTLIGHT_W 6
#define IDX_DIRECTLIGHT_A 7
#define IDX_RR 8

#define VERTEX_SAMPLE_SIZE 9

#endif

//------------------------------------------------------------------------------
// Sample data types
//------------------------------------------------------------------------------

typedef struct {
	unsigned int bucketIndex, pixelOffset, passOffset, pass;

	// Staggered cyclic bucket sweep: offset inside the current bucket at
	// which this task's sweep started (and wraps back to, triggering the
	// next bucket fetch). Without a per-task stagger, all tasks sharing a
	// bucket walk pixelOffset 0..bucketSize-1 in lockstep: the first
	// sample wave then covers only bucketCount distinct morton offsets
	// (a periodic pixel mask when taskCount >> filmPixels and rendering
	// halts early). A gid-derived stagger spreads wave-1 coverage over
	// the whole film while preserving per-bucket sweep completeness.
	unsigned int bucketCycleStart;
} RandomSample;

typedef struct {
	float totalI;

	// Using ushort here totally freeze the ATI driver
	unsigned int largeMutationCount, smallMutationCount;
	unsigned int current, proposed, consecutiveRejects;

	float weight;

	SampleResult currentResult;
} MetropolisSample;

typedef struct {
	unsigned int bucketIndex, pixelOffset, passOffset, pass;

	Seed rngGeneratorSeed;
	unsigned int rngPass;
	float rng0, rng1;

	// See RandomSample::bucketCycleStart
	unsigned int bucketCycleStart;
} SobolSample;

typedef struct {
	unsigned int rngPass;
	float rng0, rng1;

	unsigned int pass;
} TilePathSample;

//------------------------------------------------------------------------------
// Sampler shared data types
//------------------------------------------------------------------------------

typedef struct {
	unsigned int bucketIndex;
} RandomSamplerSharedData;

typedef struct {
	unsigned int seedBase;
	unsigned int bucketIndex;

	// This is used to compute the size of appended data at the end
	// of SobolSamplerSharedData
	unsigned int filmRegionPixelCount;

	// Number of Sobol dimensions uploaded after the pass array; the
	// Owen blue-noise scramble tile lives right after the directions
	unsigned int sobolDimensions;

	// Plus the a pass field for each pixel
	// Plus Sobol directions array
	// Plus Owen scramble tile (SOBOL_OWEN_TILE_SIZE^2 uints, SOBOL only)
	// Plus per-pixel luma moments for adaptive sampling
	// (2 floats per pixel: sum and sum of squares, SOBOL only)
} SobolSamplerSharedData;

typedef struct {
	// cameraFilmWidth/cameraFilmHeight and filmWidth/filmHeight are usually
	// the same. They are different when doing tile rendering
	unsigned int cameraFilmWidth, cameraFilmHeight;
	unsigned int tileStartX, tileStartY;
	unsigned int tileWidth, tileHeight;
	unsigned int tilePass, aaSamples;
	unsigned int multipassIndexToRender;

	// Plus Sobol directions array
} TilePathSamplerSharedData;

//------------------------------------------------------------------------------
// Sampler data types
//------------------------------------------------------------------------------

typedef enum {
	RANDOM = 0,
	METROPOLIS = 1,
	SOBOL = 2,
	TILEPATHSAMPLER = 3,
	PMJ02SAMPLER = 4
} SamplerType;

typedef struct {
	SamplerType type;
	union {
		struct {
			float adaptiveStrength, adaptiveUserImportanceWeight;
			unsigned int bucketSize, tileSize, superSampling, overlapping;
		} random;
		struct {
			float adaptiveStrength, adaptiveUserImportanceWeight;
			unsigned int bucketSize, tileSize, superSampling, overlapping;
			unsigned int bluenoiseEnable;
			unsigned int owenEnable;
			unsigned int owenTileEnable;
			unsigned int adaptiveMomentsEnable;
			float adaptiveRelErrTarget;
		} sobol;
		// PMJ02 keeps the bucket cursor fields at the same offsets as
		// sobol/random (the shared sampler prologue reads them through
		// either member); the tables live in the sampler shared data
		// buffer after the per-pixel pass array.
		struct {
			float adaptiveStrength, adaptiveUserImportanceWeight;
			unsigned int bucketSize, tileSize, superSampling, overlapping;
			unsigned int tableSamples, tablePairs;
		} pmj02;
		struct {
			float largeMutationProbability, imageMutationRange;
			unsigned int maxRejects;
		} metropolis;
	};
} Sampler;

#define SOBOL_BITS 32
#define SOBOL_MAX_DIMENSIONS 21201
#define SOBOL_STARTOFFSET 32
#define SOBOL_OWEN_TILE_SIZE 64

// Minimum per-pixel samples before the second-moment adaptive
// estimate is trusted
#define SOBOL_ADAPTIVE_MOMENTS_MIN_SAMPLES 8

#define SAMPLER_PARAM_DECL \
		, Seed *seed \
		, __global void *samplerSharedDataBuff \
		, __global void *samplesBuff \
		, __global float *samplesDataBuff \
		, __global SampleResult *sampleResultsBuff \
		/* Wavefront (B2/E3): the calling kernel's task index (its
		 * remapped WAVEFRONT_GID, or plain get_global_id(0) under the
		 * dense path). All task-persistent arrays must be indexed by
		 * this value, never by get_global_id(0) directly, because a
		 * compacted launch maps lane ids through the task queue. */ \
		, const size_t gid
#define SAMPLER_PARAM \
		, seed \
		, samplerSharedDataBuff \
		, samplesBuff \
		, samplesDataBuff \
		, sampleResultsBuff \
		, gid
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
