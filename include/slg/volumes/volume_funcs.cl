#line 2 "volume_funcs.cl"

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

OPENCL_FORCE_INLINE float3 Volume_Emission(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	const uint emiTexIndex = vol->volume.volumeEmissionTexIndex;
	if (emiTexIndex != NULL_INDEX) {
#if defined(SLG_SPECTRAL)
		// Emission-context eval: leaf RGB producers pick the illuminant
		// Smits basis (mirrors CPU GetEmissionSpectrumValue). tmpHitPoint
		// is a task-local buffer; the flag is restored after the eval.
		const uint prevEmissionEval = hitPoint->spectralEmissionEval;
		((__global HitPoint *)hitPoint)->spectralEmissionEval = 1u;
#endif
		const float3 emiTex = Texture_GetSpectrumValue(emiTexIndex, hitPoint
			TEXTURES_PARAM);
#if defined(SLG_SPECTRAL)
		((__global HitPoint *)hitPoint)->spectralEmissionEval = prevEmissionEval;
#endif
		return clamp(emiTex, 0.f, INFINITY);
	} else
		return BLACK;
}

OPENCL_FORCE_INLINE void Volume_InitializeTmpHitPoint(__global HitPoint *tmpHitPoint,
		const float3 rayOrig, const float3 rayDir, const float passThroughEvent) {
	// Preserve the path spectral state: Scene_Intersect() copies it onto
	// tmpHitPoint before the volume scatter walk, and HitPoint_InitDefault
	// would otherwise reset it to the neutral default.
	const float swSave0 = tmpHitPoint->spectralW[0];
	const float swSave1 = tmpHitPoint->spectralW[1];
	const float swSave2 = tmpHitPoint->spectralW[2];
	const uint heroAliveSave = tmpHitPoint->spectralHeroAlive;

	// Initialize tmpHitPoint
	HitPoint_InitDefault(tmpHitPoint);

	tmpHitPoint->spectralW[0] = swSave0;
	tmpHitPoint->spectralW[1] = swSave1;
	tmpHitPoint->spectralW[2] = swSave2;
	tmpHitPoint->spectralHeroAlive = heroAliveSave;

	VSTORE3F(rayDir, &tmpHitPoint->fixedDir.x);
	VSTORE3F(rayOrig, &tmpHitPoint->p.x);
	VSTORE3F(-rayDir, &tmpHitPoint->geometryN.x);
	VSTORE3F(-rayDir, &tmpHitPoint->interpolatedN.x);
	VSTORE3F(-rayDir, &tmpHitPoint->shadeN.x);
	tmpHitPoint->passThroughEvent = passThroughEvent;
	Transform_Init(&tmpHitPoint->localToWorld);
}

OPENCL_FORCE_INLINE float HomogeneousVolume_SegmentScatter(const float u, 
		const bool scatterAllowed, const float segmentLength,
		const float3 *sigmaA, const float3 *sigmaS, const float3 *emission,
		float3 *segmentTransmittance, float3 *segmentEmission) {
	// This code must work also with segmentLength = INFINITY

	bool scatter = false;
	*segmentTransmittance = WHITE;
	*segmentEmission = BLACK;

	//--------------------------------------------------------------------------
	// Check if there is a scattering event
	//--------------------------------------------------------------------------

	float scatterDistance = segmentLength;
	const float sigmaSValue = Spectrum_Filter(*sigmaS);
	if (scatterAllowed && (sigmaSValue > 0.f)) {
		// Determine scattering distance
		const float proposedScatterDistance = -log(1.f - u) / sigmaSValue;

		scatter = (proposedScatterDistance < segmentLength);
		scatterDistance = scatter ? proposedScatterDistance : segmentLength;

		// Note: scatterDistance can not be infinity because otherwise there would
		// have been a scatter event before.
		const float tau = scatterDistance * sigmaSValue;
		const float pdf = exp(-tau) * (scatter ? sigmaSValue : 1.f);
		*segmentTransmittance *= 1.f / pdf;
	}

	//--------------------------------------------------------------------------
	// Volume transmittance
	//--------------------------------------------------------------------------
	
	const float3 sigmaT = *sigmaA + *sigmaS;
	if (!Spectrum_IsBlack(sigmaT)) {
		if (isinf(scatterDistance)) {
			// This avoid NaN in case scatterDistance is inf
			*segmentTransmittance = BLACK;
		} else {
			const float3 tau = scatterDistance * sigmaT;
			*segmentTransmittance *= Spectrum_Exp(-tau) * (scatter ? sigmaT : WHITE);
		}
	}

	//--------------------------------------------------------------------------
	// Volume emission
	//--------------------------------------------------------------------------

	*segmentEmission += (*segmentTransmittance) * scatterDistance * (*emission);

	return scatter ? scatterDistance : -1.f;
}

//------------------------------------------------------------------------------
// ClearVolume scatter
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 ClearVolume_SigmaA(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	const float3 sigmaA = Texture_GetSpectrumValue(vol->volume.clear.sigmaATexIndex, hitPoint
		TEXTURES_PARAM);
			
	return clamp(sigmaA, 0.f, INFINITY);
}

OPENCL_FORCE_INLINE float3 ClearVolume_SigmaS(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	return BLACK;
}

OPENCL_FORCE_INLINE float3 ClearVolume_SigmaT(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	return
			ClearVolume_SigmaA(vol, hitPoint
				TEXTURES_PARAM) +
			ClearVolume_SigmaS(vol, hitPoint
				TEXTURES_PARAM);
}

OPENCL_FORCE_INLINE float ClearVolume_Scatter(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThroughEvent,
		const bool scatteredStart, float3 *connectionThroughput,
		float3 *connectionEmission, __global HitPoint *tmpHitPoint
		TEXTURES_PARAM_DECL) {
	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);

	// Initialize tmpHitPoint
	Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThroughEvent);

	const float distance = hitT - ray->mint;	
	float3 transmittance = WHITE;

	const float3 sigmaT = ClearVolume_SigmaT(vol, tmpHitPoint
			TEXTURES_PARAM);
	if (!Spectrum_IsBlack(sigmaT)) {
		const float3 tau = clamp(distance * sigmaT, 0.f, INFINITY);
		transmittance = Spectrum_Exp(-tau);
	}

	// Apply volume transmittance
	*connectionThroughput *= transmittance;

	// Apply volume emission
	const uint emiTexIndex = vol->volume.volumeEmissionTexIndex;
	if (emiTexIndex != NULL_INDEX) {
		const float3 emiTex = Texture_GetSpectrumValue(emiTexIndex, tmpHitPoint
			TEXTURES_PARAM);
		*connectionEmission += *connectionThroughput * distance * clamp(emiTex, 0.f, INFINITY);
	}

	return -1.f;
}

//------------------------------------------------------------------------------
// HomogeneousVolume scatter
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 HomogeneousVolume_SigmaA(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	const float3 sigmaA = Texture_GetSpectrumValue(vol->volume.homogenous.sigmaATexIndex, hitPoint
		TEXTURES_PARAM);
			
	return clamp(sigmaA, 0.f, INFINITY);
}

OPENCL_FORCE_INLINE float3 HomogeneousVolume_SigmaS(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	const float3 sigmaS = Texture_GetSpectrumValue(vol->volume.homogenous.sigmaSTexIndex, hitPoint
		TEXTURES_PARAM);
			
	return clamp(sigmaS, 0.f, INFINITY);
}

OPENCL_FORCE_INLINE float HomogeneousVolume_Scatter(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThroughEvent,
		const bool scatteredStart, float3 *connectionThroughput,
		float3 *connectionEmission, __global HitPoint *tmpHitPoint
		TEXTURES_PARAM_DECL) {
	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);

	// Initialize tmpHitPoint
	Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThroughEvent);

	const float segmentLength = hitT - ray->mint;

	// Check if I have to support multi-scattering
	const bool scatterAllowed = (!scatteredStart || vol->volume.homogenous.multiScattering);

	const float3 sigmaA = HomogeneousVolume_SigmaA(vol, tmpHitPoint
			TEXTURES_PARAM);
	const float3 sigmaS = HomogeneousVolume_SigmaS(vol, tmpHitPoint
			TEXTURES_PARAM);
	const float3 emission = Volume_Emission(vol, tmpHitPoint
			TEXTURES_PARAM);

	const float sigmaSValue = Spectrum_Filter(sigmaS);
	if (vol->volume.homogenous.distanceSampling && (eqLightCount > 0u) &&
			scatterAllowed && (sigmaSValue > 0.f) && isfinite(segmentLength) &&
			(segmentLength > 0.f)) {
		// Equiangular + transmittance MIS distance sampling
		// (Kulla & Fajardo, EGSR 2012), mirroring the CPU
		// HomogeneousVolume::ScatterEquiangular
		Seed seed;
		Rnd_InitFloat(passThroughEvent, &seed);

		// Contribution-aware light selection:
		// w_i = lum_i * (thetaB_i - thetaA_i) / D_i (two-pass weighted pick;
		// the selection probability cancels in the conditioned MIS)
		const float u1 = Rnd_FloatValue(&seed);
		float weightsSum = 0.f;
		for (uint i = 0u; i < eqLightCount; ++i) {
			const float4 lp = eqLightPoints[i];
			const float3 toL = ((float3)(lp.x, lp.y, lp.z)) - rayOrig;
			const float dlt = dot(toL, rayDir);
			const float D = sqrt(max(1e-10f, dot(toL, toL) - dlt * dlt));
			const float thA = atan2(ray->mint - dlt, D);
			const float thB = atan2(hitT - dlt, D);
			weightsSum += lp.w * (thB - thA) / D;
		}
		uint lightIdx = eqLightCount - 1u;
		if (weightsSum > 0.f) {
			float acc = 0.f;
			for (uint i = 0u; i < eqLightCount; ++i) {
				const float4 lp = eqLightPoints[i];
				const float3 toL = ((float3)(lp.x, lp.y, lp.z)) - rayOrig;
				const float dlt = dot(toL, rayDir);
				const float D = sqrt(max(1e-10f, dot(toL, toL) - dlt * dlt));
				const float thA = atan2(ray->mint - dlt, D);
				const float thB = atan2(hitT - dlt, D);
				acc += lp.w * (thB - thA) / D;
				if (acc >= u1 * weightsSum) {
					lightIdx = i;
					break;
				}
			}
		} else
			lightIdx = min((uint)(u1 * eqLightCount), eqLightCount - 1u);

		const float4 lp = eqLightPoints[lightIdx];
		const float3 lightPos = (float3)(lp.x, lp.y, lp.z);

		// delta: projection of the light on the ray (absolute t domain);
		// D: perpendicular distance of the light from the ray
		const float3 toLight = lightPos - rayOrig;
		const float delta = dot(toLight, rayDir);
		const float D = sqrt(max(1e-10f, dot(toLight, toLight) - delta * delta));
		const float thetaA = atan2(ray->mint - delta, D);
		const float thetaB = atan2(hitT - delta, D);
		const float invThetaRange = 1.f / (thetaB - thetaA);

		// One-sample MIS conditioned on the picked light (the light
		// selection probability cancels in the balance heuristic)
		const float3 sigmaT = sigmaA + sigmaS;
		float dist, pdf;
		bool collision;
		if (Rnd_FloatValue(&seed) < .5f) {
			// Equiangular strategy: always a vertex in [mint, maxt]
			const float tAbs = delta + D * tan(thetaA +
					(thetaB - thetaA) * Rnd_FloatValue(&seed));
			dist = clamp(tAbs - ray->mint, 0.f, segmentLength);
			const float pdfT = sigmaSValue * exp(-sigmaSValue * dist);
			const float pdfE = D * invThetaRange /
					(D * D + (tAbs - delta) * (tAbs - delta));
			pdf = .5f * (pdfT + pdfE);
			collision = true;
		} else {
			// Transmittance strategy
			dist = -log(1.f - Rnd_FloatValue(&seed)) / sigmaSValue;
			collision = (dist < segmentLength);
			if (collision) {
				const float pdfT = sigmaSValue * exp(-sigmaSValue * dist);
				const float tAbs = ray->mint + dist;
				const float pdfE = D * invThetaRange /
						(D * D + (tAbs - delta) * (tAbs - delta));
				pdf = .5f * (pdfT + pdfE);
			} else {
				dist = segmentLength;
				pdf = .5f * exp(-sigmaSValue * segmentLength);
			}
		}

		float3 segmentTransmittance = WHITE / pdf;
		if (collision) {
			const float3 tau = dist * sigmaT;
			segmentTransmittance *= Spectrum_Exp(-tau) * sigmaT;
		} else {
			const float3 tau = segmentLength * sigmaT;
			segmentTransmittance *= Spectrum_Exp(-tau);
		}

		// I need to update first connectionEmission and than connectionThroughput
		*connectionEmission += *connectionThroughput * emission;
		*connectionThroughput *= segmentTransmittance;

		return collision ? (ray->mint + dist) : -1.f;
	}

	float3 segmentTransmittance, segmentEmission;
	const float scatterDistance = HomogeneousVolume_SegmentScatter(passThroughEvent, scatterAllowed,
			segmentLength, &sigmaA, &sigmaS, &emission,
			&segmentTransmittance, &segmentEmission);

	// I need to update first connectionEmission and than connectionThroughput
	*connectionEmission += *connectionThroughput * emission;
	*connectionThroughput *= segmentTransmittance;

	return (scatterDistance == -1.f) ? -1.f : (ray->mint + scatterDistance);
}

//------------------------------------------------------------------------------
// HeterogeneousVolume scatter
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 HeterogeneousVolume_SigmaA(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	const float3 sigmaA = Texture_GetSpectrumValue(vol->volume.heterogenous.sigmaATexIndex, hitPoint
		TEXTURES_PARAM);
			
	return clamp(sigmaA, 0.f, INFINITY);
}

OPENCL_FORCE_INLINE float3 HeterogeneousVolume_SigmaS(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	const float3 sigmaS = Texture_GetSpectrumValue(vol->volume.heterogenous.sigmaSTexIndex, hitPoint
		TEXTURES_PARAM);
			
	return clamp(sigmaS, 0.f, INFINITY);
}

OPENCL_FORCE_INLINE float3 HeterogeneousVolume_SigmaT(__global const Volume *vol, __global const HitPoint *hitPoint
	TEXTURES_PARAM_DECL) {
	return HeterogeneousVolume_SigmaA(vol, hitPoint TEXTURES_PARAM) +
			HeterogeneousVolume_SigmaS(vol, hitPoint TEXTURES_PARAM);
}

OPENCL_FORCE_INLINE float HeterogeneousVolume_MarchScatter(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThroughEvent,
		const bool scatteredStart, float3 *connectionThroughput,
		float3 *connectionEmission, __global HitPoint *tmpHitPoint,
		Seed *seed
		TEXTURES_PARAM_DECL) {
	const float stepSize = vol->volume.heterogenous.stepSize;
	const uint maxStepsCount = vol->volume.heterogenous.maxStepsCount;

	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);

	// Compute the number of steps to evaluate the volume
	const float segmentLength = hitT - ray->mint;

	// Handle the case when segmentLength is infinity or a very large number
	//
	// Note: the old code"Min(maxStepsCount, Ceil2UInt(segmentLength / stepSize))"
	// can overflow for large values of segmentLength so I have to use
	// "Ceil2UInt(Min((float)maxStepsCount, segmentLength / stepSize))"
	const uint steps = Ceil2UInt(fmin((float)maxStepsCount, segmentLength / stepSize));

	const float currentStepSize = fmin(segmentLength / steps, maxStepsCount * stepSize);

	// Check if I have to support multi-scattering
	const bool scatterAllowed = (!scatteredStart || vol->volume.heterogenous.multiScattering);

	// Initialize tmpHitPoint
	Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThroughEvent);

	for (uint s = 0; s < steps; ++s) {
		// Compute the scattering over the current step
		const float evaluationPoint = (s + Rnd_FloatValue(seed)) * currentStepSize;

		VSTORE3F(rayOrig + (ray->mint + evaluationPoint) * rayDir, &tmpHitPoint->p.x);

		// Volume segment values
		const float3 sigmaA = HeterogeneousVolume_SigmaA(vol, tmpHitPoint
				TEXTURES_PARAM);
		const float3 sigmaS = HeterogeneousVolume_SigmaS(vol, tmpHitPoint
				TEXTURES_PARAM);
		const float3 emission = Volume_Emission(vol, tmpHitPoint
				TEXTURES_PARAM);

		// Evaluate the current segment like if it was an homogenous volume
		//
		// This could be optimized by inlining the code and exploiting
		// exp(a) * exp(b) = exp(a + b) in order to evaluate a single exp() at
		// the end instead of one each step.
		// However the code would be far less simple and readable.
		float3 segmentTransmittance, segmentEmission;
		const float scatterDistance = HomogeneousVolume_SegmentScatter(Rnd_FloatValue(seed), scatterAllowed,
				currentStepSize, &sigmaA, &sigmaS, &emission,
				&segmentTransmittance, &segmentEmission);

		// I need to update first connectionEmission and than connectionThroughput
		*connectionEmission += *connectionThroughput * emission;
		*connectionThroughput *= segmentTransmittance;

		if (scatterDistance >= 0.f)
			return ray->mint + s * currentStepSize + scatterDistance;
	}

	return -1.f;
}

// Jittered ray-marching estimate of the whole-segment transmittance
OPENCL_FORCE_INLINE float3 HeterogeneousVolume_MarchTransmittance(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThroughEvent, __global HitPoint *tmpHitPoint,
		Seed *seed
		TEXTURES_PARAM_DECL) {
	const float stepSize = vol->volume.heterogenous.stepSize;
	const uint maxStepsCount = vol->volume.heterogenous.maxStepsCount;

	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);

	const float segmentLength = hitT - ray->mint;
	const uint steps = Ceil2UInt(fmin((float)maxStepsCount, segmentLength / stepSize));
	if (steps == 0)
		return WHITE;
	const float currentStepSize = fmin(segmentLength / steps, maxStepsCount * stepSize);

	Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThroughEvent);

	float3 transmittance = WHITE;
	for (uint s = 0; s < steps; ++s) {
		VSTORE3F(rayOrig + (ray->mint + (s + Rnd_FloatValue(seed)) * currentStepSize) * rayDir,
				&tmpHitPoint->p.x);
		transmittance *= Spectrum_Exp(-HeterogeneousVolume_SigmaT(vol, tmpHitPoint
				TEXTURES_PARAM) * currentStepSize);
		if (Spectrum_IsBlack(transmittance))
			break;
	}

	return transmittance;
}

//------------------------------------------------------------------------------
// Null-collision tracking: DDA walk over the majorant grid
//------------------------------------------------------------------------------

typedef struct {
	int voxelX, voxelY, voxelZ;
	float nextCrossingTX, nextCrossingTY, nextCrossingTZ;
	float deltaTX, deltaTY, deltaTZ;
	int stepX, stepY, stepZ;
	int voxelLimitX, voxelLimitY, voxelLimitZ;
	float tEnter, tExit;
	float t, tMax;
	float t0, t1, maj, mn;
	int phase;		// 0=pre-domain, 1=in-grid DDA, 2=post-domain, 3=done
	bool hasGrid, gridInit;
} VolMajorantWalk;

OPENCL_FORCE_INLINE void VolWalk_Init(VolMajorantWalk *w,
		__global const Volume *vol,
		const float3 rayOrig, const float3 rayDir,
		const float mint, const float tEnd) {
	w->t = mint;
	w->tMax = tEnd;
	w->phase = 0;
	w->gridInit = false;

	__global const HeterogenousVolumeParam *p = &vol->volume.heterogenous;

	// Slab test of the ray against the grid domain
	const float3 bmin = (float3)(p->majorantBBoxMinX, p->majorantBBoxMinY, p->majorantBBoxMinZ);
	const float3 bmax = (float3)(p->majorantBBoxMaxX, p->majorantBBoxMaxY, p->majorantBBoxMaxZ);
	const float3 invD = 1.f / rayDir;
	const float3 ta = (bmin - rayOrig) * invD;
	const float3 tb = (bmax - rayOrig) * invD;
	const float3 tmin3 = fmin(ta, tb);
	const float3 tmax3 = fmax(ta, tb);
	const float tEnter = fmax(fmax(tmin3.x, tmin3.y), tmin3.z);
	const float tExit = fmin(fmin(tmax3.x, tmax3.y), tmax3.z);

	w->hasGrid = (tExit > tEnter) && !isnan(tEnter) && !isnan(tExit);
	if (w->hasGrid) {
		w->tEnter = clamp(tEnter, mint, tEnd);
		w->tExit = clamp(tExit, mint, tEnd);
		w->hasGrid = (w->tExit > w->tEnter);
	}
	if (!w->hasGrid) {
		w->tEnter = tEnd;
		w->tExit = tEnd;
	}
}

OPENCL_FORCE_INLINE float VolWalk_CellMajorant(__global const Volume *vol,
		const int x, const int y, const int z,
		__global const float* restrict volMajorants) {
	__global const HeterogenousVolumeParam *p = &vol->volume.heterogenous;
	// Cells are stored as (minorant, majorant) pairs
	return volMajorants[p->majorantOffset +
			(((uint)z * p->majorantResY + (uint)y) * p->majorantResX + (uint)x) * 2 + 1];
}

OPENCL_FORCE_INLINE float VolWalk_CellMinorant(__global const Volume *vol,
		const int x, const int y, const int z,
		__global const float* restrict volMajorants) {
	__global const HeterogenousVolumeParam *p = &vol->volume.heterogenous;
	return volMajorants[p->majorantOffset +
			(((uint)z * p->majorantResY + (uint)y) * p->majorantResX + (uint)x) * 2];
}

OPENCL_FORCE_INLINE bool VolWalk_Next(VolMajorantWalk *w,
		__global const Volume *vol,
		const float3 rayOrig, const float3 rayDir,
		__global const float* restrict volMajorants) {
	__global const HeterogenousVolumeParam *p = &vol->volume.heterogenous;

	while (true) {
		switch (w->phase) {
			case 0: {
				// Segment before the grid domain (global majorant)
				const float t1 = w->hasGrid ? w->tEnter : w->tMax;
				if (w->t < t1) {
					w->t0 = w->t;
					w->t1 = t1;
					w->maj = p->globalMajorant;
					w->mn = p->globalMinorant;
					w->t = t1;
					return true;
				}
				w->phase = 1;
				break;
			}
			case 1: {
				if (!w->hasGrid || (w->t >= w->tExit)) {
					w->phase = 2;
					break;
				}
				if (!w->gridInit) {
					// 3D DDA setup at the domain entry point
					const float3 pos = rayOrig + w->t * rayDir;
					const float3 bmin = (float3)(p->majorantBBoxMinX,
							p->majorantBBoxMinY, p->majorantBBoxMinZ);
					const float cellSize = p->majorantCellSize;

					const float gx = (pos.x - bmin.x) / cellSize;
					const float gy = (pos.y - bmin.y) / cellSize;
					const float gz = (pos.z - bmin.z) / cellSize;
					w->voxelX = clamp(Floor2Int(gx), 0, (int)p->majorantResX - 1);
					w->voxelY = clamp(Floor2Int(gy), 0, (int)p->majorantResY - 1);
					w->voxelZ = clamp(Floor2Int(gz), 0, (int)p->majorantResZ - 1);

					if (rayDir.x > 0.f) {
						w->stepX = 1;
						w->voxelLimitX = (int)p->majorantResX;
						w->deltaTX = cellSize / rayDir.x;
						w->nextCrossingTX = w->t +
								(bmin.x + (w->voxelX + 1) * cellSize - pos.x) / rayDir.x;
					} else if (rayDir.x < 0.f) {
						w->stepX = -1;
						w->voxelLimitX = -1;
						w->deltaTX = -cellSize / rayDir.x;
						w->nextCrossingTX = w->t +
								(bmin.x + w->voxelX * cellSize - pos.x) / rayDir.x;
					} else {
						w->stepX = 0;
						w->voxelLimitX = (int)p->majorantResX;
						w->deltaTX = INFINITY;
						w->nextCrossingTX = INFINITY;
					}
					if (rayDir.y > 0.f) {
						w->stepY = 1;
						w->voxelLimitY = (int)p->majorantResY;
						w->deltaTY = cellSize / rayDir.y;
						w->nextCrossingTY = w->t +
								(bmin.y + (w->voxelY + 1) * cellSize - pos.y) / rayDir.y;
					} else if (rayDir.y < 0.f) {
						w->stepY = -1;
						w->voxelLimitY = -1;
						w->deltaTY = -cellSize / rayDir.y;
						w->nextCrossingTY = w->t +
								(bmin.y + w->voxelY * cellSize - pos.y) / rayDir.y;
					} else {
						w->stepY = 0;
						w->voxelLimitY = (int)p->majorantResY;
						w->deltaTY = INFINITY;
						w->nextCrossingTY = INFINITY;
					}
					if (rayDir.z > 0.f) {
						w->stepZ = 1;
						w->voxelLimitZ = (int)p->majorantResZ;
						w->deltaTZ = cellSize / rayDir.z;
						w->nextCrossingTZ = w->t +
								(bmin.z + (w->voxelZ + 1) * cellSize - pos.z) / rayDir.z;
					} else if (rayDir.z < 0.f) {
						w->stepZ = -1;
						w->voxelLimitZ = -1;
						w->deltaTZ = -cellSize / rayDir.z;
						w->nextCrossingTZ = w->t +
								(bmin.z + w->voxelZ * cellSize - pos.z) / rayDir.z;
					} else {
						w->stepZ = 0;
						w->voxelLimitZ = (int)p->majorantResZ;
						w->deltaTZ = INFINITY;
						w->nextCrossingTZ = INFINITY;
					}
					w->gridInit = true;
				}

				// Emit the current cell segment: [t, next voxel boundary]
				const uint axis =
						(w->nextCrossingTX < w->nextCrossingTY) ?
						((w->nextCrossingTX < w->nextCrossingTZ) ? 0 : 2) :
						((w->nextCrossingTY < w->nextCrossingTZ) ? 1 : 2);
				const float nextT = (axis == 0) ? w->nextCrossingTX :
						((axis == 1) ? w->nextCrossingTY : w->nextCrossingTZ);
				w->t0 = w->t;
				w->t1 = fmax(w->t, fmin(nextT, w->tExit));
				w->maj = VolWalk_CellMajorant(vol, w->voxelX, w->voxelY, w->voxelZ,
						volMajorants);
				w->mn = VolWalk_CellMinorant(vol, w->voxelX, w->voxelY, w->voxelZ,
						volMajorants);

				w->t = w->t1;
				if (axis == 0) {
					w->voxelX += w->stepX;
					w->nextCrossingTX += w->deltaTX;
					if ((w->t >= w->tExit) || (w->voxelX == w->voxelLimitX))
						w->phase = 2;
				} else if (axis == 1) {
					w->voxelY += w->stepY;
					w->nextCrossingTY += w->deltaTY;
					if ((w->t >= w->tExit) || (w->voxelY == w->voxelLimitY))
						w->phase = 2;
				} else {
					w->voxelZ += w->stepZ;
					w->nextCrossingTZ += w->deltaTZ;
					if ((w->t >= w->tExit) || (w->voxelZ == w->voxelLimitZ))
						w->phase = 2;
				}

				return true;
			}
			case 2: {
				// Segment after the grid domain (global majorant)
				if (w->t < w->tMax) {
					w->t0 = w->t;
					w->t1 = w->tMax;
					w->maj = p->globalMajorant;
					w->mn = p->globalMinorant;
					w->t = w->tMax;
					return true;
				}
				w->phase = 3;
				return false;
			}
			default:
				return false;
		}
	}
}

//------------------------------------------------------------------------------
// Delta tracking free-flight sampling / ratio tracking transmittance.
// Mirrors the CPU implementation (see heterogenous.cpp for references).
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float HeterogeneousVolume_DeltaTrackScatter(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThroughEvent,
		const bool scatteredStart, float3 *connectionThroughput,
		float3 *connectionEmission, __global HitPoint *tmpHitPoint,
		Seed *seed
		TEXTURES_PARAM_DECL) {
	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);

	const bool scatterAllowed = (!scatteredStart || vol->volume.heterogenous.multiScattering);
	const bool hasEmission = (vol->volume.volumeEmissionTexIndex != NULL_INDEX);

	Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThroughEvent);

	// Weighted delta-tracking correction (underestimated majorants)
	float w = 1.f;
	// Spectral correctness factor
	float3 R = WHITE;

	VolMajorantWalk walk;
	VolWalk_Init(&walk, vol, rayOrig, rayDir, ray->mint, hitT);

	uint candidateCount = 0;
	const uint maxCandidateCount = 65536;

	while (VolWalk_Next(&walk, vol, rayOrig, rayDir, volMajorants)) {
		const float maj = walk.maj;
		if (!(maj > 0.f))
			continue;
		const float invMaj = 1.f / maj;
		// Residual decomposition (Novak et al. 2014 applied to collision
		// sampling, mirrors the CPU tracker): the per-cell minorant is a
		// homogeneous exponential stream of real collisions; residual
		// candidates arrive at maj - sigma_c. Dense smooth cells
		// (sigma_c ~= maj) produce almost no residual candidates.
		const float sigmaC = fmin(walk.mn, maj);
		const float resRate = maj - sigmaC;
		const float invResRate = (resRate > 0.f) ? 1.f / resRate : INFINITY;

		float t = walk.t0;
		float tMinor = (scatterAllowed && (sigmaC > 0.f)) ?
				t + (-log(1.f - Rnd_FloatValue(seed)) / sigmaC) : INFINITY;

		while (true) {
			float tCand = INFINITY;
			if (resRate > 0.f) {
				t += -log(1.f - Rnd_FloatValue(seed)) * invResRate;
				tCand = t;
			}
			t = fmin(tCand, tMinor);
			if (t >= walk.t1)
				break;
			if (++candidateCount > maxCandidateCount) {
				*connectionThroughput *= w * R;
				return -1.f;
			}
			const bool minorEvent = (t == tMinor);

			VSTORE3F(rayOrig + t * rayDir, &tmpHitPoint->p.x);
			const float3 sigmaT = HeterogeneousVolume_SigmaT(vol, tmpHitPoint
					TEXTURES_PARAM);
			const float sigmaTf = Spectrum_Filter(sigmaT);

			if (hasEmission)
				*connectionEmission += (*connectionThroughput) * w * R *
						Volume_Emission(vol, tmpHitPoint TEXTURES_PARAM) * invMaj;

			if (scatterAllowed) {
				if (minorEvent) {
					// Minorant-stream collision: unconditionally real;
					// overestimated minorants are weight-corrected.
					if (sigmaTf < sigmaC)
						w *= sigmaTf / sigmaC;
					if (sigmaTf > 0.f)
						R *= sigmaT / sigmaTf;
					*connectionThroughput *= w * R;
					return t;
				}

				const float pAccept = (sigmaTf - sigmaC) * invResRate;
				if ((pAccept >= 1.f) || (Rnd_FloatValue(seed) < pAccept)) {
					// Real collision
					if (pAccept >= 1.f)
						w *= pAccept;
					if (sigmaTf > 0.f)
						R *= sigmaT / sigmaTf;
					*connectionThroughput *= w * R;
					return t;
				}

				// Null collision: R *= (maj - sigma_t) / (maj - sigma_t,f)
				const float invDenom = 1.f / (maj - sigmaTf);
				R *= fmax(BLACK, (maj - sigmaT) * invDenom);
			} else {
				// Residual ratio-tracking transmittance:
				// T = exp(-sigma_c*l) * prod((maj - sigma_t)/(maj - sigma_c))
				R *= fmax(BLACK, (maj - sigmaT) / resRate);
				if (Spectrum_Filter(R) <= 0.f) {
					*connectionThroughput *= w * R;
					return -1.f;
				}
			}
		}

		if (!scatterAllowed && (sigmaC > 0.f))
			R *= exp(-sigmaC * (walk.t1 - walk.t0));
	}

	*connectionThroughput *= w * R;
	return -1.f;
}

OPENCL_FORCE_INLINE float3 HeterogeneousVolume_RatioTrackTransmittance(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThroughEvent, __global HitPoint *tmpHitPoint,
		Seed *seed
		TEXTURES_PARAM_DECL) {
	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);

	Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThroughEvent);

	// Residual ratio tracking with per-cell control extinction
	// (Novak et al. 2014): T = prod(exp(-sigma_c*l) *
	// prod(1 - (sigma_t - sigma_c)/(maj - sigma_c))). Cells with
	// minorant ~= majorant degenerate to a deterministic exponential.
	float3 T = WHITE;
	uint candidateCount = 0;
	const uint maxCandidateCount = 65536;

	VolMajorantWalk walk;
	VolWalk_Init(&walk, vol, rayOrig, rayDir, ray->mint, hitT);

	while (VolWalk_Next(&walk, vol, rayOrig, rayDir, volMajorants)) {
		const float maj = walk.maj;
		if (!(maj > 0.f))
			continue;
		const float sigmaC = fmin(walk.mn, maj);
		const float residualRate = maj - sigmaC;

		const float segLen = walk.t1 - walk.t0;
		if (sigmaC > 0.f)
			T *= exp(-sigmaC * segLen);
		if (!(residualRate > 0.f))
			continue;
		const float invRate = 1.f / residualRate;

		float t = walk.t0;
		while (true) {
			t += -log(1.f - Rnd_FloatValue(seed)) * invRate;
			if (t >= walk.t1)
				break;
			if (++candidateCount > maxCandidateCount)
				return T;

			VSTORE3F(rayOrig + t * rayDir, &tmpHitPoint->p.x);
			const float3 sigmaT = HeterogeneousVolume_SigmaT(vol, tmpHitPoint
					TEXTURES_PARAM);
			T *= fmax(BLACK, WHITE - (sigmaT - sigmaC) * invRate);

			if (Spectrum_Filter(T) <= 1e-6f)
				return BLACK;
		}
	}

	return T;
}

OPENCL_FORCE_INLINE float HeterogeneousVolume_Scatter(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThroughEvent,
		const bool scatteredStart, float3 *connectionThroughput,
		float3 *connectionEmission, __global HitPoint *tmpHitPoint
		TEXTURES_PARAM_DECL) {
	// I need a sequence of pseudo-random numbers starting form a floating point
	// pseudo-random number
	Seed seed;
	Rnd_InitFloat(passThroughEvent, &seed);

	if (vol->volume.heterogenous.deltaTracking &&
			(vol->volume.heterogenous.majorantOffset != NULL_INDEX))
		return HeterogeneousVolume_DeltaTrackScatter(vol, ray, hitT,
				passThroughEvent, scatteredStart, connectionThroughput,
				connectionEmission, tmpHitPoint, &seed
				TEXTURES_PARAM);
	else
		return HeterogeneousVolume_MarchScatter(vol, ray, hitT,
				passThroughEvent, scatteredStart, connectionThroughput,
				connectionEmission, tmpHitPoint, &seed
				TEXTURES_PARAM);
}

//------------------------------------------------------------------------------
// Volume scatter
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE float Volume_Scatter(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThrough,
		const bool scatteredStart, float3 *connectionThroughput,
		float3 *connectionEmission, __global HitPoint *tmpHitPoint
		TEXTURES_PARAM_DECL) {
	switch (vol->type) {
		case CLEAR_VOL:
			return ClearVolume_Scatter(vol, ray, hitT,
					passThrough, scatteredStart,
					connectionThroughput, connectionEmission, tmpHitPoint
					TEXTURES_PARAM);
		case HOMOGENEOUS_VOL:
			return HomogeneousVolume_Scatter(vol, ray, hitT,
					passThrough, scatteredStart,
					connectionThroughput, connectionEmission, tmpHitPoint
					TEXTURES_PARAM);
		case HETEROGENEOUS_VOL:
			return HeterogeneousVolume_Scatter(vol, ray, hitT,
					passThrough, scatteredStart,
					connectionThroughput, connectionEmission, tmpHitPoint
					TEXTURES_PARAM);
		default:
			return -1.f;
	}
}

//------------------------------------------------------------------------------
// Volume transmittance estimate (shadow rays)
//------------------------------------------------------------------------------

OPENCL_FORCE_NOT_INLINE float3 Volume_TransmittanceEstimate(__global const Volume *vol,
		__global Ray *ray, const float hitT,
		const float passThrough, __global HitPoint *tmpHitPoint
		TEXTURES_PARAM_DECL) {
	const float3 rayOrig = VLOAD3F(&ray->o.x);
	const float3 rayDir = VLOAD3F(&ray->d.x);

	switch (vol->type) {
		case CLEAR_VOL: {
			Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThrough);
			const float3 sigmaT = ClearVolume_SigmaT(vol, tmpHitPoint
					TEXTURES_PARAM);
			if (Spectrum_IsBlack(sigmaT))
				return WHITE;
			const float3 tau = clamp((hitT - ray->mint) * sigmaT, 0.f, INFINITY);
			return Spectrum_Exp(-tau);
		}
		case HOMOGENEOUS_VOL: {
			Volume_InitializeTmpHitPoint(tmpHitPoint, rayOrig, rayDir, passThrough);
			const float3 sigmaT = HomogeneousVolume_SigmaA(vol, tmpHitPoint
					TEXTURES_PARAM) +
					HomogeneousVolume_SigmaS(vol, tmpHitPoint TEXTURES_PARAM);
			if (Spectrum_IsBlack(sigmaT) || (hitT <= ray->mint))
				return WHITE;
			const float3 tau = clamp((hitT - ray->mint) * sigmaT, 0.f, INFINITY);
			return Spectrum_Exp(-tau);
		}
		case HETEROGENEOUS_VOL: {
			Seed seed;
			Rnd_InitFloat(passThrough, &seed);
			if (vol->volume.heterogenous.deltaTracking &&
					(vol->volume.heterogenous.majorantOffset != NULL_INDEX))
				return HeterogeneousVolume_RatioTrackTransmittance(vol, ray, hitT,
						passThrough, tmpHitPoint, &seed
						TEXTURES_PARAM);
			else
				return HeterogeneousVolume_MarchTransmittance(vol, ray, hitT,
						passThrough, tmpHitPoint, &seed
						TEXTURES_PARAM);
		}
		default:
			return WHITE;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
