#line 2 "sampler_funcs.cl"

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
// Sampler functions
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float Sampler_GetSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const uint index
		SAMPLER_PARAM_DECL) {
	switch (taskConfig->sampler.type) {
		case RANDOM:
			return RandomSampler_GetSample(taskConfig, index SAMPLER_PARAM);
		case SOBOL:
			return SobolSampler_GetSample(taskConfig, index SAMPLER_PARAM);
		case METROPOLIS:
			return MetropolisSampler_GetSample(taskConfig, index SAMPLER_PARAM);
		case TILEPATHSAMPLER:
			return TilePathSampler_GetSample(taskConfig, index SAMPLER_PARAM);
		case PMJ02SAMPLER:
			return PMJ02Sampler_GetSample(taskConfig, index SAMPLER_PARAM);
		default:
			// Something has gone very wrong here
			return 0.f;
	}
}

// Cuda reports large argument size, so overrides noinline attribute anyway
OPENCL_FORCE_INLINE void Sampler_SplatSample(
		__constant const GPUTaskConfiguration* restrict taskConfig
		SAMPLER_PARAM_DECL
		FILM_PARAM_DECL
		) {
	switch (taskConfig->sampler.type) {
		case RANDOM:
			return RandomSampler_SplatSample(taskConfig
					SAMPLER_PARAM
					FILM_PARAM);
		case SOBOL:
			return SobolSampler_SplatSample(taskConfig
					SAMPLER_PARAM
					FILM_PARAM);
		case METROPOLIS:
			return MetropolisSampler_SplatSample(taskConfig
					SAMPLER_PARAM
					FILM_PARAM);
		case TILEPATHSAMPLER:
			return TilePathSampler_SplatSample(taskConfig
					SAMPLER_PARAM
					FILM_PARAM);
		case PMJ02SAMPLER:
			return PMJ02Sampler_SplatSample(taskConfig
					SAMPLER_PARAM
					FILM_PARAM);
		default:
			// Something has gone very wrong here
			return;
	}
}

OPENCL_FORCE_NOT_INLINE void Sampler_NextSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	switch (taskConfig->sampler.type) {
		case RANDOM:
			return RandomSampler_NextSample(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case SOBOL:
			return SobolSampler_NextSample(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case METROPOLIS:
			return MetropolisSampler_NextSample(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case TILEPATHSAMPLER:
			return TilePathSampler_NextSample(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case PMJ02SAMPLER:
			return PMJ02Sampler_NextSample(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		default:
			// Something has gone very wrong here
			return;
	}
}

OPENCL_FORCE_NOT_INLINE bool Sampler_Init(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		__global float *filmNoise,
		__global float *filmUserImportance,
		const uint filmWidth, const uint filmHeight,
		const uint filmSubRegion0, const uint filmSubRegion1,
		const uint filmSubRegion2, const uint filmSubRegion3
		SAMPLER_PARAM_DECL) {
	switch (taskConfig->sampler.type) {
		case RANDOM:
			return RandomSampler_Init(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case SOBOL:
			return SobolSampler_Init(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case METROPOLIS:
			return MetropolisSampler_Init(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case TILEPATHSAMPLER:
			return TilePathSampler_Init(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		case PMJ02SAMPLER:
			return PMJ02Sampler_Init(taskConfig,
					filmNoise,
					filmUserImportance,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1,
					filmSubRegion2, filmSubRegion3
					SAMPLER_PARAM);
		default:
			// Something has gone very wrong here
			return true;
	}
}

//------------------------------------------------------------------------------
// GPU light tracing (doc/features/gpu_lighttracing.md)
//
// Light-path tasks share the eye sampler infrastructure but are not bound
// to a film pixel: every light-task dimension d maps to sampler dimension
// d + 2 (dims 0/1 are the screen X/Y special cases of every device
// sampler). The sequence state lives in the task's own sample slot; the
// pixel picking machinery of InitNewSample is never used.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float Sampler_GetLightSample(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const uint index
		SAMPLER_PARAM_DECL) {
	// A light path is not a Metropolis chain: the eye-path mutation
	// machinery (accept/reject, importance normalization) has no
	// meaning for a task that just deposits splats. Light tasks draw
	// an i.i.d. uniform stream from their private seed instead of
	// touching the current/proposed sample vectors (which are also not
	// sized for the light dimension offset)
	if (taskConfig->sampler.type == METROPOLIS)
		return Rnd_FloatValue(seed);

	return Sampler_GetSample(taskConfig, index + 2 SAMPLER_PARAM);
}

// Per-task one-time init (Init kernel): seeds the sequence state so each
// light task walks an independent, decorrelated stream.
OPENCL_FORCE_INLINE void Sampler_LightTaskInit(
		__constant const GPUTaskConfiguration* restrict taskConfig,
		const uint lightTaskIndex,
		const uint filmWidth, const uint filmHeight
		SAMPLER_PARAM_DECL) {
	switch (taskConfig->sampler.type) {
		case SOBOL: {
			__global SobolSample *samples = (__global SobolSample *)samplesBuff;
			__global SobolSample *sample = &samples[gid];

			// Global-unique Sobol sequence index: task i samples
			// SOBOL_STARTOFFSET + i + n * lightTaskCount (strided in
			// Sampler_LightNextSample)
			sample->pass = SOBOL_STARTOFFSET + lightTaskIndex;
			sample->rngPass = (uint)Rnd_UintValue(seed);
			sample->rng0 = Rnd_FloatValue(seed);
			sample->rng1 = Rnd_FloatValue(seed);
			break;
		}
		case PMJ02SAMPLER: {
			__global RandomSample *samples = (__global RandomSample *)samplesBuff;
			samples[gid].pass = lightTaskIndex;

			// The scramble hash is keyed on pixelX/pixelY: give the task a
			// deterministic pseudo-pixel so light tasks decorrelate
			__global SampleResult *sampleResult = &sampleResultsBuff[gid];
			sampleResult->pixelX = lightTaskIndex % filmWidth;
			sampleResult->pixelY = (lightTaskIndex / filmWidth) % filmHeight;
			break;
		}
		case TILEPATHSAMPLER: {
			__global TilePathSample *samples = (__global TilePathSample *)samplesBuff;
			__global TilePathSample *sample = &samples[gid];
			sample->pass = lightTaskIndex;
			sample->rngPass = (uint)Rnd_UintValue(seed);
			sample->rng0 = Rnd_FloatValue(seed);
			sample->rng1 = Rnd_FloatValue(seed);
			break;
		}
		default:
			// RANDOM is seed driven; METROPOLIS light tasks are too
			// (see Sampler_GetLightSample)
			break;
	}
}

// Per-sample advance: draws a fresh light-path sample.
OPENCL_FORCE_INLINE void Sampler_LightNextSample(
		__constant const GPUTaskConfiguration* restrict taskConfig
		SAMPLER_PARAM_DECL) {
	const uint lightTaskCount = taskConfig->pathTracer.lightTracing.lightTaskCount;

	switch (taskConfig->sampler.type) {
		case SOBOL: {
			__global SobolSample *samples = (__global SobolSample *)samplesBuff;
			__global SobolSample *sample = &samples[gid];
			sample->pass += lightTaskCount;
			break;
		}
		case PMJ02SAMPLER: {
			__global RandomSample *samples = (__global RandomSample *)samplesBuff;
			samples[gid].pass += 1u;
			break;
		}
		case TILEPATHSAMPLER: {
			__global TilePathSample *samples = (__global TilePathSample *)samplesBuff;
			samples[gid].pass += 1u;
			break;
		}
		default:
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
