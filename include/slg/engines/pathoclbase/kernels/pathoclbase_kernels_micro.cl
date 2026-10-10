#line 2 "pathoclbase_kernels_micro.cl"

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
// AdvancePaths (Micro-Kernels)
//------------------------------------------------------------------------------

//#define DEBUG_PRINTF_KERNEL_NAME 1

// Adaptive aim radius for a focus-ring target: the cone should cover the
// recorded target's local neighbourhood, so it is sized by the distance
// to the nearest entry already in the ring. A dense cluster (a real
// caustic hotspot) yields a tight cone; isolated/spread receivers yield
// a broad one - self-tuning the radius instead of a fixed global value.
// Clamped to [focus.radius, worldRadius]: the property is now the
// tightest allowed cone, and an empty ring bootstraps to the broad cap.
OPENCL_FORCE_INLINE float FocusAimRadius(
		__global const float4 *ring, const uint nValid,
		const float px, const float py, const float pz,
		const float minR, const float maxR) {
	float nn2 = INFINITY;
	for (uint e = 0; e < nValid; ++e) {
		const float4 q = ring[e];
		const float dx = q.x - px, dy = q.y - py, dz = q.z - pz;
		nn2 = fmin(nn2, dx * dx + dy * dy + dz * dz);
	}
	// The ring is written lock-free, so a scanned entry may be mid-update;
	// a torn read can yield NaN. Fold that (and the empty-ring INF) to the
	// broad cap; a genuine tight cluster (r < minR) keeps the floor.
	const float r = sqrt(nn2);
	return (r == r) ? clamp(r, minR, maxR) : maxR;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_RT_NEXT_VERTEX
// To: MK_HIT_NOTHING or MK_HIT_OBJECT or MK_RT_NEXT_VERTEX
//------------------------------------------------------------------------------

// Experimental nonlocal Cycles random walk. Each free flight is resolved by
// the normal hardware trace pass; foreign hits retain the distance proposal.
OPENCL_FORCE_INLINE float CyclesBSSRDF_Random(__global CyclesBSSRDFState *s) {
	Seed seed = s->seed;
	const float u = Rnd_FloatValue(&seed);
	s->seed = seed;
	return u;
}

OPENCL_FORCE_INLINE float CyclesBSSRDF_Alpha(const float albedo, const float g) {
	const float a = 4.20863f * albedo + 4.09712f - sqrt(9.59217f + 41.6808f * albedo + 17.7126f * albedo * albedo);
	return clamp((1.f - a * a) / (1.f - g * a * a), 0.f, .999999f);
}

OPENCL_FORCE_INLINE float3 CyclesBSSRDF_GGX(const float3 v, const float alpha, const float u0, const float u1) {
	if (alpha == 0.f) return MAKE_FLOAT3(0.f, 0.f, 1.f);
	const float3 vh = normalize(MAKE_FLOAT3(alpha * v.x, alpha * v.y, v.z));
	const float l = vh.x * vh.x + vh.y * vh.y;
	const float3 t1 = l > 0.f ? MAKE_FLOAT3(-vh.y, vh.x, 0.f) / sqrt(l) : MAKE_FLOAT3(1.f, 0.f, 0.f);
	const float3 t2 = cross(vh, t1);
	const float r = sqrt(u0), phi = 2.f * M_PI_F * u1;
	const float x = r * cos(phi), mix = .5f * (1.f + vh.z);
	const float y = (1.f - mix) * sqrt(fmax(0.f, 1.f - x * x)) + mix * r * sin(phi);
	const float3 n = x * t1 + y * t2 + sqrt(fmax(0.f, 1.f - x * x - y * y)) * vh;
	return normalize(MAKE_FLOAT3(alpha * n.x, alpha * n.y, fmax(0.f, n.z)));
}

OPENCL_FORCE_INLINE float3 CyclesBSSRDF_Phase(const float3 direction, const float g, const float u0, const float u1) {
	float c;
	if (fabs(g) < 1e-6f) c = 1.f - 2.f * u0;
	else {
		const float a = (1.f - g * g) / (1.f - g + 2.f * g * u0);
		c = clamp((1.f + g * g - a * a) / (2.f * g), -1.f, 1.f);
	}
	float3 x, y;
	CoordinateSystem(direction, &x, &y);
	const float d = sqrt(fmax(0.f, 1.f - c * c)), phi = 2.f * M_PI_F * u1;
	return normalize(d * cos(phi) * x + d * sin(phi) * y + c * direction);
}

OPENCL_FORCE_INLINE bool CyclesBSSRDF_QueueFlight(__global CyclesBSSRDFState *s, __global Ray *ray) {
	const float3 weight = VLOAD3F(s->throughput.c), alpha = VLOAD3F(s->alpha.c);
	float3 probabilities = fmax(weight * alpha, BLACK);
	const float sum = probabilities.x + probabilities.y + probabilities.z;
	if (!(sum > 0.f)) return false;
	probabilities /= sum;
	VSTORE3F(probabilities, s->probabilities.c);
	float selector = CyclesBSSRDF_Random(s);
	uint channel = 2u;
	if (selector < probabilities.x) channel = 0u;
	else if (selector - probabilities.x < probabilities.y) channel = 1u;
	// Metal has no log1p. Correct log(1-u) for the rounded subtraction;
	// if it rounded to one, u is the correctly rounded first-order value.
	const float u = fmin(CyclesBSSRDF_Random(s), .99999994f), complement = 1.f - u;
	const float opticalDistance = complement == 1.f ? u : -log(complement) * (u / (1.f - complement));
	s->distance = opticalDistance / s->sigmaT.c[channel];
	ray->mint = MachineEpsilon_E(0.f);
	ray->maxt = s->distance;
	return true;
}

// Returns 0 for the local-radius branch, 1 for a queued walk and -1 for
// absorption/invalid entry. Local/nonlocal choice is probability compensated.
OPENCL_FORCE_INLINE int CyclesBSSRDF_Start(__global GPUTask *task, __global GPUTaskState *state,
		__global Ray *ray MATERIALS_PARAM_DECL) {
	__global BSDF *bsdf = &state->bsdf;
	uint matIndex = bsdf->materialIndex;
	if (!mats[matIndex].hasCyclesBSSRDF) return 0;
	__global CyclesBSSRDFState *s = (__global CyclesBSSRDFState *)&task->tmpHitPoint;
	s->seed = state->seedPassThroughEvent;
	(void)CyclesBSSRDF_Random(s);
	for (uint depth = 0u; depth < 64u; ++depth) {
		if (!mats[matIndex].hasCyclesBSSRDF || mats[matIndex].type == CYCLES_BSSRDF) break;
		if (mats[matIndex].type == TWOSIDED)
			matIndex = bsdf->hitPoint.intoObject ? mats[matIndex].twosided.frontMatIndex : mats[matIndex].twosided.backMatIndex;
		else if (mats[matIndex].type == MIX) {
			const bool additive = mats[matIndex].mix.additive;
			const float amount = additive ? .5f : clamp(Texture_GetFloatValue(
					mats[matIndex].mix.mixFactorTexIndex, &bsdf->hitPoint TEXTURES_PARAM), 0.f, 1.f);
			matIndex = CyclesBSSRDF_Random(s) < 1.f - amount ? mats[matIndex].mix.matAIndex : mats[matIndex].mix.matBIndex;
			if (additive) VSTORE3F(2.f * VLOAD3F(state->throughput.c), state->throughput.c);
		} else return -1;
		bsdf->materialIndex = matIndex;
		bsdf->nullSelectionConditioned = false;
	}
	if (mats[matIndex].type != CYCLES_BSSRDF)
		return mats[matIndex].hasCyclesBSSRDF ? -1 : 0;
	__global const CyclesBSSRDFParam *p = &mats[matIndex].cyclesbssrdf;
	const float scale = Texture_GetFloatValue(p->scaleTexIndex, &bsdf->hitPoint TEXTURES_PARAM);
	const float3 radius = fmax(Texture_GetSpectrumValue(p->radiusTexIndex, &bsdf->hitPoint TEXTURES_PARAM) * scale, BLACK);
	task->tmpBsdf = *bsdf;
	task->tmpBsdf.hitPoint.spectralEmissionEval = 2u;
	const float3 radiusRGB = fmax(Texture_GetSpectrumValue(p->radiusTexIndex, &task->tmpBsdf.hitPoint TEXTURES_PARAM) * scale, BLACK);
	task->tmpBsdf.hitPoint.spectralEmissionEval = bsdf->hitPoint.spectralEmissionEval;
	if (radiusRGB.x < 1e-8f && radiusRGB.y < 1e-8f && radiusRGB.z < 1e-8f) return 0;
	const float3 color = clamp(Texture_GetSpectrumValue(p->kdTexIndex, &bsdf->hitPoint TEXTURES_PARAM), BLACK, WHITE);
	float3 throughput = MAKE_FLOAT3(color.x > 0.f ? 1.f : 0.f, color.y > 0.f ? 1.f : 0.f, color.z > 0.f ? 1.f : 0.f);
	if (radiusRGB.x < 1e-8f || radiusRGB.y < 1e-8f || radiusRGB.z < 1e-8f) {
		const float total = color.x + color.y + color.z;
		const float local = (radiusRGB.x < 1e-8f ? color.x : 0.f) + (radiusRGB.y < 1e-8f ? color.y : 0.f) + (radiusRGB.z < 1e-8f ? color.z : 0.f);
		const float probability = total > 0.f ? local / total : 1.f;
		if (CyclesBSSRDF_Random(s) < probability) {
			const float3 mask = MAKE_FLOAT3(radiusRGB.x < 1e-8f ? 1.f / probability : 0.f, radiusRGB.y < 1e-8f ? 1.f / probability : 0.f, radiusRGB.z < 1e-8f ? 1.f / probability : 0.f);
			VSTORE3F(VLOAD3F(state->throughput.c) * mask, state->throughput.c);
			return 0;
		}
		throughput = MAKE_FLOAT3(radiusRGB.x < 1e-8f ? 0.f : throughput.x / (1.f - probability), radiusRGB.y < 1e-8f ? 0.f : throughput.y / (1.f - probability), radiusRGB.z < 1e-8f ? 0.f : throughput.z / (1.f - probability));
	}
	s->anisotropy = clamp(Texture_GetFloatValue(p->anisotropyTexIndex, &bsdf->hitPoint TEXTURES_PARAM), -.99f, .99f);
	float3 alpha = MAKE_FLOAT3(CyclesBSSRDF_Alpha(color.x, s->anisotropy), CyclesBSSRDF_Alpha(color.y, s->anisotropy), CyclesBSSRDF_Alpha(color.z, s->anisotropy));
	VSTORE3F(alpha, s->alpha.c);
	VSTORE3F(throughput, s->throughput.c);
	for (uint i = 0u; i < 3u; ++i) {
		// CUDA's float3 has no operator[]. Spectrum's scalar storage is
		// shared by all backends, with explicit vector component reads.
		const float rgbRadius = i == 0u ? radiusRGB.x : (i == 1u ? radiusRGB.y : radiusRGB.z);
		const float transportRadius = i == 0u ? radius.x : (i == 1u ? radius.y : radius.z);
		const bool local = rgbRadius < 1e-8f;
		s->sigmaT.c[i] = local ? 1.f : 1.f / fmax(transportRadius, 1e-16f);
		if (local) s->alpha.c[i] = 0.f;
		else if (s->alpha.c[i] < .2f) { s->throughput.c[i] *= s->alpha.c[i] / .2f; s->alpha.c[i] = .2f; }
	}
	const float roughness = clamp(Texture_GetFloatValue(p->roughnessTexIndex, &bsdf->hitPoint TEXTURES_PARAM), 0.f, 1.f);
	const float ior = clamp(Texture_GetFloatValue(p->iorTexIndex, &bsdf->hitPoint TEXTURES_PARAM), 1.01f, 3.8f);
	const float3 wo = Frame_ToLocal(&bsdf->frame, VLOAD3F(&bsdf->hitPoint.fixedDir.x));
	if (!(wo.z > 0.f)) return -1;
	const float3 h = CyclesBSSRDF_GGX(wo, roughness, CyclesBSSRDF_Random(s), CyclesBSSRDF_Random(s));
	const float eta = 1.f / ior, cosHI = dot(h, wo);
	const float cosHT = sqrt(fmax(0.f, 1.f - eta * eta * (1.f - cosHI * cosHI)));
	const float3 direction = normalize(Frame_ToWorld(&bsdf->frame, -eta * wo + (eta * cosHI - cosHT) * h));
	if (!(dot(direction, VLOAD3F(&bsdf->hitPoint.geometryN.x)) < 0.f)) return -1;
	const float time = ray->time;
	Ray_Init2(ray, BSDF_GetRayOrigin(bsdf, direction), direction, time);
	s->entryMesh = bsdf->sceneObjectIndex;
	s->groupIndex = sceneObjs[s->entryMesh].subsurfaceGroupIndex;
	s->bounce = 0u;
	if (!CyclesBSSRDF_QueueFlight(s, ray)) return -1;
	bsdf->cyclesBSSRDFPhase = 1u;
	state->state = MK_RT_NEXT_VERTEX;
	return 1;
}

OPENCL_FORCE_INLINE void CyclesBSSRDF_Resolve(__global GPUTask *task, __global GPUTaskState *state,
		__global EyePathInfo *pathInfo, __global Ray *ray, __global RayHit *hit MATERIALS_PARAM_DECL) {
	__global CyclesBSSRDFState *s = (__global CyclesBSSRDFState *)&task->tmpHitPoint;
	bool escaped = hit->meshIndex != NULL_INDEX && (hit->meshIndex == s->entryMesh ||
			(s->groupIndex != NULL_INDEX && sceneObjs[hit->meshIndex].subsurfaceGroupIndex == s->groupIndex));
	if (hit->meshIndex != NULL_INDEX && !escaped) {
		const float next = hit->t + MachineEpsilon_E(hit->t);
		if (next > ray->mint && next < s->distance) { ray->mint = next; return; }
	}
	const float distance = escaped ? hit->t : s->distance;
	const float3 sigmaT = VLOAD3F(s->sigmaT.c), alpha = VLOAD3F(s->alpha.c);
	const float3 transmittance = Spectrum_Exp(-sigmaT * distance);
	const float3 factor = transmittance * (escaped ? WHITE : sigmaT);
	const float3 probabilities = VLOAD3F(s->probabilities.c);
	const float pdf = dot(probabilities, factor);
	if (!(pdf > 0.f) || !isfinite(pdf)) { state->state = MK_SPLAT_SAMPLE; return; }
	float3 throughput = VLOAD3F(s->throughput.c) * factor / pdf;
	if (!escaped) throughput *= alpha;
	if (escaped) {
		VSTORE3F(VLOAD3F(state->throughput.c) * throughput, state->throughput.c);
		BSDF_Init(&state->bsdf, false, ray, hit, CyclesBSSRDF_Random(s), &pathInfo->volume MATERIALS_PARAM);
		VSTORE3F(VLOAD3F(&state->bsdf.hitPoint.geometryN.x), &state->bsdf.hitPoint.fixedDir.x);
		state->bsdf.hitPoint.intoObject = true;
		state->bsdf.triangleLightSourceIndex = NULL_INDEX;
		state->bsdf.nullSelectionConditioned = false;
		state->bsdf.cyclesBSSRDFPhase = 2u;
		state->state = MK_HIT_OBJECT;
		return;
	}
	if (s->bounce >= 8u) {
		const float survival = clamp(fmax(throughput.x, fmax(throughput.y, throughput.z)), .05f, .95f);
		if (CyclesBSSRDF_Random(s) >= survival) { state->state = MK_SPLAT_SAMPLE; return; }
		throughput /= survival;
	}
	VSTORE3F(throughput, s->throughput.c);
	if (++s->bounce >= 256u) { state->state = MK_SPLAT_SAMPLE; return; }
	const float3 origin = VLOAD3F(&ray->o.x) + distance * VLOAD3F(&ray->d.x);
	const float3 direction = CyclesBSSRDF_Phase(VLOAD3F(&ray->d.x), s->anisotropy, CyclesBSSRDF_Random(s), CyclesBSSRDF_Random(s));
	const float time = ray->time;
	Ray_Init2(ray, origin, direction, time);
	if (!CyclesBSSRDF_QueueFlight(s, ray)) state->state = MK_SPLAT_SAMPLE;
}

__kernel void AdvancePaths_MK_RT_NEXT_VERTEX(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	// This has to be done by the first kernel to run after RT kernel.
	// Under wavefront queues, AdvancePaths_BuildQueues already accounts
	// the traced ray for every task, so skip it here.
	if (!wavefrontEnable)
		sampleResult->rayCount += 1;

	// Read the path state
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_RT_NEXT_VERTEX(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_RT_NEXT_VERTEX)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------
	
	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	if (taskState->bsdf.cyclesBSSRDFPhase == 1u) {
		CyclesBSSRDF_Resolve(&tasks[gid], taskState, pathInfo, &rays[gid], &rayHits[gid] MATERIALS_PARAM);
		return;
	}

	float3 connectionThroughput;

	Seed seedPassThroughEvent = taskState->seedPassThroughEvent;
	const float passThroughEvent = Rnd_FloatValue(&seedPassThroughEvent);
	taskState->seedPassThroughEvent = seedPassThroughEvent;

	int throughShadowTransparency = taskState->throughShadowTransparency;
	const bool continueToTrace = Scene_Intersect(taskConfig,
			EYE_RAY | ((pathInfo->depth.depth == 0) ? CAMERA_RAY : INDIRECT_RAY),
			&pathInfo->depth, pathInfo->lastBSDFEvent,
			&throughShadowTransparency,
			&pathInfo->volume,
			&tasks[gid].tmpHitPoint,
			passThroughEvent,
			&rays[gid], &rayHits[gid], &taskState->bsdf,
			&connectionThroughput, VLOAD3F(taskState->throughput.c),
			sampleResult,
			false
			MATERIALS_PARAM
			);
	taskState->throughShadowTransparency = throughShadowTransparency;
	VSTORE3F(connectionThroughput * VLOAD3F(taskState->throughput.c), taskState->throughput.c);

	// If continueToTrace, there is nothing to do, just keep the same state
	if (!continueToTrace) {
		// ReSTIR PT (PT-2): the armed pick's landing vertex (or miss) is
		// the reconnection vertex x2. Scene_Intersect has already folded
		// pass-throughs and the connection throughput, so this entry is
		// the WALKED landing - record what the path actually hit and
		// snapshot the pre-contribution ledger (radiance has not yet
		// received this vertex's emission/DL terms, matching the CPU
		// capture between Intersect() and the contribution section).
		if (taskConfig->pathTracer.restirPT.enabled &&
				(taskConfig->pathTracer.restirPT.ptCandCount > 0u)) {
			const uint ptK = taskConfig->pathTracer.restirPT.ptCandCount;
			__global RestirPTResult *ptResult = (__global RestirPTResult *)
					(((__global RestirGICandidate *)(restirReservoirs +
					taskConfig->pathTracer.restirPT.ptCandDataOffset +
					gid * taskConfig->pathTracer.restirPT.ptCandStride)) +
					ptK);
			if (ptResult->pending == 3u) {
				const bool missed =
						(rayHits[gid].meshIndex == NULL_INDEX);
				ptResult->pMiss = missed ? 1u : 0u;
				if (!missed) {
					VSTORE3F(VLOAD3F(&taskState->bsdf.hitPoint.p.x),
							&ptResult->pX2X);
					VSTORE3F(VLOAD3F(&taskState->bsdf.hitPoint.geometryN.x),
							&ptResult->pX2nX);
				}
				VSTORE3F(VLOAD3F(&rays[gid].d.x), &ptResult->dirX);
				VSTORE3F(VLOAD3F(taskState->throughput.c),
						&ptResult->thrR);
				VSTORE3F(VLOAD3F(&sampleResult->
						radiancePerPixelNormalized[0].c[0]),
						&ptResult->radR);
				ptResult->pending = 4u;
			}
		}
		if (rayHits[gid].meshIndex == NULL_INDEX)
			taskState->state = MK_HIT_NOTHING;
		else {
			const BSDFEvent eventTypes = BSDF_GetEventTypes(&taskState->bsdf
					MATERIALS_PARAM);

			sampleResult->lastPathVertex = PathDepthInfo_IsLastPathVertexVol(&pathInfo->depth,
					&taskConfig->pathTracer.maxPathDepth, eventTypes, taskState->bsdf.isVolume);

			taskState->state = MK_HIT_OBJECT;
		}
	}
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_HIT_NOTHING
// To: MK_SPLAT_SAMPLE
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_HIT_NOTHING(
		KERNEL_ARGS
		KERNEL_ARGS_VC
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_HIT_NOTHING(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_HIT_NOTHING)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	// Nothing was hit, add environmental lights radiance

	bool checkDirectLightHit = true;
	
	checkDirectLightHit = checkDirectLightHit &&
			(!(taskConfig->pathTracer.forceBlackBackground && pathInfo->isPassThroughPath) || !pathInfo->isPassThroughPath);

	checkDirectLightHit = checkDirectLightHit &&
			// Avoid to render caustic path if hybridBackForwardEnable
			(!taskConfig->pathTracer.hybridBackForward.enabled ||
			(taskConfig->pathTracer.hybridBackForward.adaptiveCaustic ?
				// Env lights have infinite solid angle: only a delta
				// terminal makes the connection eye-hard
				!EyePathInfo_IsAdaptiveCausticHitPath(pathInfo,
						taskConfig->pathTracer.hybridBackForward.terminalGlossiness,
						taskConfig->pathTracer.hybridBackForward.connectProb,
						INFINITY) :
				!EyePathInfo_IsCausticPath(pathInfo)));

	checkDirectLightHit = checkDirectLightHit &&
			((!taskConfig->pathTracer.pgic.indirectEnabled && !taskConfig->pathTracer.pgic.causticEnabled) ||
			PhotonGICache_IsDirectLightHitVisible(taskConfig, pathInfo, taskState->photonGICausticCacheUsed,
					taskState->photonGICausticChainCached));

	if (checkDirectLightHit) {
		DirectHitInfiniteLight(
				&taskConfig->film,
				taskConfig,
				worldRadius,
				emitLightsDistribution,
				pathInfo,
				&taskState->throughput,
				&rays[gid],
				sampleResult->firstPathVertex ? NULL : &taskState->bsdf,
				sampleResult
				LPE_PARAM
				LIGHTS_PARAM);
	}

	if (pathInfo->depth.depth == 0) {
		// Scene_Intersect accumulated the primary-ray opacity.
		sampleResult->depth = INFINITY;
		sampleResult->position.x = INFINITY;
		sampleResult->position.y = INFINITY;
		sampleResult->position.z = INFINITY;
		sampleResult->geometryNormal.x = 0.f;
		sampleResult->geometryNormal.y = 0.f;
		sampleResult->geometryNormal.z = 0.f;
		sampleResult->shadingNormal.x = 0.f;
		sampleResult->shadingNormal.y = 0.f;
		sampleResult->shadingNormal.z = 0.f;
		sampleResult->materialID = 0;
		sampleResult->objectID = 0;
		sampleResult->cryptoObjectID = 0.f;
		sampleResult->cryptoMaterialID = 0.f;
		sampleResult->uv.u = INFINITY;
		sampleResult->uv.v = INFINITY;
		sampleResult->isHoldout = false;
		if (taskConfig->film.hasChannelMotionVector) {
			__global const Ray *ray = &rays[gid];
			Camera_ComputeEnvMotionVector(camera,
					VLOAD3F(&ray->o.x), VLOAD3F(&ray->d.x), ray->time,
					filmHeight, sampleResult->motionVector);
		}
	} else if (!sampleResult->isHoldout && pathInfo->isTransmittedPath) {
		// I set to 0.0 also the alpha all purely transmitted paths hitting nothing
		sampleResult->alpha = 0.f;
	}

	taskState->state = MK_SPLAT_SAMPLE;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_HIT_OBJECT
// To: MK_DL_ILLUMINATE or MK_SPLAT_SAMPLE
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_HIT_OBJECT(
		KERNEL_ARGS
		KERNEL_ARGS_VC
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_HIT_OBJECT(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_HIT_OBJECT)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global BSDF *bsdf = &taskState->bsdf;
	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	// Something was hit

	if (bsdf->cyclesBSSRDFPhase != 2u && taskState->albedoToDo && BSDF_IsAlbedoEndPoint(bsdf, taskConfig->pathTracer.albedo.specularSetting,
			taskConfig->pathTracer.albedo.specularGlossinessThreshold MATERIALS_PARAM)) {
		const float3 albedo = VLOAD3F(taskState->throughput.c) * BSDF_Albedo(bsdf
				MATERIALS_PARAM);
		VSTORE3F(albedo, sampleResult->albedo.c);
		sampleResult->shadingNormal = bsdf->hitPoint.shadeN;

		taskState->albedoToDo = false;
	}

	if (bsdf->cyclesBSSRDFPhase != 2u && pathInfo->depth.depth == 0) {
		const bool isHoldout = BSDF_IsHoldout(bsdf
				MATERIALS_PARAM);
		if (isHoldout) sampleResult->alpha = 0.f;
		else if (bsdf->isVolume) sampleResult->alpha = 1.f;
		sampleResult->depth = rayHits[gid].t;
		sampleResult->position = bsdf->hitPoint.p;
		sampleResult->geometryNormal = bsdf->hitPoint.geometryN;
		sampleResult->materialID = BSDF_GetMaterialID(bsdf
				MATERIALS_PARAM);
		sampleResult->objectID = BSDF_GetObjectID(bsdf, sceneObjs);
		sampleResult->cryptoObjectID = BSDF_GetCryptoObjectID(bsdf);
		sampleResult->cryptoMaterialID = BSDF_GetCryptoMaterialID(bsdf
				MATERIALS_PARAM);
		sampleResult->uv = bsdf->hitPoint.defaultUV;
		sampleResult->isHoldout = isHoldout;
		if (taskConfig->film.hasChannelMotionVector)
			PathOCL_ComputeFirstHitMotionVector(camera, meshDescs,
					interpolatedTransforms, &bsdf->hitPoint,
					rays[gid].time, filmHeight, sampleResult->motionVector);
	}

	if (taskConfig->pathTracer.vertexConnect.enabled) {
		// Vertex connection (M6) eye-vertex MIS fold (BiDirCPURenderThread
		// eye loop): dVCM *= MIS(t^2)/MIS(cos), dVC *= 1/MIS(cos). The
		// first-vertex t gains the clipHither offset (CPU t_MIS: the hit
		// t is measured from the clip start).
		const float t_MIS = rayHits[gid].t +
				((pathInfo->depth.depth == 0) ? camera->base.hither : 0.f);
		// A medium vertex has no surface: its area measure carries no
		// cosine (pdfA = pdfW / d^2) - a fake shading-normal cosine made the
		// strategy weights of one path not sum to 1 (dense media 3x bright)
		const float factor = bsdf->isVolume ? 1.f : (1.f / VCMis(fabs(dot(
				VLOAD3F(&bsdf->hitPoint.shadeN.x),
				VLOAD3F(&rays[gid].d.x)))));
		pathInfo->vcFoldVCM = VCMis(t_MIS * t_MIS) * factor;
		pathInfo->vcFoldVC = factor;
		pathInfo->dVCM *= pathInfo->vcFoldVCM;
		pathInfo->dVC *= pathInfo->vcFoldVC;
		// dVM folds with the same 1/MIS(|cos|) factor as dVC (CPU
		// Bounce: dVM *= factor) - it shares vcFoldVC
		pathInfo->dVM *= pathInfo->vcFoldVC;
	}

	//----------------------------------------------------------------------
	// Check if it is a baked material
	//----------------------------------------------------------------------

	if (BSDF_HasBakeMap(bsdf, COMBINED MATERIALS_PARAM)) {
		const float3 radiance = VLOAD3F(&taskState->throughput.c[0]) * BSDF_GetBakeMapValue(bsdf MATERIALS_PARAM);
		VADD3F(sampleResult->radiancePerPixelNormalized[0].c, radiance);

		taskState->state = MK_SPLAT_SAMPLE;
		return;
	} else if (BSDF_HasBakeMap(bsdf, LIGHTMAP MATERIALS_PARAM)) {
		const float3 radiance = VLOAD3F(&taskState->throughput.c[0]) *
				BSDF_Albedo(bsdf MATERIALS_PARAM) *
				BSDF_GetBakeMapValue(bsdf MATERIALS_PARAM);
		VADD3F(sampleResult->radiancePerPixelNormalized[0].c, radiance);

		taskState->state = MK_SPLAT_SAMPLE;
		return;
	}

	//--------------------------------------------------------------------------
	// Check if it is a light source and I have to add light emission
	//--------------------------------------------------------------------------

	bool checkDirectLightHit = true;

	checkDirectLightHit = checkDirectLightHit &&
			// Avoid to render caustic path if hybridBackForwardEnable
			(!taskConfig->pathTracer.hybridBackForward.enabled ||
			(taskConfig->pathTracer.hybridBackForward.adaptiveCaustic ?
				// Only triangle lights are hittable; the hit BSDF carries
				// the light index. The terminal vertex is the ray origin.
				!(BSDF_IsLightSource(bsdf) &&
					EyePathInfo_IsAdaptiveCausticHitPath(pathInfo,
						taskConfig->pathTracer.hybridBackForward.terminalGlossiness,
						taskConfig->pathTracer.hybridBackForward.connectProb,
						Light_ConnectionSolidAngle(&lights[bsdf->triangleLightSourceIndex],
								VLOAD3F(&rays[gid].o.x)))) :
				!EyePathInfo_IsCausticPath(pathInfo)));

	checkDirectLightHit = checkDirectLightHit &&
			((!taskConfig->pathTracer.pgic.indirectEnabled && !taskConfig->pathTracer.pgic.causticEnabled) ||
			PhotonGICache_IsDirectLightHitVisible(taskConfig, pathInfo, taskState->photonGICausticCacheUsed,
					taskState->photonGICausticChainCached));

	// Check if it is a light source (note: I can hit only triangle area light sources)
	if (BSDF_IsLightSource(bsdf) && checkDirectLightHit) {
		DirectHitFiniteLight(
				&taskConfig->film,
				taskConfig,
				emitLightsDistribution,
				pathInfo,
				&taskState->throughput,
				&rays[gid],
				rayHits[gid].t,
				bsdf,
				sampleResult
				LPE_PARAM
				LIGHTS_PARAM);
	}

	if (bsdf->cyclesBSSRDFPhase == 0u) {
		const int result = CyclesBSSRDF_Start(&tasks[gid], taskState, &rays[gid] MATERIALS_PARAM);
		if (result != 0) {
			if (result < 0) taskState->state = MK_SPLAT_SAMPLE;
			return;
		}
	}

	// Closure selection (or the nonlocal exit) determines the event budget.
	sampleResult->lastPathVertex = PathDepthInfo_IsLastPathVertexVol(&pathInfo->depth,
			&taskConfig->pathTracer.maxPathDepth, BSDF_GetEventTypes(bsdf MATERIALS_PARAM), bsdf->isVolume);

	//----------------------------------------------------------------------
	// Check if I can use the photon cache
	//----------------------------------------------------------------------

	if (taskConfig->pathTracer.pgic.indirectEnabled || taskConfig->pathTracer.pgic.causticEnabled) {
		const bool isPhotonGIEnabled = PhotonGICache_IsPhotonGIEnabled(bsdf,
				taskConfig->pathTracer.pgic.glossinessUsageThreshold,
				taskConfig->pathTracer.pgic.causticVolumeBeams
				MATERIALS_PARAM);

		switch (taskConfig->pathTracer.pgic.debugType) {
			case PGIC_DEBUG_SHOWINDIRECT: {
				if (isPhotonGIEnabled) {
					__global const Spectrum* restrict radiance = PhotonGICache_GetIndirectRadiance(bsdf,
							pgicRadiancePhotons, pgicLightGroupCounts, pgicRadiancePhotonsValues, pgicRadiancePhotonsBVHNodes,
							taskConfig->pathTracer.pgic.indirectLookUpRadius * taskConfig->pathTracer.pgic.indirectLookUpRadius,
							taskConfig->pathTracer.pgic.indirectLookUpNormalCosAngle);
					if (radiance) {
						for (uint i = 0; i < pgicLightGroupCounts; ++i)
							VADD3F(sampleResult->radiancePerPixelNormalized[i].c, VLOAD3F(radiance[i].c));
					}
				}
				taskState->state = MK_SPLAT_SAMPLE;
				return;
			}
			case PGIC_DEBUG_SHOWCAUSTIC: {
				if (isPhotonGIEnabled) {
					PhotonGICache_ConnectWithCausticPaths(bsdf,
							pgicCausticPhotons, pgicCausticPhotonsBVHNodes,
							pgicCausticBeams, pgicCausticBeamsBVHNodes,
							taskConfig->pathTracer.pgic.causticVolumeBeams,
							taskConfig->pathTracer.pgic.causticPhotonTracedCount,
							taskConfig->pathTracer.pgic.causticLookUpRadius,
							taskConfig->pathTracer.pgic.causticLookUpNormalCosAngle,
							WHITE,
							&sampleResult->radiancePerPixelNormalized[0],
							&rays[gid], &tasks[gid].tmpHitPoint
							MATERIALS_PARAM);
				}
				taskState->state = MK_SPLAT_SAMPLE;
				return;
			}
			case PGIC_DEBUG_SHOWINDIRECTPATHMIX: {
				if (isPhotonGIEnabled) {
					Seed seedPassThroughEvent = taskState->seedPassThroughEvent;
					const float passThroughEvent = Rnd_FloatValue(&seedPassThroughEvent);

					if (taskState->photonGICacheEnabledOnLastHit &&
							(rayHits[gid].t > PhotonGICache_GetIndirectUsageThreshold(
								pathInfo->lastBSDFEvent,
								pathInfo->lastGlossiness,
								// I hope to not introduce strange sample correlations
								// by using passThrough here
								passThroughEvent,
								taskConfig->pathTracer.pgic.glossinessUsageThreshold,
								taskConfig->pathTracer.pgic.indirectUsageThresholdScale,
								taskConfig->pathTracer.pgic.indirectLookUpRadius))) {
						VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 1.f), sampleResult->radiancePerPixelNormalized[0].c);
						taskState->photonGIShowIndirectPathMixUsed = true;

						taskState->state = MK_SPLAT_SAMPLE;
						return;
					}

					taskState->photonGICacheEnabledOnLastHit = true;
				} else
					taskState->photonGICacheEnabledOnLastHit = false;

				break;
			}
			case PGIC_DEBUG_NONE:
			default: {
				if (isPhotonGIEnabled) {
					if (taskConfig->pathTracer.pgic.causticEnabled &&
							(!taskConfig->pathTracer.hybridBackForward.enabled || (pathInfo->depth.depth != 0))) {
						const bool isEmpty = PhotonGICache_ConnectWithCausticPaths(bsdf,
								pgicCausticPhotons, pgicCausticPhotonsBVHNodes,
								pgicCausticBeams, pgicCausticBeamsBVHNodes,
								taskConfig->pathTracer.pgic.causticVolumeBeams,
								taskConfig->pathTracer.pgic.causticPhotonTracedCount,
								taskConfig->pathTracer.pgic.causticLookUpRadius,
								taskConfig->pathTracer.pgic.causticLookUpNormalCosAngle,
								VLOAD3F(taskState->throughput.c),
								&sampleResult->radiancePerPixelNormalized[0],
								&rays[gid], &tasks[gid].tmpHitPoint
								MATERIALS_PARAM);

						if (!isEmpty)
							taskState->photonGICausticCacheUsed = true;
					}

					if (taskConfig->pathTracer.pgic.indirectEnabled) {
						Seed seedPassThroughEvent = taskState->seedPassThroughEvent;
						const float passThroughEvent = Rnd_FloatValue(&seedPassThroughEvent);

						if (taskState->photonGICacheEnabledOnLastHit &&
								(rayHits[gid].t > PhotonGICache_GetIndirectUsageThreshold(
									pathInfo->lastBSDFEvent,
									pathInfo->lastGlossiness,
									// I hope to not introduce strange sample correlations
									// by using passThrough here
									passThroughEvent,
									taskConfig->pathTracer.pgic.glossinessUsageThreshold,
									taskConfig->pathTracer.pgic.indirectUsageThresholdScale,
									taskConfig->pathTracer.pgic.indirectLookUpRadius))) {
							__global const Spectrum* restrict radiance = PhotonGICache_GetIndirectRadiance(bsdf,
								pgicRadiancePhotons, pgicLightGroupCounts, pgicRadiancePhotonsValues, pgicRadiancePhotonsBVHNodes,
								taskConfig->pathTracer.pgic.indirectLookUpRadius * taskConfig->pathTracer.pgic.indirectLookUpRadius,
								taskConfig->pathTracer.pgic.indirectLookUpNormalCosAngle);

							if (radiance) {
								for (uint i = 0; i < pgicLightGroupCounts; ++i)
									VADD3F(sampleResult->radiancePerPixelNormalized[i].c, VLOAD3F(taskState->throughput.c) * VLOAD3F(radiance[i].c));
							}

							// I can terminate the path, all done
							taskState->state = MK_SPLAT_SAMPLE;
							return;
						}
					}

					taskState->photonGICacheEnabledOnLastHit = true;
				} else
					taskState->photonGICacheEnabledOnLastHit = false;

				break;
			}
		}
	}

	// RIS product guiding (M4b, port of the pre-DL candidate loop in
	// PathTracer::RenderEyePath): draw K candidates from the
	// (1-wG)*BSDF + wG*guide mixture and resample one proportional to
	// w_i = t(w_i)/pMix(w_i) with the product target t = f|cos|*Lhat.
	// Runs here - post-BSDF, pre-DL - because MK_DL_SAMPLE_BSDF's MIS
	// weight needs the independent normalization zHatMis. risZhat > 0
	// marks an active winner applied by MK_GENERATE_NEXT_VERTEX_RAY.
	taskState->risZhat = 0.f;
	taskState->risZhatMis = 0.f;
	if ((taskConfig->pathTracer.guidingRisK > 0u) && (guidingEnable != 0u) &&
			!sampleResult->lastPathVertex && !sampleResult->firstPathVertex &&
			!BSDF_IsDelta(bsdf MATERIALS_PARAM) &&
			(pathInfo->depth.depth >=
				max(1u, taskConfig->pathTracer.guidingMinDepth))) {
		__global const float *risLeaf = GuideTree_LeafAt(guideNodes,
				guideLeaves, VLOAD3F(&bsdf->hitPoint.p.x));
		const BSDFEvent risEventTypes = BSDF_GetEventTypes(bsdf MATERIALS_PARAM);
		// Mirrors CPU GuidableBsdf(ris = true): the BSDF-side candidates
		// resolve any lobe themselves, so the glossiness cutoff relaxes
		// to a thin band above delta; the field only has to cover the
		// directions the BSDF would not try. Volumes stay guidable, and
		// the diffuse opt-in mirrors path.guiding.diffuse.
		const bool risGuidable = bsdf->isVolume ||
				(((risEventTypes & GLOSSY) != 0u) ?
				(BSDF_GetGlossiness(bsdf MATERIALS_PARAM) >= .05f) :
				(taskConfig->pathTracer.guidingDiffuse != 0u));
		if (risGuidable && risLeaf && ((uint)risLeaf[22] > 0u) &&
				(risLeaf[20] >= GUIDE_WARMUP_RECORDS)) {
			const uint risK = min(taskConfig->pathTracer.guidingRisK, 8u);
			const float wG = taskConfig->pathTracer.guidingStrength *
					Guide_MixWeight(risLeaf[20], risLeaf[21]);
			const float3 risShadeN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
			const bool isVol = bsdf->isVolume;
			const uint sampleOffset = taskConfig->pathTracer.eyeSampleBootSize +
					pathInfo->depth.depth * taskConfig->pathTracer.eyeSampleStepSize;
			const uint risSalt = (sampleOffset * 2971215073u) ^
					(sampleResult->pixelX * 73856093u) ^
					(sampleResult->pixelY * 19349663u) ^
					(GuidingPass(taskConfig, gid, samplesBuff) * 83492791u);
			float3 cDir[8], cEval[8];
			BSDFEvent cEvent[8];
			float cUD0[8], cUD1[8], cT[8], cPMix[8], cW[8];
			bool cBsdf[8];
			float wSum = 0.f;
			for (uint ci = 0u; ci < risK; ++ci)
				wSum += (cW[ci] = Guide_RisCandidate(risLeaf, bsdf,
						risShadeN, isVol, wG,
						risSalt ^ (ci * 0x9e3779b9u), true,
						&cDir[ci], &cEval[ci], &cEvent[ci],
						&cUD0[ci], &cUD1[ci], &cBsdf[ci],
						&cT[ci], &cPMix[ci]
						MATERIALS_PARAM));
			if (wSum > 0.f) {
				// Resample proportional to the weights.
				const float uPick = GuidingHash(risSalt ^ 0x165667b1u) *
						(1.f / 4294967296.f) * wSum;
				float acc = 0.f;
				int sel = -1;
				for (uint ci = 0u; ci < risK; ++ci) {
					acc += cW[ci];
					if ((uPick <= acc) && (cW[ci] > 0.f)) {
						sel = (int)ci;
						break;
					}
				}
				if (sel >= 0) {
					// The MIS density pHat = t/zHatMis needs an
					// INDEPENDENT normalization: conditioning on the
					// winner tilts the selection pool's W low for the
					// directions that reach a light (w/W selection), so
					// a second pool decorrelates it (CPU comment in
					// pathtracer.cpp - pg-indirect-slit halo bias).
					float wSumM = 0.f;
					float3 d2, e2;
					BSDFEvent ev2;
					float u20, u21, t2, pm2;
					bool sb2;
					for (uint ci = 0u; ci < risK; ++ci)
						wSumM += Guide_RisCandidate(risLeaf, bsdf,
								risShadeN, isVol, wG,
								risSalt ^ (0x51ab3d29u + ci * 0x85ebca6bu),
								false,
								&d2, &e2, &ev2, &u20, &u21, &sb2, &t2, &pm2
								MATERIALS_PARAM);
					const float zHat = wSum / risK;
					const float zHatMis = (wSumM > 0.f) ? (wSumM / risK) : zHat;
					const float3 wt = cEval[sel] * (zHat / cT[sel]);
					taskState->risZhat = zHat;
					taskState->risZhatMis = zHatMis;
					taskState->risDirX = cDir[sel].x;
					taskState->risDirY = cDir[sel].y;
					taskState->risDirZ = cDir[sel].z;
					taskState->risWtR = wt.x;
					taskState->risWtG = wt.y;
					taskState->risWtB = wt.z;
					taskState->risPHat = cT[sel] / zHatMis;
					taskState->risUD0 = cUD0[sel];
					taskState->risUD1 = cUD1[sel];
					taskState->risEvent = (uint)cEvent[sel];
					taskState->risSideBsdf = cBsdf[sel] ? 1u : 0u;
				}
			}
		}
	}

	// Portal-guided bounce sampling (M5, port of the bounce-side gate in
	// PathTracer::RenderEyePath): with probability portalShare the bounce
	// direction is proposed by aiming at a uniform point on an aperture
	// rect. The decision runs here - post-RIS, pre-DL - because
	// MK_DL_SAMPLE_BSDF folds wP*PortalPdfW into the bounce density: the
	// CPU mirrors the same predicate in both places; on the GPU the
	// mirrored result travels in the task state. Mutually exclusive with
	// RIS (pHat replaces the rest-mixture) and with the ReSTIR-GI-eligible
	// first vertex (giPdfW owns the books there).
	taskState->portalW = 0.f;
	taskState->portalTake = 0u;

	// Vertex connection (M6): a fresh eye vertex starts a fresh connect
	// pass over the paired light task's vertex cache
	taskState->vcCursor = 0u;
	taskState->vcPending = 0u;
	{
		const uint portalCount = taskConfig->pathTracer.portalCount;
		if ((portalCount > 0u) && !BSDF_IsDelta(bsdf MATERIALS_PARAM) &&
				!bsdf->isVolume && (taskState->risZhat <= 0.f) &&
				!(taskConfig->pathTracer.restirGI.enabled &&
					sampleResult->firstPathVertex)) {
			const float3 portalP = VLOAD3F(&bsdf->hitPoint.p.x);
			const float3 portalN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
			if (Portal_UsableAt(portalRects, portalCount, portalP) &&
					Portal_SideOK(portalRects, portalCount,
						taskConfig->pathTracer.portalSideGate, portalP) &&
					Portal_FacingOK(portalRects, portalCount, portalP,
						portalN)) {
				const float wPortal = Portal_ShareAt(portalRects,
						portalCount, taskConfig->pathTracer.portalShare,
						taskConfig->pathTracer.portalAdapt != 0u,
						guideNodes, guideLeaves, guidingEnable, portalP);
				if (wPortal > 0.f) {
					const uint sampleOffset =
							taskConfig->pathTracer.eyeSampleBootSize +
							pathInfo->depth.depth *
							taskConfig->pathTracer.eyeSampleStepSize;
					const uint portalSalt =
							(sampleResult->pixelX * 2654435761u) ^
							(sampleResult->pixelY * 2246822519u) ^
							(GuidingPass(taskConfig, gid, samplesBuff) *
								3266489917u) ^
							(sampleOffset * 668265263u);
					taskState->portalW = wPortal;
					taskState->portalTake =
							(GuidingHash(portalSalt) * (1.f / 4294967296.f) <
								wPortal) ? 1u : 0u;
				}
			}
		}
	}

	//----------------------------------------------------------------------
	// Check if this is the last path vertex (but not also the first)
	//
	// I handle as a special case when the path vertex is both the first
	// and the last: I do direct light sampling without MIS.
	// Vertex connection (M6): the last vertex still connects to the light
	// sub-path vertices before splatting (CPU BIDIR parity)
	taskState->state = (sampleResult->lastPathVertex && !sampleResult->firstPathVertex) ?
		(taskConfig->pathTracer.vertexConnect.enabled ?
			MK_VC_CONNECT : MK_SPLAT_SAMPLE) : MK_DL_ILLUMINATE;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_RT_DL
// To: MK_SPLAT_SAMPLE or MK_GENERATE_NEXT_VERTEX_RAY
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_RT_DL(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_RT_DL(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_RT_DL)
		return;

 	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global GPUTaskDirectLight *taskDirectLight = &tasksDirectLight[gid];
	__global GPUTaskMnee *taskMnee = &tasksMnee[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES
	
	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	float3 connectionThroughput = WHITE;

	Seed seedPassThroughEvent = taskDirectLight->seedPassThroughEvent;
	const float passThroughEvent = Rnd_FloatValue(&seedPassThroughEvent);
	taskDirectLight->seedPassThroughEvent = seedPassThroughEvent;

	int throughShadowTransparency = taskDirectLight->throughShadowTransparency;
	const bool continueToTrace =
		Scene_Intersect(taskConfig,
			EYE_RAY | SHADOW_RAY,
			// Shadow rays have no generating bounce event. The task's
			// tmpPathDepthInfo is the depthInfo copy made by
			// DirectLight_BSDFSampling() (eye path depth + 1 bounce):
			// using it (instead of &eyePathInfos[gid].depth) also keeps
			// the eye path transparentDepth accumulation untouched by
			// shadow ray pass-throughs.
			&task->tmpPathDepthInfo, NONE,
			&throughShadowTransparency,
			&directLightVolInfos[gid],
			&task->tmpHitPoint,
			passThroughEvent,
			&rays[gid], &rayHits[gid], &task->tmpBsdf,
			&connectionThroughput, WHITE,
			sampleResult,
			true
			MATERIALS_PARAM
			);
	taskDirectLight->throughShadowTransparency = throughShadowTransparency;
	VSTORE3F(connectionThroughput * VLOAD3F(taskDirectLight->illumInfo.lightRadiance.c), taskDirectLight->illumInfo.lightRadiance.c);
	VSTORE3F(connectionThroughput * VLOAD3F(taskDirectLight->illumInfo.lightIrradiance.c), taskDirectLight->illumInfo.lightIrradiance.c);

	// Vertex connection (M6): CPU DirectLightSampling lifts the MIS
	// weight to 1 when the shadow ray crossed a shadow-transparent
	// occluder (throughShadowTransparency of the last shadow hit).
	// The flag accumulates over the multi-segment shadow walk, but the
	// undo must be applied ONCE on the terminating segment - doing it
	// per iteration compounds 1/vcMisWeight for every transparent hit.
	if (!continueToTrace &&
			taskConfig->pathTracer.vertexConnect.enabled &&
			throughShadowTransparency &&
			(taskDirectLight->illumInfo.vcMisWeight > 0.f) &&
			(taskDirectLight->illumInfo.vcMisWeight < 1.f))
		VSTORE3F(VLOAD3F(taskDirectLight->illumInfo.lightRadiance.c) /
				taskDirectLight->illumInfo.vcMisWeight,
				taskDirectLight->illumInfo.lightRadiance.c);

	const bool rayMiss = (rayHits[gid].meshIndex == NULL_INDEX);

	// If continueToTrace, there is nothing to do, just keep the same state
	if (!continueToTrace) {
		if (rayMiss) {
			// Nothing was hit, the light source is visible

			__global BSDF *bsdf = &taskState->bsdf;

			if (!BSDF_IsShadowCatcher(bsdf MATERIALS_PARAM)) {
				const float3 lightRadiance = VLOAD3F(taskDirectLight->illumInfo.lightRadiance.c);
				SampleResult_AddDirectLight(&taskConfig->film,
						sampleResult, taskDirectLight->illumInfo.lightID,
						BSDF_GetEventTypes(bsdf
							MATERIALS_PARAM),
						VLOAD3F(taskState->throughput.c), lightRadiance,
						1.f);
				// LPE terminal: next-event estimation at this vertex
				// (env lights classify as E, all other emitters as L -
				// CPU PathTracer::DirectLightSampling parity). The
				// direction bit comes from the shadow ray's hemisphere
				// vs the incoming direction (bsdf->hitPoint.fixedDir).
				const float3 lpeGN = VLOAD3F(&bsdf->hitPoint.geometryN.x);
				const bool lpeTransmit = (dot(lpeGN, VLOAD3F(&bsdf->hitPoint.fixedDir.x)) *
						dot(lpeGN, VLOAD3F(&rays[gid].d.x))) < 0.f;
				LPE_AccumulateVertex(sampleResult, &eyePathInfos[gid],
						LPE_VertexEvent(
							(BSDF_GetEventTypes(bsdf MATERIALS_PARAM) & (DIFFUSE | GLOSSY | SPECULAR)) |
								(lpeTransmit ? TRANSMIT : REFLECT),
							bsdf->isVolume),
						Light_IsEnvironmental(&lights[taskDirectLight->illumInfo.lightIndex]) ?
							LPE_SYM_E : LPE_SYM_L,
						VLOAD3F(taskState->throughput.c) * lightRadiance
						LPE_PARAM);

				// The first path vertex is not handled by AddDirectLight(). This is valid
				// for irradiance AOV only if it is not a SPECULAR material.
				//
				// Note: irradiance samples the light sources only here (i.e. no
				// direct hit, no MIS, it would be useless)
				if ((sampleResult->firstPathVertex) && !(BSDF_GetEventTypes(bsdf
							MATERIALS_PARAM) & SPECULAR)) {
					const float3 irradiance = (M_1_PI_F * fabs(dot(
								VLOAD3F(&bsdf->hitPoint.shadeN.x),
								VLOAD3F(&rays[gid].d.x)))) *
							VLOAD3F(taskDirectLight->illumInfo.lightIrradiance.c);
					VSTORE3F(irradiance, sampleResult->irradiance.c);
				}
			}

			taskDirectLight->directLightResult = ILLUMINATED;
		} else
			taskDirectLight->directLightResult = SHADOWED;

		// MNEE: if the blocker is a delta specular surface and the light is
		// a positional delta emitter, try to solve the specular chain
		// x0 -> x1 -> y (the kernel port of PathTracer::MNEEDirectSampling
		// in pathtracer_mnee.cpp), else the multi-specular chain
		// (MNEEMultiDirectSampling port) when maxspecular > 1. The plain
		// estimator is 0 on these paths and forward BSDF sampling can not
		// hit a positional delta light, so the estimators are disjoint and
		// no MIS is required.
		// hybridBackForward: every MNEE path is caustic-class (light ->
		// specular chain -> diffuse -> eye) and is owned by the light pass;
		// the isNearlyCaustic gate only sees speculars in the eye prefix,
		// so MNEE is suppressed here explicitly (measured: the two
		// estimators summed exactly on tinycaster, 2x bias).
		// Mnee_Start returns 1 (single vertex started), 2 (single vertex
		// inapplicable but the chain may apply: mirror opposite-side seed),
		// 0 (neither).
		const int mneeStartResult =
				((taskDirectLight->directLightResult == SHADOWED) &&
				!taskConfig->pathTracer.hybridBackForward.enabled ?
				Mnee_Start(taskConfig, task, taskDirectLight, taskMnee, taskState,
					&rayHits[gid], &rays[gid], mneeSeeds, worldRadius
					LIGHTS_PARAM) : 0);
		if ((mneeStartResult == 1) ||
				((mneeStartResult == 2) &&
				 MneeChain_StartFromShadow(taskConfig, task, taskDirectLight, taskMnee, taskState,
					&rays[gid],
					&directLightVolInfos[gid], &eyePathInfos[gid]
					LIGHTS_PARAM))) {
			taskState->state = MK_MNEE_NEXT_VERTEX;
		} else {
			// Check if this is the last path vertex
			// Vertex connection (M6): the connect stage sits between the
			// DL resolve and the bounce (CPU BIDIR parity)
			if (taskConfig->pathTracer.vertexConnect.enabled)
				pathState = MK_VC_CONNECT;
			else if (sampleResult->lastPathVertex)
				pathState = MK_SPLAT_SAMPLE;
			else
				pathState = MK_GENERATE_NEXT_VERTEX_RAY;

			// Save the state
			taskState->state = pathState;
		}
	}
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_DL_ILLUMINATE (ReSTIR visibility phase 1 queued candidate
//       shadow rays into the rays[] tail; the trace pass filled
//       rayHits[] for them)
// To:   MK_DL_SAMPLE_BSDF (winner emitted its real shadow ray) or
//       MK_GENERATE_NEXT_VERTEX_RAY / MK_SPLAT_SAMPLE (no valid light)
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_RT_RESTIR(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_RT_RESTIR(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_RT_RESTIR)
		return;

 	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__global BSDF *bsdf = &taskState->bsdf;

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__global GPUTaskDirectLight *taskDirectLight = &tasksDirectLight[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	const uint sampleOffset = taskConfig->pathTracer.eyeSampleBootSize + pathInfo->depth.depth * taskConfig->pathTracer.eyeSampleStepSize;

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	const uint restirVisCandCount = taskConfig->pathTracer.restir.visCandCount;
	// Per-task tail stride: K fresh + RESTIR_PIXEL_MERGES_MAX merge
	// candidates (E2d)
	const uint tailStride = restirVisCandCount + RESTIR_PIXEL_MERGES_MAX;
	__global Ray *candRays =
			&rays[taskConfig->pathTracer.restir.visCandRayBase +
					gid * tailStride];
	__global const RayHit *candHits =
			&rayHits[taskConfig->pathTracer.restir.visCandRayBase +
					gid * tailStride];
	// The candidate records are appended after the per-pixel
	// reservoirs (2 RestirReservoir slots each)
	__global const RestirVisCandidate *candData =
			(__global const RestirVisCandidate *)(restirReservoirs +
			taskConfig->pathTracer.restir.visCandDataOffset) +
			gid * tailStride;

	const bool resolved = DirectLight_RestirResolveVisibility(
			bsdf,
			&rays[gid],
			candRays, candHits, candData, restirVisCandCount,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius,
			&task->tmpHitPoint,
			rays[gid].time,
			Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_X SAMPLER_PARAM),
			Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_Y SAMPLER_PARAM),
			Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_Z SAMPLER_PARAM),
			Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_W SAMPLER_PARAM),
			// The stored slot is a PRIMARY-HIT reservoir: its target
			// was evaluated at the previous pass's depth-0 point of
			// this pixel. Merging it at a deeper vertex without
			// re-evaluating the stored winner's target is not a valid
			// GRIS merge (the pi_new/pi_old ratio degenerates to 1 and
			// the output weight is multiplied by an arbitrary
			// p(x_cur)/p(x_src) factor - a real bias). The merges and
			// the store are therefore all depth-0 only, matching the
			// classic ReSTIR-DI G-buffer-reservoir design.
			taskConfig->pathTracer.restir.temporalEnable &&
					(pathInfo->depth.depth == 0),
			(taskConfig->pathTracer.restir.temporalEnable ||
					taskConfig->pathTracer.restir.spatialEnable) &&
					(pathInfo->depth.depth == 0),
			sampleResult->pixelY * filmWidth + sampleResult->pixelX,
			restirReservoirs,
			&taskDirectLight->illumInfo
			LIGHTS_PARAM);

	// Re-mask the candidate tail slots. Without this, slots written
	// by a previous MK_DL_ILLUMINATE stay valid-and-unmasked forever
	// and every subsequent trace pass would re-trace them: at
	// taskCount * K scale that saturates the GPU for the whole render
	// (measured: display starvation -> userspace watchdog panic on
	// macOS). Masked rays are skipped by both the MetalRT intersector
	// and the software kernels; a task re-entering MK_DL_ILLUMINATE
	// rewrites its slots anyway.
	for (uint i = 0; i < tailStride; ++i)
		candRays[i].flags = RAY_FLAGS_MASKED;

	if (resolved) {
		// The winner's shadow ray is queued in rays[gid]: evaluate the
		// BSDF and trace it through the normal direct-light path
		taskState->state = MK_DL_SAMPLE_BSDF;
	} else {
		// No visible candidate: move to the next vertex ray, or splat
		// if this was the last path vertex
		// Vertex connection (M6): the connect stage precedes the bounce
		taskState->state = taskConfig->pathTracer.vertexConnect.enabled ?
				MK_VC_CONNECT :
				((sampleResult->lastPathVertex) ? MK_SPLAT_SAMPLE : MK_GENERATE_NEXT_VERTEX_RAY);
	}

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_RT_GI_BOUNCE (queued by MK_GENERATE_NEXT_VERTEX_RAY)
// To: MK_RT_GI_RESOLVE
//
// Consumes the GI candidate bounce-ray hits (traced this iteration),
// builds each hit candidate's x2 BSDF and queues its one-sample NEE
// shadow ray into the second half of the task's GI tail.
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_RT_GI_BOUNCE(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_RT_GI_BOUNCE(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_RT_GI_BOUNCE)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__global BSDF *bsdf = &taskState->bsdf;

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	const uint giK = taskConfig->pathTracer.restirGI.giCandCount;
	const uint giRayBase = taskConfig->pathTracer.restirGI.giCandRayBase +
			gid * (2u * giK + 1u);
	__global Ray *candRays = &rays[giRayBase];
	__global RayHit *candHits = &rayHits[giRayBase];
	__global RestirGICandidate *candData = (__global RestirGICandidate *)
			(restirReservoirs + taskConfig->pathTracer.restirGI.giCandDataOffset +
			gid * taskConfig->pathTracer.restirGI.giCandStride);
	__global RestirGIResult *giResult =
			(__global RestirGIResult *)(candData + giK);

	const uint giPass = GuidingPass(taskConfig, gid, samplesBuff);
	const uint baseSeed = (sampleResult->pixelX * 73856093u) ^
			(sampleResult->pixelY * 19349663u) ^
			(giPass * 83492791u) ^
			seedValue.s1;

	RestirGI_Bounce(
			bsdf,
			&task->tmpBsdf,
			&directLightVolInfos[gid],
			pathInfo,
			&task->tmpHitPoint,
			&task->tmpPathDepthInfo,
			candRays, candHits, candData,
			giResult,
			(__global RestirGIReservoir *)(restirReservoirs +
					taskConfig->pathTracer.restirGI.giReservoirOffset),
			sampleResult->pixelY * filmWidth + sampleResult->pixelX,
			giPass,
			taskConfig->pathTracer.restirGI.temporalEnable,
			giK, baseSeed, rays[gid].time,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius
			LIGHTS_PARAM);

	// The NEE rays land in rayHits on the next trace pass; the resolve
	// must skip this task until then (dense dispatch runs the resolve
	// kernel later in THIS same pass).
	giResult->needsTrace = 1u;
	taskState->state = MK_RT_GI_RESOLVE;

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_RT_GI_RESOLVE
// To: MK_GENERATE_NEXT_VERTEX_RAY
//
// Folds the NEE shadow-ray visibility into the candidate proxies, runs
// the RIS + temporal/spatial merges, stores the pre-spatial reservoir
// and hands the winner to MK_GENERATE_NEXT_VERTEX_RAY through the
// task's RestirGIResult record.
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_RT_GI_RESOLVE(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_RT_GI_RESOLVE(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_RT_GI_RESOLVE)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global BSDF *bsdf = &taskState->bsdf;

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	const uint giK = taskConfig->pathTracer.restirGI.giCandCount;
	const uint giRayBase = taskConfig->pathTracer.restirGI.giCandRayBase +
			gid * (2u * giK + 1u);
	__global Ray *candRays = &rays[giRayBase];
	__global RayHit *candHits = &rayHits[giRayBase];
	__global RestirGICandidate *candData = (__global RestirGICandidate *)
			(restirReservoirs + taskConfig->pathTracer.restirGI.giCandDataOffset +
			gid * taskConfig->pathTracer.restirGI.giCandStride);
	__global RestirGIResult *giResult =
			(__global RestirGIResult *)(candData + giK);

	// Dense dispatch runs this kernel in the same pass that queued the
	// NEE rays: skip until they have actually been traced. (Wavefront
	// queues are rebuilt from taskState->state once per iteration, so a
	// task only reaches this queue after a trace pass - the flag is
	// redundant there, and clearing it keeps the state consistent if
	// the dispatch mode ever changed.)
	if (giResult->needsTrace) {
		giResult->needsTrace = 0u;
		if (!wavefrontEnable)
			return;
	}

	const uint pixelIndex =
			sampleResult->pixelY * filmWidth + sampleResult->pixelX;
	const uint giPass = GuidingPass(taskConfig, gid, samplesBuff);
	const uint baseSeed = (sampleResult->pixelX * 73856093u) ^
			(sampleResult->pixelY * 19349663u) ^
			(giPass * 83492791u) ^ seedValue.s1;

	RestirGI_Resolve(
			bsdf,
			candRays, candHits, candData,
			giResult,
			(__global RestirGIReservoir *)(restirReservoirs +
					taskConfig->pathTracer.restirGI.giReservoirOffset),
			pixelIndex, giPass,
			giK, baseSeed,
			taskConfig->pathTracer.restirGI.temporalEnable,
			taskConfig->pathTracer.restirGI.spatialEnable,
			filmWidth,
			taskConfig->pathTracer.restir.reservoirCount,
			worldRadius
			MATERIALS_PARAM);

	// Re-mask the NEE tail slots (the bounce half was masked in
	// MK_RT_GI_BOUNCE): without this, slots written by a previous GI
	// resolve stay valid-and-unmasked forever and every subsequent
	// trace pass would re-trace them (the same GPU-saturation hazard
	// documented at the DI tail). The merge-visibility slot 2K is
	// likewise consumed by this point.
	for (uint i = 0; i < giK; ++i)
		candRays[giK + i].flags = RAY_FLAGS_MASKED;
	candRays[2u * giK].flags = RAY_FLAGS_MASKED;

	// ReSTIR PG feed: an accepted winner is a target-proportional
	// direction sample - train the guide field on it. pending == 2 is
	// the no-winner fallback (nothing accepted - no record).
	if ((guidingEnable != 0u) && (giResult->pending == 1u)) {
		const float flux = Spectrum_Y(MAKE_FLOAT3(giResult->lHatR,
				giResult->lHatG, giResult->lHatB));
		if (flux > 0.f)
			Guide_EmitRec(gid, VLOAD3F(&bsdf->hitPoint.p.x),
					VLOAD3F(&giResult->dirX), flux,
					guideRec0, guideRec1, guideRec2, guideRec3,
					guideRec4, guideRec5, guideRec6, guideRec7,
					guideRec8, guideRec9, guideRec10, guideRec11,
					guideRec12, guideRec13, guideRec14, guideRec15);
	}

	// Hand the resolved winner (or the normal-sample fallback, pending
	// == 2) back to the shared continuation path.
	taskState->state = MK_GENERATE_NEXT_VERTEX_RAY;

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_PT_BOUNCE (queued by MK_GENERATE_NEXT_VERTEX_RAY)
// To: MK_PT_RESOLVE
//
// ReSTIR PT (PT-2 GPU): consumes the candidate bounce-ray hits
// (traced this iteration), builds each hit candidate's x2 record and
// queues the NEE + merge-visibility shadow rays into the second half
// of the task's PT tail (2K+3 slots).
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_PT_BOUNCE(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_PT_BOUNCE(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_PT_BOUNCE)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__global BSDF *bsdf = &taskState->bsdf;

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	const uint ptK = taskConfig->pathTracer.restirPT.ptCandCount;
	const uint ptRayBase = taskConfig->pathTracer.restirPT.ptCandRayBase +
			gid * (2u * ptK + 3u);
	__global Ray *candRays = &rays[ptRayBase];
	__global RayHit *candHits = &rayHits[ptRayBase];
	__global RestirGICandidate *candData = (__global RestirGICandidate *)
			(restirReservoirs + taskConfig->pathTracer.restirPT.ptCandDataOffset +
			gid * taskConfig->pathTracer.restirPT.ptCandStride);
	__global RestirPTResult *ptResult =
			(__global RestirPTResult *)(candData + ptK);

	const uint ptPass = GuidingPass(taskConfig, gid, samplesBuff);
	const uint baseSeed = (sampleResult->pixelX * 73856093u) ^
			(sampleResult->pixelY * 19349663u) ^
			(ptPass * 83492791u) ^
			seedValue.s1;

	RestirPT_Bounce(
			bsdf,
			&task->tmpBsdf,
			&directLightVolInfos[gid],
			pathInfo,
			&task->tmpHitPoint,
			&task->tmpPathDepthInfo,
			candRays, candHits, candData,
			ptResult,
			(__global RestirPTReservoir *)(restirReservoirs +
					taskConfig->pathTracer.restirPT.ptReservoirOffset),
			sampleResult->pixelY * filmWidth + sampleResult->pixelX,
			ptPass,
			taskConfig->pathTracer.restirPT.temporalEnable,
			taskConfig->pathTracer.restirPT.spatialEnable,
			filmWidth,
			taskConfig->pathTracer.restirPT.reservoirCount,
			ptK, baseSeed, rays[gid].time,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius
			LIGHTS_PARAM);

	// The NEE/merge-visibility rays land in rayHits on the next trace
	// pass; the resolve must skip this task until then (dense dispatch
	// runs the resolve kernel later in THIS same pass).
	ptResult->needsTrace = 1u;
	taskState->state = MK_PT_RESOLVE;

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_PT_RESOLVE
// To: MK_GENERATE_NEXT_VERTEX_RAY (fresh winner or fallback) or
//     MK_SPLAT_SAMPLE (stored-suffix win consumed the path)
//
// Folds the NEE/merge visibility into the candidate targets, runs the
// RIS + temporal/spatial merges over the measured-suffix reservoirs,
// then either hands the fresh winner to MK_GENERATE (pending = 1) or
// pays thr * fcos * W * L_suf directly and ends the path.
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_PT_RESOLVE(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_PT_RESOLVE(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_PT_RESOLVE)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global BSDF *bsdf = &taskState->bsdf;

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	const uint ptK = taskConfig->pathTracer.restirPT.ptCandCount;
	const uint ptRayBase = taskConfig->pathTracer.restirPT.ptCandRayBase +
			gid * (2u * ptK + 3u);
	__global Ray *candRays = &rays[ptRayBase];
	__global RayHit *candHits = &rayHits[ptRayBase];
	__global RestirGICandidate *candData = (__global RestirGICandidate *)
			(restirReservoirs + taskConfig->pathTracer.restirPT.ptCandDataOffset +
			gid * taskConfig->pathTracer.restirPT.ptCandStride);
	__global RestirPTResult *ptResult =
			(__global RestirPTResult *)(candData + ptK);

	// Dense dispatch runs this kernel in the same pass that queued the
	// NEE/visibility rays: skip until they have actually been traced
	// (GI parity: redundant under wavefront queue rebuilds).
	if (ptResult->needsTrace) {
		ptResult->needsTrace = 0u;
		if (!wavefrontEnable)
			return;
	}

	const uint pixelIndex =
			sampleResult->pixelY * filmWidth + sampleResult->pixelX;
	const uint ptPass = GuidingPass(taskConfig, gid, samplesBuff);
	const uint baseSeed = (sampleResult->pixelX * 73856093u) ^
			(sampleResult->pixelY * 19349663u) ^
			(ptPass * 83492791u) ^ seedValue.s1;

	const bool consumed = RestirPT_Resolve(
			bsdf,
			candRays, candHits, candData,
			ptResult,
			(__global RestirPTReservoir *)(restirReservoirs +
					taskConfig->pathTracer.restirPT.ptReservoirOffset),
			pixelIndex, ptPass,
			ptK, baseSeed,
			taskConfig->pathTracer.restirPT.temporalEnable,
			taskConfig->pathTracer.restirPT.spatialEnable,
			filmWidth,
			taskConfig->pathTracer.restirPT.reservoirCount,
			worldRadius
			MATERIALS_PARAM);

	// Re-mask the whole tail (bounce + NEE + merge-visibility): slots
	// written by an earlier PT resolve stay valid forever otherwise and
	// every subsequent trace pass would re-trace them.
	for (uint i = 0; i < 2u * ptK + 3u; ++i)
		candRays[i].flags = RAY_FLAGS_MASKED;

	if (consumed) {
		// Stored-suffix win: pay thr * (fcos*W) * L_suf directly and
		// end the path (CPU parity: radiance[0] += pathThroughput *
		// pick.eval * pick.connThr * pick.lsuf; connThr folds as 1 -
		// the tail rays carry no volume info, documented v1 limit).
		const float3 contrib = VLOAD3F(taskState->throughput.c) *
				VLOAD3F(&ptResult->bsdfR) *
				MAKE_FLOAT3(ptResult->lsufR, ptResult->lsufG,
						ptResult->lsufB);
		VADD3F(sampleResult->radiancePerPixelNormalized[0].c, contrib);
		// AOV bookkeeping mirrors the CPU hook: bucket the consumed
		// contribution by the winning event type.
		if (ptResult->event & DIFFUSE)
			VADD3F(sampleResult->indirectDiffuseReflect.c, contrib);
		else if (ptResult->event & GLOSSY)
			VADD3F(sampleResult->indirectGlossyReflect.c, contrib);
		else
			VADD3F(sampleResult->indirectSpecularReflect.c, contrib);
		sampleResult->firstPathVertexEvent = (BSDFEvent)ptResult->event;
		ptResult->pending = 0u;
		// ReSTIR PG feed: the consumed winner is an accepted
		// (measured-suffix) direction - train the guide field on it.
		if (guidingEnable != 0u) {
			const float flux = Spectrum_Y(MAKE_FLOAT3(ptResult->lsufR,
					ptResult->lsufG, ptResult->lsufB));
			if (flux > 0.f)
				Guide_EmitRec(gid, VLOAD3F(&bsdf->hitPoint.p.x),
						VLOAD3F(&ptResult->dirX), flux,
						guideRec0, guideRec1, guideRec2, guideRec3,
						guideRec4, guideRec5, guideRec6, guideRec7,
						guideRec8, guideRec9, guideRec10, guideRec11,
						guideRec12, guideRec13, guideRec14, guideRec15);
		}
		taskState->state = MK_SPLAT_SAMPLE;
	} else {
		// Fresh winner (pending = 1, consumed by MK_GENERATE) or
		// fallback to a normal BSDF sample (pending = 2).
		taskState->state = MK_GENERATE_NEXT_VERTEX_RAY;
	}

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_DL_ILLUMINATE
// To: MK_DL_SAMPLE_BSDF or MK_GENERATE_NEXT_VERTEX_RAY
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_DL_ILLUMINATE(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_DL_ILLUMINATE(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_DL_ILLUMINATE)
		return;

 	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global EyePathInfo *pathInfo = &eyePathInfos[gid];

	__global BSDF *bsdf = &taskState->bsdf;

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__global GPUTaskDirectLight *taskDirectLight = &tasksDirectLight[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	const uint sampleOffset = taskConfig->pathTracer.eyeSampleBootSize + pathInfo->depth.depth * taskConfig->pathTracer.eyeSampleStepSize;

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES
	
	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	// It will set eventually to true if the light is visible
	taskDirectLight->directLightResult = NOT_VISIBLE;

	//----------------------------------------------------------------------
	// ReSTIR visibility-weighted target (E2a): phase 1 enqueues the
	// candidate shadow rays into the shared rays[] tail and defers the
	// reservoir merge to the MK_RT_RESTIR state (next iteration, after
	// the trace pass).
	//----------------------------------------------------------------------
	const uint restirVisCandCount = taskConfig->pathTracer.restir.visCandCount;
	if (taskConfig->pathTracer.restir.enabled &&
			taskConfig->pathTracer.restir.visibilityEnable &&
			(restirVisCandCount > 0u) &&
			!BSDF_IsDelta(bsdf MATERIALS_PARAM)) {
		// Per-task tail stride: K fresh + RESTIR_PIXEL_MERGES_MAX merge
		// candidates (E2d)
		const uint tailStride = restirVisCandCount + RESTIR_PIXEL_MERGES_MAX;
		__global Ray *candRays =
				&rays[taskConfig->pathTracer.restir.visCandRayBase +
						gid * tailStride];
		__global RestirVisCandidate *candData =
				(__global RestirVisCandidate *)(restirReservoirs +
				taskConfig->pathTracer.restir.visCandDataOffset) +
				gid * tailStride;

		const bool anyCandidate = DirectLight_RestirEnqueueVisibility(
				bsdf,
				candRays, candData, restirVisCandCount,
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				&task->tmpHitPoint,
				rays[gid].time,
				Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_X SAMPLER_PARAM),
				// Neighbour-pixel reservoirs are primary-hit data:
				// merging them at deeper vertices is off-design (the
				// same-surface gate nearly always rejects there anyway)
				// and wastes a Light_Illuminate per merge slot.
				taskConfig->pathTracer.restir.spatialEnable &&
						(pathInfo->depth.depth == 0),
				sampleResult->pixelY * filmWidth + sampleResult->pixelX,
				filmWidth, taskConfig->pathTracer.restir.reservoirCount,
				restirReservoirs
				LIGHTS_PARAM);

		// Vertex connection (M6): the connect stage precedes the bounce
		taskState->state = anyCandidate ? MK_RT_RESTIR :
				(taskConfig->pathTracer.vertexConnect.enabled ? MK_VC_CONNECT :
				((sampleResult->lastPathVertex) ? MK_SPLAT_SAMPLE : MK_GENERATE_NEXT_VERTEX_RAY));

		// Save the seed
		task->seed = seedValue;
		return;
	}

	if (!BSDF_IsDelta(bsdf
			MATERIALS_PARAM) &&
			DirectLight_Illuminate(
				bsdf,
				&rays[gid],
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				&task->tmpHitPoint,
				rays[gid].time,
				Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_X SAMPLER_PARAM),
				Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_Y SAMPLER_PARAM),
				Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_Z SAMPLER_PARAM),
				Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_W SAMPLER_PARAM),
				taskConfig->pathTracer.restir.enabled,
				taskConfig->pathTracer.restir.candidateCount,
				// Reuse is primary-hit only: the per-pixel reservoir's
				// stored target is measured at the previous pass's
				// depth-0 point, so merging it at a deeper vertex
				// without re-evaluation is not a valid GRIS merge
				// (arbitrary pi_new/pi_old bias). The spatial merge
				// re-evaluates targets but the same-surface gate
				// rejects deeper vertices almost always - gating both
				// merges to depth 0 is cheaper and matches the
				// reservoir's documented semantics.
				taskConfig->pathTracer.restir.temporalEnable &&
						(pathInfo->depth.depth == 0),
				(taskConfig->pathTracer.restir.temporalEnable ||
						taskConfig->pathTracer.restir.spatialEnable) &&
						(pathInfo->depth.depth == 0),
				sampleResult->pixelY * filmWidth + sampleResult->pixelX,
				restirReservoirs,
				taskConfig->pathTracer.restir.spatialEnable &&
						(pathInfo->depth.depth == 0),
				filmWidth,
				taskConfig->pathTracer.restir.reservoirCount,
				&taskDirectLight->illumInfo
				LIGHTS_PARAM)) {
		// I have now to evaluate the BSDF
		taskState->state = MK_DL_SAMPLE_BSDF;
	} else {
		// No shadow ray to trace, move to the next vertex ray
		// however, I have to Check if this is the last path vertex
		// Vertex connection (M6): the connect stage precedes the bounce
		taskState->state = taskConfig->pathTracer.vertexConnect.enabled ?
				MK_VC_CONNECT :
				((sampleResult->lastPathVertex) ? MK_SPLAT_SAMPLE : MK_GENERATE_NEXT_VERTEX_RAY);
	}

	//--------------------------------------------------------------------------

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_DL_SAMPLE_BSDF
// To: MK_GENERATE_NEXT_VERTEX_RAY or MK_RT_DL or MK_SPLAT_SAMPLE
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_DL_SAMPLE_BSDF(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_DL_SAMPLE_BSDF(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_DL_SAMPLE_BSDF)
		return;

 	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global GPUTask *task = &tasks[gid];
	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	const uint sampleOffset = taskConfig->pathTracer.eyeSampleBootSize + pathInfo->depth.depth * taskConfig->pathTracer.eyeSampleStepSize;

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES
	
	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	if (DirectLight_BSDFSampling(
			taskConfig,
			&tasksDirectLight[gid].illumInfo,
			rays[gid].time, sampleResult->lastPathVertex,
			pathInfo,
			&task->tmpPathDepthInfo,
			&taskState->bsdf,
			VLOAD3F(&rays[gid].d.x)
			LIGHTS_PARAM,
			guideNodes, guideLeaves, guidingEnable,
			taskState->risZhatMis,
			portalRects, taskConfig->pathTracer.portalCount,
			taskState->portalW)) {
		__global GPUTask *task = &tasks[gid];
		Seed seedValue = task->seed;
		// This trick is required by SAMPLER_PARAM macro
		Seed *seed = &seedValue;

		// Initialize the pass-through event for the shadow ray
		const float passThroughEvent = Sampler_GetSample(taskConfig, sampleOffset + IDX_DIRECTLIGHT_A SAMPLER_PARAM);
		Seed seedPassThroughEvent;
		Rnd_InitFloat(passThroughEvent, &seedPassThroughEvent);
		tasksDirectLight[gid].seedPassThroughEvent = seedPassThroughEvent;

		// Save the seed
		task->seed = seedValue;

		// Initialize the trough a shadow transparency flag used by Scene_Intersect()
		tasksDirectLight[gid].throughShadowTransparency = false;

		// Make a copy of current PathVolumeInfo for tracing the
		// shadow ray
		directLightVolInfos[gid] = pathInfo->volume;

		// I have to trace the shadow ray
		taskState->state = MK_RT_DL;
	} else {
		// No shadow ray to trace, move to the next vertex ray
		// however, I have to check if this is the last path vertex
		// Vertex connection (M6): the connect stage precedes the bounce
		taskState->state = taskConfig->pathTracer.vertexConnect.enabled ?
				MK_VC_CONNECT :
				((sampleResult->lastPathVertex) ? MK_SPLAT_SAMPLE : MK_GENERATE_NEXT_VERTEX_RAY);
	}
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// MNEE specular chain sub-state machine (see Mnee_ProcessState() in
// pathoclbase_funcs.cl): solves the specular chain x0 -> x1 -> y after the
// direct light shadow ray was blocked by a delta specular surface. One
// trace per render iteration: this kernel either consumes the trace result
// of the current MNEE ray (seed / Newton proposal / x1 -> y shadow) and
// writes the next one, or exits back to the normal path advance.
//
// From: MK_MNEE_NEXT_VERTEX
// To: MK_MNEE_NEXT_VERTEX (Newton iterations) or MK_SPLAT_SAMPLE or
//     MK_GENERATE_NEXT_VERTEX_RAY
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_MNEE_NEXT_VERTEX(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_MNEE_NEXT_VERTEX(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_MNEE_NEXT_VERTEX)
		return;

 	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global GPUTask *task = &tasks[gid];
	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	Mnee_ProcessState(taskConfig,
			task, &tasksDirectLight[gid], &tasksMnee[gid], taskState, pathInfo,
			&rays[gid], &rayHits[gid], &directLightVolInfos[gid],
			sampleResult, (uint)gid,
			worldCenterX, worldCenterY, worldCenterZ, worldRadius,
			mneeSeeds
			LPE_PARAM
			LIGHTS_PARAM);
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_GENERATE_NEXT_VERTEX_RAY
// To: MK_SPLAT_SAMPLE or MK_RT_NEXT_VERTEX
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_GENERATE_NEXT_VERTEX_RAY(
		KERNEL_ARGS
		/* SSP eye-side specular tail recorder (path.ssp.enable): per-task
		 * record consumed by the paired light task's camera connects.
		 * NULL when disabled. */ \
		, __global SspTail *sspTails
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_GENERATE_NEXT_VERTEX_RAY(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_GENERATE_NEXT_VERTEX_RAY)
		return;

 	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__global BSDF *bsdf = &taskState->bsdf;

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__constant const Scene* restrict scene = &taskConfig->scene;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	const uint sampleOffset = taskConfig->pathTracer.eyeSampleBootSize + pathInfo->depth.depth * taskConfig->pathTracer.eyeSampleStepSize;

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	__global Ray *ray = &rays[gid];
	
	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	// Sample the BSDF
	float3 sampledDir;
	float3 bsdfSample;
	float cosSampledDir;
	float bsdfPdfW;
	BSDFEvent bsdfEvent;

	if (BSDF_IsShadowCatcher(bsdf MATERIALS_PARAM) && (tasksDirectLight[gid].directLightResult != SHADOWED)) {
		bsdfSample = BSDF_ShadowCatcherSample(bsdf,
				&sampledDir, &bsdfPdfW, &cosSampledDir, &bsdfEvent
				MATERIALS_PARAM);

		if (sampleResult->firstPathVertex) {
			// In this case I have also to set the value of the alpha channel to 0.0
			sampleResult->alpha = 0.f;
		}
	} else {
		const float3 shadowTransparency = BSDF_GetPassThroughShadowTransparency(bsdf
				MATERIALS_PARAM);
		if (!sampleResult->firstPathVertex && !Spectrum_IsBlack(shadowTransparency) && !pathInfo->isNearlyS) {
			sampledDir = -VLOAD3F(&bsdf->hitPoint.fixedDir.x);
			bsdfSample = shadowTransparency;
			bsdfPdfW = pathInfo->lastBSDFPdfW;
			cosSampledDir = -1.f;
			bsdfEvent = pathInfo->lastBSDFEvent;
		} else {
			// guideLeaf is the flattened leaf record at the vertex
			// position; NULL when guiding is off or the field is empty.
			// Hoisted above the ReSTIR enqueue blocks: reservoir
			// candidates draw from the leaf mixture (ReSTIR PG).
			__global const float *guideLeaf = (guidingEnable != 0u) ?
					GuideTree_LeafAt(guideNodes, guideLeaves,
						VLOAD3F(&bsdf->hitPoint.p.x)) : NULL;
			// PG candidate mix weight: the leaf gates itself (warmup +
			// nComp), so this is 0 on cold leaves - pure BSDF draws.
			const float resWG = (guideLeaf && ((uint)guideLeaf[22] > 0u) &&
					(guideLeaf[20] >= GUIDE_WARMUP_RECORDS)) ?
					taskConfig->pathTracer.guidingStrength *
					Guide_MixWeight(guideLeaf[20], guideLeaf[21]) : 0.f;
			// ReSTIR PT (PT-2 GPU): measured-suffix resampling at the
			// depth-0 non-delta vertex - same hook slot as GI (mutually
			// exclusive: path.restir.pt.enable overrides gi on the host
			// parse). The resolve hands the fresh winner back through
			// the task's RestirPTResult record:
			//   pending == 1: consume (dir, fcos*W, risPdfW, event) in
			//       place of a fresh BSDF draw, then arm the pick record
			//       (pending -> 3; MK_RT_NEXT_VERTEX captures the
			//       landing, MK_SPLAT_SAMPLE commits the measured
			//       suffix);
			//   pending == 2: the resolve found no usable winner - take
			//       a normal BSDF sample;
			//   pending == 0: enqueue the K candidate bounce rays into
			//       the PT tail and re-enter through MK_PT_BOUNCE;
			//   pending >= 3: already armed this bounce - plain sample.
			uint ptPending = 0u;
			__global RestirPTResult *ptResult = NULL;
			if (taskConfig->pathTracer.restirPT.enabled &&
					(taskConfig->pathTracer.restirPT.ptCandCount > 0u)) {
				const uint ptK = taskConfig->pathTracer.restirPT.ptCandCount;
				__global RestirGICandidate *ptCand =
						(__global RestirGICandidate *)(restirReservoirs +
						taskConfig->pathTracer.restirPT.ptCandDataOffset +
						gid * taskConfig->pathTracer.restirPT.ptCandStride);
				ptResult = (__global RestirPTResult *)(ptCand + ptK);
				ptPending = ptResult->pending;
				if ((ptPending == 0u) && sampleResult->firstPathVertex &&
						!BSDF_IsDelta(bsdf MATERIALS_PARAM)) {
					__global Ray *ptRays = &rays[
							taskConfig->pathTracer.restirPT.ptCandRayBase +
							gid * (2u * ptK + 3u)];
					// Same decorrelation as the GI draw below.
					const uint ptSeed = (sampleResult->pixelX * 73856093u) ^
							(sampleResult->pixelY * 19349663u) ^
							(GuidingPass(taskConfig, gid, samplesBuff) *
							83492791u) ^ seedValue.s1;
					if (RestirGI_EnqueueBounce(bsdf, ptRays, ptCand,
							ptK, ptSeed, ray->time, guideLeaf, resWG
							MATERIALS_PARAM)) {
						taskState->state = MK_PT_BOUNCE;
						task->seed = seedValue;
						return;
					}
				}
				if (ptPending == 1u) {
					sampledDir = MAKE_FLOAT3(ptResult->dirX,
							ptResult->dirY, ptResult->dirZ);
					bsdfSample = MAKE_FLOAT3(ptResult->bsdfR,
							ptResult->bsdfG, ptResult->bsdfB);
					bsdfPdfW = ptResult->pdfW;
					bsdfEvent = (BSDFEvent)ptResult->event;
					cosSampledDir = fabs(dot(
							VLOAD3F(&bsdf->hitPoint.shadeN.x), sampledDir));
					// Fresh winner consumed: arm the landing capture
					// (MK_RT_NEXT_VERTEX) + path-end commit
					// (MK_SPLAT_SAMPLE).
					ptResult->pending = 3u;
#if defined(SLG_SPECTRAL)
					sampleResult->spectralHeroAlive =
							bsdf->hitPoint.spectralHeroAlive;
#endif
				} else if (ptPending == 2u)
					// Fallback decision consumed; the armed/landed
					// states (3/4) must survive to the commit.
					ptResult->pending = 0u;
			}
			// ReSTIR GI (G1 GPU): first-bounce reservoir resampling at
			// the depth-0 non-delta vertex. The resolve hands the winner
			// back through the task's RestirGIResult record:
			//   pending == 1: consume the resampled (dir, fcos*W,
			//       risPdfW, event) in place of a fresh BSDF draw;
			//   pending == 2: the resolve found no usable winner - take
			//       a normal BSDF sample (the CPU side's fallback);
			//   pending == 0: enqueue the K candidate bounce rays into
			//       the GI tail and re-enter through MK_RT_GI_BOUNCE.
			uint giPending = 0u;
			if (taskConfig->pathTracer.restirGI.enabled &&
					(taskConfig->pathTracer.restirGI.giCandCount > 0u)) {
				const uint giK = taskConfig->pathTracer.restirGI.giCandCount;
				__global RestirGICandidate *giCand =
						(__global RestirGICandidate *)(restirReservoirs +
						taskConfig->pathTracer.restirGI.giCandDataOffset +
						gid * taskConfig->pathTracer.restirGI.giCandStride);
				__global RestirGIResult *giResult =
						(__global RestirGIResult *)(giCand + giK);
				giPending = giResult->pending;
				giResult->pending = 0u;
				if ((giPending == 0u) && sampleResult->firstPathVertex &&
						!BSDF_IsDelta(bsdf MATERIALS_PARAM)) {
					__global Ray *giRays = &rays[
							taskConfig->pathTracer.restirGI.giCandRayBase +
							gid * (2u * giK + 1u)];
					// seedValue.s1 keeps the draws decorrelated on
					// samplers that never advance sample->pass (RANDOM).
					const uint giSeed = (sampleResult->pixelX * 73856093u) ^
							(sampleResult->pixelY * 19349663u) ^
							(GuidingPass(taskConfig, gid, samplesBuff) *
							83492791u) ^ seedValue.s1;
					if (RestirGI_EnqueueBounce(bsdf, giRays, giCand,
							giK, giSeed, ray->time, guideLeaf, resWG
							MATERIALS_PARAM)) {
						taskState->state = MK_RT_GI_BOUNCE;
						task->seed = seedValue;
						return;
					}
				}
				if (giPending == 1u) {
					sampledDir = MAKE_FLOAT3(giResult->dirX,
							giResult->dirY, giResult->dirZ);
					bsdfSample = MAKE_FLOAT3(giResult->bsdfR,
							giResult->bsdfG, giResult->bsdfB);
					bsdfPdfW = giResult->pdfW;
					bsdfEvent = (BSDFEvent)giResult->event;
					cosSampledDir = fabs(dot(
							VLOAD3F(&bsdf->hitPoint.shadeN.x), sampledDir));
#if defined(SLG_SPECTRAL)
					sampleResult->spectralHeroAlive =
							bsdf->hitPoint.spectralHeroAlive;
#endif
				}
			}
			// Path guiding (P1-3 M4e): flattened SD-tree + vMF leaf
			// one-sample MIS, mirroring PathTracer::RenderEyePath() on
			// the CPU (see src/slg/engines/pathtracer.cpp). Training
			// records below (M2b-2).
			if ((giPending != 1u) && (ptPending != 1u)) {
			const BSDFEvent eventTypes = BSDF_GetEventTypes(bsdf MATERIALS_PARAM);
			// Mirrors CPU GuidableBsdf(): volume scattering vertices are
			// always guidable (phase lobes sample blind w.r.t. the
			// incident field); glossy bounces need enough roughness
			// (path.guiding.glossythreshold); pure diffuse needs the
			// path.guiding.diffuse opt-in.
			const bool guidableBsdf = bsdf->isVolume ||
					(((eventTypes & GLOSSY) != 0u) ?
					(BSDF_GetGlossiness(bsdf MATERIALS_PARAM) >=
						taskConfig->pathTracer.guidingGlossiness) :
					(taskConfig->pathTracer.guidingDiffuse != 0u));
			// Same gate as CPU CanGuide(): fitted leaf past warmup
			// (min depth from path.guiding.mindepth).
			const bool tryGuide = (guidingEnable != 0u) &&
					!BSDF_IsDelta(bsdf MATERIALS_PARAM) &&
					guidableBsdf &&
					(pathInfo->depth.depth >=
						taskConfig->pathTracer.guidingMinDepth) &&
					guideLeaf &&
					((uint)guideLeaf[22] > 0u) &&
					(guideLeaf[20] >= GUIDE_WARMUP_RECORDS);
			// Guiding stats
			if (tryGuide)
				guideDbgBuff[0] = 1u;
			// M2c adaptive mixture (mirrors the CPU side): selection
			// probability from the leaf record count x PeakGate, scaled
			// by the artist strength dial (path.guiding.strength).
			const float wGuide = tryGuide ?
					taskConfig->pathTracer.guidingStrength *
					Guide_MixWeight(guideLeaf[20], guideLeaf[21]) : .5f;
			const float uSelRaw = Sampler_GetSample(taskConfig, sampleOffset + IDX_BSDF_X SAMPLER_PARAM);
			const bool takeGuideSide = (uSelRaw < wGuide);
			const float uSelRescaled = takeGuideSide ?
					uSelRaw / max(wGuide, 1e-6f) :
					(uSelRaw - wGuide) / max(1.f - wGuide, 1e-6f);
			// Portal bounce proposal (M5): share + selector decided in
			// MK_HIT_OBJECT; portalW wraps every technique's density in
			// wP*pPortal + (1-wP)*rest (mirrored CPU predicates).
			const float wPortal = taskState->portalW;
			const uint portalCount = taskConfig->pathTracer.portalCount;
			bool guided = false;
			if (taskState->risZhat > 0.f) {
				// RIS product-guiding winner (drawn pre-DL in
				// MK_HIT_OBJECT): the continuation weight is
				// f|cos|*zHat/t(w*) and pHat is the density the DL MIS
				// partner already saw (a consistent pair).
				sampledDir = MAKE_FLOAT3(taskState->risDirX,
						taskState->risDirY, taskState->risDirZ);
				bsdfPdfW = taskState->risPHat;
				bsdfSample = MAKE_FLOAT3(taskState->risWtR,
						taskState->risWtG, taskState->risWtB);
				cosSampledDir = fabs(dot(VLOAD3F(&bsdf->hitPoint.shadeN.x),
						sampledDir));
				if (taskState->risSideBsdf != 0u) {
					bsdfEvent = (BSDFEvent)taskState->risEvent;
				} else {
					// Field-side winner: shadow BSDF draw with the
					// candidate's own uniforms for single-lobe event
					// bookkeeping (same trick as the mixture path).
					float3 discardDir;
					float discardPdfW, discardCos;
					BSDFEvent shadowEvent = (BSDFEvent)0;
					const float3 discardEval = BSDF_Sample(bsdf,
							taskState->risUD0, taskState->risUD1,
							&discardDir, &discardPdfW, &discardCos,
							&shadowEvent
							MATERIALS_PARAM);
					bsdfEvent = Spectrum_IsBlack(discardEval) ?
							(BSDFEvent)taskState->risEvent : shadowEvent;
				}
#if defined(SLG_SPECTRAL)
				// The candidate's BSDF_Sample ran in MK_HIT_OBJECT (and
				// possibly the shadow draw just above): a dispersive
				// transmit may have collapsed the alive mask on the hit
				// point - carry it back so the next intersection keeps it.
				sampleResult->spectralHeroAlive = bsdf->hitPoint.spectralHeroAlive;
#endif
				guided = true;
			}
			if (!guided && tryGuide && takeGuideSide &&
					(taskState->portalTake == 0u)) {
				float guidePdfW;
				float3 guideDir;
				const float3 shadeN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
				const float uBin = GuidingHash(
						(sampleResult->pixelX * 73856093u) ^
						(sampleResult->pixelY * 19349663u) ^
						(GuidingPass(taskConfig, gid, samplesBuff) * 83492791u) ^
						(sampleOffset * 2971215073u)) * (1.f / 4294967296.f);
				if (GuideTree_Sample(guideLeaf,
						shadeN, uBin, uSelRescaled,
						Sampler_GetSample(taskConfig, sampleOffset + IDX_BSDF_Y SAMPLER_PARAM),
						&guideDir, &guidePdfW, bsdf->isVolume) && (guidePdfW > 0.f)) {
					float3 discardDir;
					float discardPdfW, discardCos;
					BSDFEvent shadowEvent = (BSDFEvent)0;
					const float3 discardEval = BSDF_Sample(bsdf,
							uSelRescaled,
							Sampler_GetSample(taskConfig, sampleOffset + IDX_BSDF_Y SAMPLER_PARAM),
							&discardDir, &discardPdfW, &discardCos, &shadowEvent
							MATERIALS_PARAM);
					BSDFEvent guideEvent;
					float guideBsdfPdfW;
					const float3 guideEvalDouble = BSDF_Evaluate(bsdf,
							guideDir, &guideEvent, &guideBsdfPdfW
							MATERIALS_PARAM);
					// Same Disney double-cos workaround as the CPU side:
					// DISNEY only - every other Evaluate already returns
					// single-cos f*|cos| (volumes: phase*albedo, no cos).
					const float cosLocal = fabs(Frame_ToLocal(&bsdf->frame, guideDir).z);
					const float3 guideEval = (mats[bsdf->materialIndex].type == DISNEY) ?
							((cosLocal > 1e-3f) ? guideEvalDouble / cosLocal : BLACK) :
							guideEvalDouble;
					if (!Spectrum_IsBlack(guideEval)) {
						// The portal share wraps the rest-mixture
						// symmetric to the BSDF side (CPU parity).
						float mixPdfW = (1.f - wGuide) * guideBsdfPdfW + wGuide * guidePdfW;
						if (wPortal > 0.f)
							mixPdfW = wPortal * Portal_PdfW(portalRects,
									portalCount, VLOAD3F(&bsdf->hitPoint.p.x),
									guideDir) + (1.f - wPortal) * mixPdfW;
						if (mixPdfW > 0.f) {
							sampledDir = guideDir;
							bsdfSample = guideEval / mixPdfW;
							bsdfPdfW = mixPdfW;
							cosSampledDir = fabs(dot(shadeN, sampledDir));
									bsdfEvent = Spectrum_IsBlack(discardEval) ? guideEvent : shadowEvent;
									guided = true;
									// Guiding stats
									guideDbgBuff[1] = 1u;
								} else {
							guided = true;
							bsdfSample = BLACK;
						}
					} else {
						guided = true;
						bsdfSample = BLACK;
					}
				} else {
					guided = true;
					bsdfSample = BLACK;
				}
			}
			if (taskState->portalTake != 0u) {
				// Portal proposal (M5, mirrors the CPU takePortal block):
				// aim at a uniform point on a uniformly picked aperture
				// rect; the marginal density is wP*PortalPdfW +
				// (1-wP)*restPdfW with rest evaluated exactly as the
				// BSDF/guide sides (single-cos f|cos| convention).
				const uint portalSalt =
						(sampleResult->pixelX * 2654435761u) ^
						(sampleResult->pixelY * 2246822519u) ^
						(GuidingPass(taskConfig, gid, samplesBuff) *
							3266489917u) ^
						(sampleOffset * 668265263u);
				const float uIdx = GuidingHash(portalSalt ^ 0x9e3779b9u) *
						(1.f / 4294967296.f);
				const float uPU = GuidingHash(portalSalt ^ 0x85ebca6bu) *
						(1.f / 4294967296.f);
				const float uPV = GuidingHash(portalSalt ^ 0xc2b2ae35u) *
						(1.f / 4294967296.f);
				__global const float4 *pr = portalRects + 4u *
						min((uint)(uIdx * portalCount), portalCount - 1u);
				const float3 hp = VLOAD3F(&bsdf->hitPoint.p.x);
				const float3 pt = MAKE_FLOAT3(pr[0].x, pr[0].y, pr[0].z) +
						uPU * MAKE_FLOAT3(pr[1].x, pr[1].y, pr[1].z) +
						uPV * MAKE_FLOAT3(pr[2].x, pr[2].y, pr[2].z);
				sampledDir = normalize(pt - hp);
				BSDFEvent pEvent;
				float pBsdfPdfW;
				float3 pEval = BSDF_Evaluate(bsdf, sampledDir,
						&pEvent, &pBsdfPdfW
						MATERIALS_PARAM);
				// Disney double-cos correction, same as the guide and
				// RIS candidate evaluations.
				if (mats[bsdf->materialIndex].type == DISNEY) {
					const float cosLocal = fabs(Frame_ToLocal(&bsdf->frame,
							sampledDir).z);
					pEval = (cosLocal > 1e-3f) ? pEval / cosLocal : BLACK;
				}
				const float3 shadeN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
				float restPdfW = pBsdfPdfW;
				if (tryGuide)
					restPdfW = (1.f - wGuide) * pBsdfPdfW + wGuide *
							GuideTree_Pdf(guideLeaf, shadeN, sampledDir,
							bsdf->isVolume);
				const float mixPdfW = wPortal * Portal_PdfW(portalRects,
						portalCount, hp, sampledDir) +
						(1.f - wPortal) * restPdfW;
				if (!Spectrum_IsBlack(pEval) && (mixPdfW > 0.f)) {
					bsdfSample = pEval / mixPdfW;
					bsdfPdfW = mixPdfW;
					cosSampledDir = fabs(dot(shadeN, sampledDir));
					// Single-lobe event bookkeeping via a shadow BSDF
					// draw (same convention as the guide side).
					float3 discardDir;
					float discardPdfW, discardCos;
					BSDFEvent shadowEvent = (BSDFEvent)0;
					const float3 discardEval = BSDF_Sample(bsdf,
							GuidingHash(portalSalt ^ 0x27d4eb2fu) *
								(1.f / 4294967296.f),
							GuidingHash(portalSalt ^ 0x165667b1u) *
								(1.f / 4294967296.f),
							&discardDir, &discardPdfW, &discardCos,
							&shadowEvent
							MATERIALS_PARAM);
					bsdfEvent = Spectrum_IsBlack(discardEval) ?
							pEvent : shadowEvent;
				} else {
					// Valid portal draw, zero BSDF contribution (e.g.
					// below the shading hemisphere): kill the path
					// rather than resample under mixture weights.
					bsdfSample = BLACK;
				}
				guided = true;
#if defined(SLG_SPECTRAL)
				sampleResult->spectralHeroAlive =
						bsdf->hitPoint.spectralHeroAlive;
#endif
			}
			if (!guided) {
				const float uBsdf = tryGuide ?
						uSelRescaled : Sampler_GetSample(taskConfig, sampleOffset + IDX_BSDF_X SAMPLER_PARAM);
				bsdfSample = BSDF_Sample(bsdf,
						uBsdf,
						Sampler_GetSample(taskConfig, sampleOffset + IDX_BSDF_Y SAMPLER_PARAM),
						&sampledDir, &bsdfPdfW, &cosSampledDir, &bsdfEvent
						MATERIALS_PARAM);
#if defined(SLG_SPECTRAL)
				// A dispersive transmit may have collapsed the alive mask on
				// the hit point: carry it back so the next intersection
				// (which re-copies from the SampleResult) keeps it.
				sampleResult->spectralHeroAlive = bsdf->hitPoint.spectralHeroAlive;
#endif
				if (tryGuide || (wPortal > 0.f)) {
					// Every BSDF-side sample under tryGuide (whichever
					// way the selector fell) is reweighted to the
					// mixture; the portal share wraps it symmetric to
					// the guide side (mirrored CPU bookkeeping).
					float mixPdfW = bsdfPdfW;
					if (tryGuide) {
						const float3 shadeN = VLOAD3F(&bsdf->hitPoint.shadeN.x);
						const float guidePdfW = GuideTree_Pdf(guideLeaf,
								shadeN, sampledDir, bsdf->isVolume);
						mixPdfW = (1.f - wGuide) * bsdfPdfW + wGuide * guidePdfW;
					}
					if (wPortal > 0.f)
						mixPdfW = wPortal * Portal_PdfW(portalRects,
								portalCount, VLOAD3F(&bsdf->hitPoint.p.x),
								sampledDir) + (1.f - wPortal) * mixPdfW;
					if (mixPdfW > 0.f) {
						bsdfSample *= bsdfPdfW / mixPdfW;
						bsdfPdfW = mixPdfW;
					}
				}
			}
			} // giPending != 1u && ptPending != 1u

			// Path guiding (P1-3 M2b-2): incident-value training record
			// (strided per-task slot, no atomics). Exact record matching
			// the CPU Record() contract: p = ray->o (the vertex the ray
			// LEFT - where future queries land), d = ray->d (toward the
			// contributing vertex), flux = local DL+emission added at
			// this vertex / arrival throughput. Gated like the CPU: no
			// delta vertices, no depth 0 (a camera-origin record sits
			// where nothing queries and would use a garbage radStart
			// baseline).
			// NOTE: guideRec*/guideDbgBuff are null buffers when
			// path.guiding.enable is off - the writes must be gated.
			if (guidingEnable != 0u) {
				// NOTE: MAKE_FLOAT3 (raw (float3)(...) splats on Metal,
				// bare float3(...) rejected by Apple OpenCL).
				const float3 radNow = MAKE_FLOAT3(
						sampleResult->directDiffuse.c[0] + sampleResult->directGlossy.c[0] + sampleResult->emission.c[0],
						sampleResult->directDiffuse.c[1] + sampleResult->directGlossy.c[1] + sampleResult->emission.c[1],
						sampleResult->directDiffuse.c[2] + sampleResult->directGlossy.c[2] + sampleResult->emission.c[2]);
				const float3 radStart = MAKE_FLOAT3(
						taskState->guideRadStart[0],
						taskState->guideRadStart[1],
						taskState->guideRadStart[2]);
				taskState->guideRadStart[0] = radNow.x;
				taskState->guideRadStart[1] = radNow.y;
				taskState->guideRadStart[2] = radNow.z;
				const float localValue = (radNow.x + radNow.y + radNow.z -
						radStart.x - radStart.y - radStart.z) * (1.f / 3.f);
				if ((pathInfo->depth.depth >= 1u) &&
						!BSDF_IsDelta(bsdf MATERIALS_PARAM)) {
					const float3 thr = VLOAD3F(&taskState->throughput.c[0]);
					const float arrival = max((thr.x + thr.y + thr.z) * (1.f / 3.f), 1e-3f);
					const float flux = localValue / arrival;
					const float3 recP = VLOAD3F(&ray->o.x);
					const float3 recD = VLOAD3F(&ray->d.x);
					__global float4 *rec = Guide_RecBuf((uint)gid,
							guideRec0, guideRec1, guideRec2, guideRec3,
							guideRec4, guideRec5, guideRec6, guideRec7,
							guideRec8, guideRec9, guideRec10, guideRec11,
							guideRec12, guideRec13, guideRec14, guideRec15) +
							2u * (((uint)gid >> 5) & 127u);
					rec[0] = MAKE_FLOAT4(recP.x, recP.y, recP.z, flux);
					rec[1] = MAKE_FLOAT4(recD.x, recD.y, recD.z, 1.f);
					guideDbgBuff[2] = 1u;
				}
			}

			pathInfo->isPassThroughPath = false;
		}
	}

	if (sampleResult->firstPathVertex)
		sampleResult->firstPathVertexEvent = bsdfEvent;

	// A vertex that does not extend a nearly-specular chain starts a new
	// one: record whether the caustic cache was queried here (same gate
	// as the MK_HIT_OBJECT ConnectWithCausticPaths call - hybrid leaves
	// depth 0 to the light pass). CPU twin in pathtracer.cpp.
	if ((taskConfig->pathTracer.pgic.indirectEnabled || taskConfig->pathTracer.pgic.causticEnabled) &&
			PhotonGICache_IsCausticChainReceiver(bsdfEvent, BSDF_GetGlossiness(bsdf MATERIALS_PARAM),
				taskConfig->pathTracer.pgic.glossinessUsageThreshold))
		taskState->photonGICausticChainCached = taskConfig->pathTracer.pgic.causticEnabled &&
				(taskConfig->pathTracer.pgic.debugType == PGIC_DEBUG_NONE) &&
				(!taskConfig->pathTracer.hybridBackForward.enabled || (pathInfo->depth.depth != 0)) &&
				PhotonGICache_IsPhotonGIEnabled(bsdf,
						taskConfig->pathTracer.pgic.glossinessUsageThreshold,
						taskConfig->pathTracer.pgic.causticVolumeBeams
						MATERIALS_PARAM);

	EyePathInfo_AddVertex(pathInfo, bsdf, bsdfEvent, bsdfPdfW,
			taskConfig->pathTracer.hybridBackForward.glossinessThreshold
			LPE_PARAM
			MATERIALS_PARAM);

	// Cycles sample clamp for the next vertex (CPU RenderEyePath twin): an
	// emitter hit counts as the bounce the hitting ray left from (direct
	// up to depth 1), next-event estimation as the vertex's own bounce
	sampleResult->clampEmission = (pathInfo->depth.depth <= 1) ?
			taskConfig->pathTracer.cyclesClampDirect : taskConfig->pathTracer.cyclesClampIndirect;
	sampleResult->clampDirect = (pathInfo->depth.depth == 0) ?
			taskConfig->pathTracer.cyclesClampDirect : taskConfig->pathTracer.cyclesClampIndirect;

	// SSP tail recorder (path.ssp.enable): the leading delta-specular
	// run is remembered so a paired light task's blocked camera connect
	// can replay this topology instead of discovering it. The depth-1
	// call resets the record, so per-path reset needs no init plumbing.
	if (sspTails)
		SspTail_RecordVertex(&sspTails[gid], bsdf, cosSampledDir,
				pathInfo->depth.depth
				MATERIALS_PARAM);

	// Russian Roulette
	const bool rrEnabled = EyePathInfo_UseRR(pathInfo, taskConfig->pathTracer.rrDepth);
	const float rrProb = rrEnabled ?
		RussianRouletteProb(taskConfig->pathTracer.rrImportanceCap, bsdfSample) :
		1.f;
	const bool rrContinuePath = !rrEnabled ||
		!(rrProb < Sampler_GetSample(taskConfig, sampleOffset + IDX_RR SAMPLER_PARAM));

	// Max. path depth
	const bool maxPathDepth = (pathInfo->depth.depth >= taskConfig->pathTracer.maxPathDepth.depth);

	const bool continuePath = !Spectrum_IsBlack(bsdfSample) && rrContinuePath && !maxPathDepth;
	if (continuePath) {
		float3 throughputFactor = WHITE;

		// RR increases path contribution
		throughputFactor /= rrProb;
		throughputFactor *= bsdfSample;

		VSTORE3F(throughputFactor * VLOAD3F(taskState->throughput.c), taskState->throughput.c);

		if (taskConfig->pathTracer.vertexConnect.enabled) {
			if (cosSampledDir < 0.f) {
				// Pass-through vertex (shadowTransparency): not a real
				// vertex on the CPU side - undo the hit fold
				pathInfo->dVCM /= pathInfo->vcFoldVCM;
				pathInfo->dVC /= pathInfo->vcFoldVC;
				pathInfo->dVM /= pathInfo->vcFoldVC;
			} else {
				// CPU Bounce() MIS update:
				//   specular: dVCM = 0, dVC *= MIS(cos), dVM *= MIS(cos)
				//   else:     dVC  = MIS(cos/pdfW) *
				//                (dVC*MIS(revPdfW) + dVCM + misVmW)
				//             dVM  = MIS(cos/pdfW) *
				//                (dVM*MIS(revPdfW) + dVCM*misVcW + 1)
				//             dVCM = MIS(1/pdfW)
				// (misVmW/misVcW = 0 -> pure BDPT; they are nonzero only
				// when vertex merging is on)
				// NOTE: under guiding/portal/RIS proposals bsdfPdfW is the
				// mixture density while bsdfRevPdfW stays the pure-BSDF
				// reverse density (no CPU reference exists for that
				// combination - approximation, documented).
				const float vcMisVcW = taskConfig->pathTracer.vertexConnect.
						misVcWeightFactor;
				const float vcMisVmW = taskConfig->pathTracer.vertexConnect.
						misVmWeightFactor;
				float bsdfRevPdfW;
				if (bsdfEvent & SPECULAR)
					bsdfRevPdfW = bsdfPdfW;
				else
					BSDF_Pdf(bsdf, sampledDir, NULL, &bsdfRevPdfW
							MATERIALS_PARAM);
				if (bsdfEvent & SPECULAR) {
					pathInfo->dVCM = 0.f;
					const float specFactor = bsdf->isVolume ? 1.f : VCMis(cosSampledDir);
					pathInfo->dVC *= specFactor;
					pathInfo->dVM *= specFactor;
				} else {
					const float w = VCMis((bsdf->isVolume ? 1.f : cosSampledDir) / bsdfPdfW);
					pathInfo->dVC = w * (pathInfo->dVC *
							VCMis(bsdfRevPdfW) + pathInfo->dVCM + vcMisVmW);
					pathInfo->dVM = w * (pathInfo->dVM *
							VCMis(bsdfRevPdfW) + pathInfo->dVCM * vcMisVcW +
							1.f);
					pathInfo->dVCM = VCMis(1.f / bsdfPdfW);
				}
			}
		}

		// This is valid for irradiance AOV only if it is not a SPECULAR material and
		// first path vertex. Set or update sampleResult.irradiancePathThroughput
		if (sampleResult->firstPathVertex) {
			if (!(BSDF_GetEventTypes(&taskState->bsdf
						MATERIALS_PARAM) & SPECULAR))
				VSTORE3F(TO_FLOAT3(M_1_PI_F * fabs(dot(
						VLOAD3F(&bsdf->hitPoint.shadeN.x),
						sampledDir)) / rrProb),
						sampleResult->irradiancePathThroughput.c);
			else
				VSTORE3F(BLACK, sampleResult->irradiancePathThroughput.c);
		} else
			VSTORE3F(throughputFactor * VLOAD3F(sampleResult->irradiancePathThroughput.c), sampleResult->irradiancePathThroughput.c);

		Ray_Init2(ray, BSDF_GetRayOrigin(bsdf, sampledDir), sampledDir, ray->time);

		sampleResult->firstPathVertex = false;

		// Initialize the pass-through event seed
		//
		// Note: I use the IDX_PASSTHROUGH of the next path depth
		const uint nextSampleOffset = taskConfig->pathTracer.eyeSampleBootSize + pathInfo->depth.depth * taskConfig->pathTracer.eyeSampleStepSize;
		const float passThroughEvent = Sampler_GetSample(taskConfig, nextSampleOffset + IDX_PASSTHROUGH SAMPLER_PARAM);
		Seed seedPassThroughEvent;
		Rnd_InitFloat(passThroughEvent, &seedPassThroughEvent);
		taskState->seedPassThroughEvent = seedPassThroughEvent;

		// Initialize the trough a shadow transparency flag used by Scene_Intersect()
		taskState->throughShadowTransparency = false;


		pathState = MK_RT_NEXT_VERTEX;
	} else
		pathState = MK_SPLAT_SAMPLE;

	// Save the state
	taskState->state = pathState;

	//--------------------------------------------------------------------------

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_SPLAT_SAMPLE
// To: MK_NEXT_SAMPLE
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_SPLAT_SAMPLE(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_SPLAT_SAMPLE(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_SPLAT_SAMPLE)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__constant const Film* restrict film = &taskConfig->film;
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	// Initialize Film radiance group pointer table
	__global float *filmRadianceGroup[FILM_MAX_RADIANCE_GROUP_COUNT];
	filmRadianceGroup[0] = filmRadianceGroup0;
	filmRadianceGroup[1] = filmRadianceGroup1;
	filmRadianceGroup[2] = filmRadianceGroup2;
	filmRadianceGroup[3] = filmRadianceGroup3;
	filmRadianceGroup[4] = filmRadianceGroup4;
	filmRadianceGroup[5] = filmRadianceGroup5;
	filmRadianceGroup[6] = filmRadianceGroup6;
	filmRadianceGroup[7] = filmRadianceGroup7;

	// Initialize Film radiance group scale table
	float3 filmRadianceGroupScale[FILM_MAX_RADIANCE_GROUP_COUNT];
	filmRadianceGroupScale[0] = MAKE_FLOAT3(filmRadianceGroupScale0_R, filmRadianceGroupScale0_G, filmRadianceGroupScale0_B);
	filmRadianceGroupScale[1] = MAKE_FLOAT3(filmRadianceGroupScale1_R, filmRadianceGroupScale1_G, filmRadianceGroupScale1_B);
	filmRadianceGroupScale[2] = MAKE_FLOAT3(filmRadianceGroupScale2_R, filmRadianceGroupScale2_G, filmRadianceGroupScale2_B);
	filmRadianceGroupScale[3] = MAKE_FLOAT3(filmRadianceGroupScale3_R, filmRadianceGroupScale3_G, filmRadianceGroupScale3_B);
	filmRadianceGroupScale[4] = MAKE_FLOAT3(filmRadianceGroupScale4_R, filmRadianceGroupScale4_G, filmRadianceGroupScale4_B);
	filmRadianceGroupScale[5] = MAKE_FLOAT3(filmRadianceGroupScale5_R, filmRadianceGroupScale5_G, filmRadianceGroupScale5_B);
	filmRadianceGroupScale[6] = MAKE_FLOAT3(filmRadianceGroupScale6_R, filmRadianceGroupScale6_G, filmRadianceGroupScale6_B);
	filmRadianceGroupScale[7] = MAKE_FLOAT3(filmRadianceGroupScale7_R, filmRadianceGroupScale7_G, filmRadianceGroupScale7_B);

	if (sampleResult->isHoldout) {
		SampleResult_ClearRadiance(sampleResult);
		VSTORE3F(BLACK, sampleResult->albedo.c);
	}

	if (taskConfig->pathTracer.pgic.indirectEnabled &&
			(taskConfig->pathTracer.pgic.debugType == PGIC_DEBUG_SHOWINDIRECTPATHMIX) &&
			!taskState->photonGIShowIndirectPathMixUsed)
		VSTORE3F(MAKE_FLOAT3(1.f, 0.f, 0.f), sampleResult->radiancePerPixelNormalized[0].c);

	//--------------------------------------------------------------------------
	// Variance clamping
	//--------------------------------------------------------------------------

	const float sqrtVarianceClampMaxValue = taskConfig->pathTracer.sqrtVarianceClampMaxValue;
	if (sqrtVarianceClampMaxValue > 0.f) {
		// Radiance clamping (Adaptive Robust Clamping: median/MAD margin +
		// path-class scope)
		VarianceClamping_Clamp(sampleResult, sqrtVarianceClampMaxValue,
				taskConfig->pathTracer.varianceClampAdaptive,
				taskConfig->pathTracer.varianceClampScope,
				taskConfig->pathTracer.varianceClampSigma
				FILM_PARAM);
	}

	//--------------------------------------------------------------------------
	// Sampler splat sample
	//--------------------------------------------------------------------------

	Sampler_SplatSample(taskConfig
			SAMPLER_PARAM
			FILM_PARAM);
	taskStats[gid].sampleCount += 1;

	// ReSTIR PT (PT-2): path-end commit - publish the measured suffix
	// L_suf = (radEnd - radX2) / thrX2 to the pixel's reservoir.
	// pending == 4 = armed pick landed; pending == 3 = the armed winner
	// was killed before landing (RR/depth) - dropped uncommitted,
	// matching the CPU's ptArmed && ptCaptured gate.
	if (taskConfig->pathTracer.restirPT.enabled &&
			(taskConfig->pathTracer.restirPT.ptCandCount > 0u)) {
		const uint ptK = taskConfig->pathTracer.restirPT.ptCandCount;
		__global RestirPTResult *ptResult = (__global RestirPTResult *)
				(((__global RestirGICandidate *)(restirReservoirs +
				taskConfig->pathTracer.restirPT.ptCandDataOffset +
				gid * taskConfig->pathTracer.restirPT.ptCandStride)) +
				ptK);
		if (ptResult->pending == 4u) {
			const float3 lsuf = RestirPT_Commit(
					(__global RestirPTReservoir *)(restirReservoirs +
						taskConfig->pathTracer.restirPT.ptReservoirOffset) +
						sampleResult->pixelY * filmWidth +
						sampleResult->pixelX,
					ptResult,
					VLOAD3F(&sampleResult->
						radiancePerPixelNormalized[0].c[0]),
					GuidingPass(taskConfig, gid, samplesBuff));
			// ReSTIR PG feed: landed pick, measured suffix - emit the
			// accepted direction as a guiding training record.
			if (guidingEnable != 0u) {
				const float flux = Spectrum_Y(lsuf);
				if (flux > 0.f)
					Guide_EmitRec(gid,
							MAKE_FLOAT3(ptResult->x1X, ptResult->x1Y,
									ptResult->x1Z),
							MAKE_FLOAT3(ptResult->dirX, ptResult->dirY,
									ptResult->dirZ),
							flux,
							guideRec0, guideRec1, guideRec2, guideRec3,
							guideRec4, guideRec5, guideRec6, guideRec7,
							guideRec8, guideRec9, guideRec10, guideRec11,
							guideRec12, guideRec13, guideRec14,
							guideRec15);
			}
		}
		ptResult->pending = 0u;
	}

	// Save the state
	taskState->state = MK_NEXT_SAMPLE;

	//--------------------------------------------------------------------------

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_NEXT_SAMPLE
// To: MK_GENERATE_CAMERA_RAY
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_NEXT_SAMPLE(
		KERNEL_ARGS
		) {
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_NEXT_SAMPLE(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_NEXT_SAMPLE)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	Sampler_NextSample(taskConfig,
			filmNoise,
			filmUserImportance,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3
			SAMPLER_PARAM);

	// Save the state

	// Generate a new path and camera ray only it is not TILEPATHOCL
#if !defined(RENDER_ENGINE_TILEPATHOCL) && !defined(RENDER_ENGINE_RTPATHOCL)
	taskState->state = MK_GENERATE_CAMERA_RAY;
#else
	taskState->state = MK_DONE;
	// Mark the ray like like one to NOT trace
	rays[gid].flags = RAY_FLAGS_MASKED;
#endif

	//--------------------------------------------------------------------------

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_GENERATE_CAMERA_RAY
// To: MK_RT_NEXT_VERTEX
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_GENERATE_CAMERA_RAY(
		KERNEL_ARGS
		) {
	// Generate a new path and camera ray only it is not TILEPATHOCL: path regeneration
	// is not used in this case
#if !defined(RENDER_ENGINE_TILEPATHOCL) && !defined(RENDER_ENGINE_RTPATHOCL)
	WAVEFRONT_GUARD

	// Read the path state
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_GENERATE_CAMERA_RAY(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_GENERATE_CAMERA_RAY)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	// Read the seed
	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__global Ray *ray = &rays[gid];
	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	
	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	// Re-initialize the volume information
	PathVolumeInfo_Init(&pathInfo->volume);

	GenerateEyePath(taskConfig,
			&tasksDirectLight[gid], taskState,
			camera,
			cameraBokehDistribution,
			filmWidth, filmHeight,
			filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3,
			pixelFilterDistribution,
			ray,
			pathInfo
			LPE_PARAM
			SAMPLER_PARAM);
	// taskState->state is set to RT_NEXT_VERTEX inside GenerateEyePath()

	//--------------------------------------------------------------------------

	// Save the seed
	task->seed = seedValue;

#endif
}

//------------------------------------------------------------------------------
// GPU light tracing (doc/features/gpu_lighttracing.md)
//
// Light-path helpers: the LightPathInfo analogue of the EyePathInfo
// functions in pathinfo_funcs.cl, ported from
// src/slg/utils/pathinfo.cpp::LightPathInfo.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE void LightPathInfo_Init(__global LightPathInfo *lpi) {
	PathDepthInfo_Init(&lpi->depth);
	PathVolumeInfo_Init(&lpi->volume);
	PathVolumeInfo_Init(&lpi->connectVolInfo);

	lpi->connectThroughShadow = false;
	lpi->lastBSDFEvent = SPECULAR; // SPECULAR is required to avoid MIS
	lpi->lastFromVolume = false;
	lpi->isNearlyS = false;
	lpi->isNearlySD = false;
	lpi->isNearlySDS = false;
	lpi->isAdaptiveS = false;
	lpi->firstVertSeen = false;
	lpi->firstVertPX = 0.f;
	lpi->firstVertPY = 0.f;
	lpi->firstVertPZ = 0.f;
	lpi->firstVertGloss = 0.f;
	lpi->firstVertDelta = 1;
	lpi->hasDeltaVertex = false;
	lpi->pathDone = false;
	lpi->mneeActive = false;
	// Vertex connection (M6): a new light subpath owns no cached
	// vertices yet
	lpi->dVCM = 0.f;
	lpi->dVC = 0.f;
	lpi->dVM = 0.f;
	lpi->vcVertexCount = 0;
	lpi->pendingSplat.fromMnee = false;
	lpi->pendingSplat.valid = false;
}

OPENCL_FORCE_INLINE bool LightPathInfo_IsNearlySpecular(
		const BSDFEvent event, const float glossiness,
		const float glossinessThreshold) {
	return (event & SPECULAR) ||
			((event & GLOSSY) && (glossiness <= glossinessThreshold));
}

OPENCL_FORCE_INLINE void LightPathInfo_AddVertex(__global LightPathInfo *lpi,
		__global const BSDF *bsdf, const BSDFEvent event,
		const float glossinessThreshold
		MATERIALS_PARAM_DECL) {
	PathDepthInfo_IncDepthsVol(&lpi->depth, event, bsdf->isVolume);
	PathVolumeInfo_Update(&lpi->volume, event, bsdf MATERIALS_PARAM);

	const float glossiness = BSDF_GetGlossiness(bsdf MATERIALS_PARAM);
	const bool isNewVertexNearlySpecular = LightPathInfo_IsNearlySpecular(
			event, glossiness, glossinessThreshold);

	// Same order as CPU PathInfo::AddVertex(): SDS before SD before S.
	// Media-transparent chains: a medium scattering vertex neither
	// extends nor breaks the run (doc/features/caustics-sota.md).
	const bool chainMedium = BSDF_IsChainTransparentMedium(bsdf MATERIALS_PARAM);
	// The next flight starts at this vertex (beam deposit gate)
	lpi->lastFromVolume = bsdf->isVolume;
	const bool chainSpec = isNewVertexNearlySpecular || chainMedium;
	lpi->isNearlySDS = (lpi->isNearlySD || lpi->isNearlySDS) && chainSpec;
	lpi->isNearlySD = lpi->isNearlyS && !chainSpec;
	lpi->isNearlyS = ((lpi->depth.depth == 1) || lpi->isNearlyS) && chainSpec;

	// Adaptive partition: any non-diffuse or medium vertex keeps the
	// chain alive; the first NON-MEDIUM vertex is the terminal of the
	// eye-side connection-difficulty test.
	lpi->isAdaptiveS = ((lpi->depth.depth == 1) || lpi->isAdaptiveS) &&
			(((event & (SPECULAR | GLOSSY)) != 0) || chainMedium);
	if (!chainMedium && !lpi->firstVertSeen) {
		lpi->firstVertSeen = true;
		const float3 hp = VLOAD3F(&bsdf->hitPoint.p.x);
		lpi->firstVertPX = hp.x;
		lpi->firstVertPY = hp.y;
		lpi->firstVertPZ = hp.z;
		lpi->firstVertGloss = glossiness;
		// PSR parity with CPU LightPathInfo::AddVertex: a regularized
		// delta surface must still end the eye-hard chain - read the
		// static material flag, not the widened (GLOSSY) sampled event.
		lpi->firstVertDelta = ((event & SPECULAR) ||
				Material_IsDelta(bsdf->materialIndex MATERIALS_PARAM)) ? 1 : 0;
	}

	lpi->lastBSDFEvent = event;
}

OPENCL_FORCE_INLINE bool LightPathInfo_UseRR(__global LightPathInfo *lpi,
		const uint rrDepth) {
	return !(lpi->lastBSDFEvent & SPECULAR) &&
			(PathDepthInfo_GetRRDepth(&lpi->depth) >= rrDepth);
}

// Port of LightPathInfo::IsCausticPath(): the path so far is nearly
// specular and the candidate connection event is not
OPENCL_FORCE_INLINE bool LightPathInfo_IsCausticPath(__global const LightPathInfo *lpi,
		const BSDFEvent event, const float glossiness,
		const float glossinessThreshold) {
	// firstVertSeen keeps pure-medium prefixes out of the caustic class
	// (a vacuously-alive chain carries no focusing surface)
	return lpi->isNearlyS && lpi->firstVertSeen && (lpi->depth.depth + 1 > 1) &&
			!LightPathInfo_IsNearlySpecular(event, glossiness, glossinessThreshold);
}

// Adaptive counterpart: the path so far is all non-diffuse
// (isAdaptiveS), the receiver is non-delta and the light-adjacent
// vertex v1 is hard for the eye path (delta, or a sharp lobe facing a
// tiny light solid angle). Same partition the eye side applies, so the
// classes stay disjoint.
OPENCL_FORCE_INLINE bool LightPathInfo_IsAdaptiveCausticPath(
		__global const LightPathInfo *lpi, const BSDFEvent event,
		const float terminalGlossiness, const float connectProb,
		__global const LightSource* restrict light) {
	return lpi->isAdaptiveS && (lpi->depth.depth + 1 > 1) &&
			!(event & SPECULAR) && lpi->firstVertSeen &&
			CausticPath_IsTerminalHard(terminalGlossiness, connectProb,
					lpi->firstVertDelta, lpi->firstVertGloss,
					Light_ConnectionSolidAngle(light,
							MAKE_FLOAT3(lpi->firstVertPX, lpi->firstVertPY,
									lpi->firstVertPZ)));
}

//------------------------------------------------------------------------------
// MK_LIGHT_INIT: draw the next light-path sample - pick the emitter,
// emit the path ray into rays[gid], sample the lens point.
//
// From: MK_LIGHT_INIT (also the entry state for light tasks)
// To: MK_LIGHT_VERTEX
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_LIGHT_INIT(
		KERNEL_ARGS
		KERNEL_ARGS_LIGHT
		) {
	WAVEFRONT_GUARD
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_LIGHT_INIT(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_LIGHT_INIT)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	Seed seedValue = task->seed;
	// This trick is required by SAMPLER_PARAM macro
	Seed *seed = &seedValue;

	__constant const PathTracer* restrict pathTracer = &taskConfig->pathTracer;
	const uint lightIndex = gid - pathTracer->lightTracing.eyeTaskCount;
	__global LightPathInfo *lpi = &lightPathInfos[lightIndex];
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	// PhotonGI deposit mode (B1'): when the host retires deposition
	// (cache full, or the scene proved caustic-free) it clears
	// pgic.depositEnabled in taskConfig - deposit-only tasks then park
	// here instead of tracing paths that produce nothing.
	if (!pathTracer->lightTracing.enabled && !pathTracer->pgic.depositEnabled) {
		taskState->state = MK_DONE;
		rays[gid].flags = RAY_FLAGS_MASKED;
		return;
	}

	// Advance the light-sample sequence (dims map to sampler dims >= 2)
	Sampler_LightNextSample(taskConfig
			SAMPLER_PARAM);

	LightPathInfo_Init(lpi);
	// PSR: light-side vertices regularize identically (vertex connect /
	// splat consistency with the eye side)
	lpi->depth.regularization = taskConfig->pathTracer.regularizationSigma;
	lpi->depth.regularizationMinDepth = taskConfig->pathTracer.regularizationMinDepth;

	// A light sample was drawn; count it even if the emission fails
	// (mirrors PathTracerThreadState::lightSampleCount accounting)
	taskStats[gid].sampleCount += 1;

	const uint bootSize = pathTracer->lightTracing.lightSampleBootSize;

#if defined(SLG_SPECTRAL)
	// Hero-wavelength spectral transport: the wavelength dimension sits
	// at the end of the boot block (see PathTracer::RenderLightSample)
	const float wavelengthSample = Sampler_GetLightSample(taskConfig,
			bootSize - 1 SAMPLER_PARAM);
	float w[SLG_SPECTRAL_BINS];
	const uint hero = Spectral_SampleWavelengths(wavelengthSample, w);
	for (uint i = 0; i < SLG_SPECTRAL_BINS; ++i)
		sampleResult->spectralW[i] = w[i];
	sampleResult->spectralHeroAlive = SLG_SW_DEFAULT | (hero << SLG_SW_HERO_SHIFT);
#endif

	const float time = mix(camera->base.shutterOpen, camera->base.shutterClose,
			Sampler_GetLightSample(taskConfig, 8 SAMPLER_PARAM));

	// Pick the light source over the emission distribution
	float pickPdf;
	const uint emitLightIndex = Distribution1D_SampleDiscrete(emitLightsDistribution,
			Sampler_GetLightSample(taskConfig, 0 SAMPLER_PARAM), &pickPdf);

	float3 flux = BLACK;
	if ((emitLightIndex != NULL_INDEX) && (pickPdf > 0.f)) {
		__global const LightSource* restrict light = &lights[emitLightIndex];

		float emissionPdfW, directPdfA, cosThetaAtLight;
#if defined(SLG_SPECTRAL)
		// Emission textures evaluate at the path wavelengths
		task->tmpHitPoint.spectralW[0] = sampleResult->spectralW[0];
		task->tmpHitPoint.spectralW[1] = sampleResult->spectralW[1];
		task->tmpHitPoint.spectralW[2] = sampleResult->spectralW[2];
		task->tmpHitPoint.spectralHeroAlive = sampleResult->spectralHeroAlive;
#endif
		flux = Light_Emit(light, time,
				Sampler_GetLightSample(taskConfig, 1 SAMPLER_PARAM),
				Sampler_GetLightSample(taskConfig, 2 SAMPLER_PARAM),
				Sampler_GetLightSample(taskConfig, 3 SAMPLER_PARAM),
				Sampler_GetLightSample(taskConfig, 4 SAMPLER_PARAM),
				Sampler_GetLightSample(taskConfig, 5 SAMPLER_PARAM),
				worldCenterX, worldCenterY, worldCenterZ, worldRadius,
				&task->tmpHitPoint, &rays[gid], &emissionPdfW,
				&directPdfA, &cosThetaAtLight
				LIGHTS_PARAM);

		// Caustic focus cache (doc/features/gpu_lighttracing.md): with
		// probability focusRatio, re-aim the emitted direction at a
		// hotspot remembered from previous successful delta-crossed
		// paths. Positional emitters only (point/spot).
		//
		// The emitted direction is always weighted by the one-sample
		// mixture pdf
		//   pdf = (1 - g) * nativePdf(dir) + g * aimPdf(dir)
		// for BOTH branches: the aim strategy is "uniform slot pick +
		// cone sample" (degenerate slots - a hotspot closer than the
		// aim radius to the origin - fall back to a native-density draw,
		// contributing (fD/fN)*nativePdf to aimPdf). Weighting native
		// draws by the same mixture keeps the estimator unbiased; using
		// the bare native pdf would over-weight directions outside the
		// aim cones by 1/(1-g).
		const uint focusN = (pathTracer->lightTracing.focusEnable && lightFocusCount) ?
				min(lightFocusCount[emitLightIndex], (uint)LIGHT_FOCUS_K) : 0;
		const bool isPositional = (light->type == TYPE_POINT) ||
				(light->type == TYPE_SPOT);

		// Area emitters (manifold-guided emission): the natively-sampled
		// surface point is kept and only the outgoing direction is
		// re-aimed into the hotspot cone. Only the open cosine hemisphere
		// is guided - a restricted or forward emission cone cannot be
		// steered without leaving its support, so those fall back to
		// native sampling. The joint position*direction pdf factors as
		// invTriangleArea * dirPdf for both branches, so the position
		// density is unchanged and only the direction density mixes.
		Frame emitFrame;
		float3 emittedRad = BLACK;
		bool isTri = false;
		if ((focusN > 0) && !isPositional && (light->type == TYPE_TRIANGLE)) {
			const uint triMatIndex = sceneObjs[light->triangle.meshIndex].materialIndex;
			// An emission map (IES, area light spread, sized spot cone)
			// is not a cosine hemisphere: re-aiming it would emit outside
			// its profile (058's 70-degree niche lights lit the walls)
			// A two-sided emitter's back-face lobe has no re-aimed twin
			if ((Material_GetEmittedCosThetaMax(triMatIndex MATERIALS_PARAM) <= 0.f) &&
					(mats[triMatIndex].emissionFuncDistOffset == NULL_INDEX) &&
					!mats[triMatIndex].emissionTwoSided) {
				isTri = true;
				HitPoint_GetFrame(&task->tmpHitPoint, &emitFrame);
				// Recover the direction-independent emitted radiance:
				// flux = emittedRad * |localDir.z| for every model
				const float origLocalZ = Frame_ToLocal_Private(&emitFrame,
						normalize(VLOAD3F(&rays[gid].d.x))).z;
				emittedRad = (origLocalZ > 0.f) ? (flux / origLocalZ) : BLACK;
			}
		}
		const bool focusActive = (focusN > 0) && (isPositional || isTri);
		if (focusActive) {
			const float3 rayOrig = VLOAD3F(&rays[gid].o.x);
			const float g = pathTracer->lightTracing.focusRatio;

			// Optionally re-aim the direction at a remembered hotspot.
			// Each ring entry carries its own aim radius in .w (tight for
			// specular portals, broad for diffuse receivers).
			if (Rnd_FloatValue(seed) < g) {
				const uint slot = min((uint)(Rnd_FloatValue(seed) * focusN), focusN - 1);
				const float4 hp = lightFocus[emitLightIndex * LIGHT_FOCUS_K + slot];
				const float3 toTarget = MAKE_FLOAT3(hp.x, hp.y, hp.z) - rayOrig;
				const float targetDist = length(toTarget);
				if (targetDist > hp.w) {
					const float sinMax = hp.w / targetDist;
					const float cosMax = sqrt(fmax(0.f, 1.f - sinMax * sinMax));
					const float3 axis = toTarget / targetDist;
					float3 axX, axY;
					CoordinateSystem(axis, &axX, &axY);
					const float3 newDir = UniformSampleCone(Rnd_FloatValue(seed),
							Rnd_FloatValue(seed), cosMax, axX, axY, axis);
					Ray_Init2(&rays[gid], rayOrig, newDir, time);

					// Emission flux of the redirected ray
					// (SpotLight_LocalFalloff returns 0 outside the cone
					// -> a leaked rim sample just dies)
					if (light->type == TYPE_POINT)
						flux = VLOAD3F(light->notIntersectable.point.emittedFactor.c) *
								(1.f / (4.f * M_PI_F));
					else if (light->type == TYPE_SPOT) {
						const float3 localDir = normalize(Transform_InvApplyVector(
								&light->notIntersectable.light2World, newDir));
						flux = VLOAD3F(light->notIntersectable.spot.emittedFactor.c) *
								(SpotLight_LocalFalloff(localDir,
								light->notIntersectable.spot.cosTotalWidth,
								light->notIntersectable.spot.cosFalloffStart,
								light->notIntersectable.spot.falloffMode) /
								(light->notIntersectable.spot.cosineCompensation ?
									fabs(CosTheta(localDir)) : 1.f));
					} else {
						// Cosine-hemisphere area emitter: emittedRad *
						// cos(theta) off the surface normal (0 below the
						// horizon - an aim into the far side just dies)
						const float localZ = Frame_ToLocal_Private(&emitFrame,
								newDir).z;
						flux = emittedRad * fmax(localZ, 0.f);
					}
				}
			}

			// Mixture pdf of the final direction over both strategies
			const float3 emitDir = normalize(VLOAD3F(&rays[gid].d.x));
			float nativePdf;
			if (light->type == TYPE_POINT)
				nativePdf = 1.f / (4.f * M_PI_F);
			else if (light->type == TYPE_SPOT)
				nativePdf = (CosTheta(normalize(Transform_InvApplyVector(
						&light->notIntersectable.light2World, emitDir))) >=
						light->notIntersectable.spot.cosTotalWidth) ?
						UniformConePdf(light->notIntersectable.spot.cosTotalWidth) : 0.f;
			else
				// cosine hemisphere around the surface normal
				nativePdf = fmax(Frame_ToLocal_Private(&emitFrame, emitDir).z,
						0.f) * (1.f / M_PI_F);

			float aimPdf = 0.f;
			for (uint k = 0; k < focusN; ++k) {
				const float4 hk = lightFocus[emitLightIndex * LIGHT_FOCUS_K + k];
				const float3 tk = MAKE_FLOAT3(hk.x, hk.y, hk.z) - rayOrig;
				const float dk = length(tk);
				if (dk > hk.w) {
					const float sk = hk.w / dk;
					const float ck = sqrt(fmax(0.f, 1.f - sk * sk));
					if (dot(emitDir, tk / dk) >= ck)
						aimPdf += UniformConePdf(ck);
				} else
					aimPdf += nativePdf; // degenerate slot: native fallback
			}
			aimPdf /= focusN;

			emissionPdfW = (1.f - g) * nativePdf + g * aimPdf;
			// Area emitters: fold the (unchanged) surface-position
			// density back into the joint emission pdf
			if (isTri)
				emissionPdfW *= light->triangle.invTriangleArea;
		}

		// Distant-light caustic focusing (CPU LightFocusEmitDistant
		// parity): distant directions aren't steerable, so with
		// probability g the emit ORIGIN is re-aimed at the projected
		// disc of a delta-specular caster. Casters are appended to
		// lightFocus after the per-light rings (float4: center.xyz +
		// bounding radius). The origin's one-sample mixture pdf is
		//   (1-g)*nativeArea + g*coverN/(pi*sumR2)
		// folded into emissionPdfW as a ratio over the native disc
		// density, keeping both branches unbiased.
		const uint focusCasterN = (pathTracer->lightTracing.focusEnable &&
				lightFocus) ? pathTracer->lightTracing.focusCasterCount : 0;
		if ((focusCasterN > 0) &&
				((light->type == TYPE_DISTANT) || (light->type == TYPE_SHARPDISTANT))) {
			const uint ltCount = as_uint(emitLightsDistribution[0]);
			__global const float4* restrict casters =
					lightFocus + ltCount * LIGHT_FOCUS_K;

			float sumR2 = 0.f;
			for (uint k = 0; k < focusCasterN; ++k)
				sumR2 += casters[k].w * casters[k].w;

			if (sumR2 > 0.f) {
				const float g = pathTracer->lightTracing.focusRatio;
				// Same radius the emit used: Light_Emit now derives
				// envRadius = GetEnvRadius(worldRadius) internally.
				const float envRadius = EnvLightSource_GetEnvRadius(worldRadius);
				const float3 wc = MAKE_FLOAT3(worldCenterX, worldCenterY, worldCenterZ);
				float3 aDir, axX, axY;
				if (light->type == TYPE_SHARPDISTANT) {
					aDir = VLOAD3F(&light->notIntersectable.sharpDistant.absoluteLightDir.x);
					axX = VLOAD3F(&light->notIntersectable.sharpDistant.x.x);
					axY = VLOAD3F(&light->notIntersectable.sharpDistant.y.x);
				} else {
					aDir = VLOAD3F(&light->notIntersectable.distant.absoluteLightDir.x);
					axX = VLOAD3F(&light->notIntersectable.distant.x.x);
					axY = VLOAD3F(&light->notIntersectable.distant.y.x);
				}
				const float invR = 1.f / envRadius;

				if (Rnd_FloatValue(seed) < g) {
					// Pick a caster proportional to its disc area r^2,
					// then sample a point of its projected disc
					float t = Rnd_FloatValue(seed) * sumR2;
					uint i = 0;
					for (; i + 1 < focusCasterN; ++i) {
						const float w = casters[i].w * casters[i].w;
						if (t < w)
							break;
						t -= w;
					}
					float dd1, dd2;
					ConcentricSampleDisk(Rnd_FloatValue(seed),
							Rnd_FloatValue(seed), &dd1, &dd2);
					const float rho = casters[i].w * invR;
					const float3 oc = MAKE_FLOAT3(casters[i].x, casters[i].y,
							casters[i].z) - wc;
					// Emit-plane disc coords map to the perpendicular
					// offset with a negative sign:
					// o = wc - R*(dir + d1*x + d2*y)
					const float s1 = -dot(oc, axX) * invR + rho * dd1;
					const float s2 = -dot(oc, axY) * invR + rho * dd2;
					Ray_Init2(&rays[gid],
							wc - envRadius * (aDir + s1 * axX + s2 * axY),
							normalize(VLOAD3F(&rays[gid].d.x)), time);
				}

				// Mixture pdf ratio on the final origin's disc coords
				const float3 oo = VLOAD3F(&rays[gid].o.x) - wc;
				const float s1 = -dot(oo, axX) * invR;
				const float s2 = -dot(oo, axY) * invR;
				uint coverN = 0;
				for (uint k = 0; k < focusCasterN; ++k) {
					const float3 oc = MAKE_FLOAT3(casters[k].x, casters[k].y,
							casters[k].z) - wc;
					const float c1 = -dot(oc, axX) * invR;
					const float c2 = -dot(oc, axY) * invR;
					const float rho = casters[k].w * invR;
					const float e1 = s1 - c1, e2 = s2 - c2;
					if (e1 * e1 + e2 * e2 <= rho * rho)
						++coverN;
				}
				const float nativeArea = (s1 * s1 + s2 * s2 <= 1.f) ? 1.f : 0.f;
				emissionPdfW *= (1.f - g) * nativeArea +
						g * coverN * envRadius * envRadius / sumR2;
			}
		}

#if defined(SLG_SPECTRAL)
		// Only TYPE_TRIANGLE evaluates its emission texture at the path
		// wavelengths (Material_GetEmittedRadiance); the other emitters
		// return baked RGB, upsampled into the bins here (same funnel as
		// DirectHitInfiniteLight)
		if ((light->type != TYPE_TRIANGLE) && !Spectrum_IsBlack(flux))
			flux = Spectral_Upsample(flux, sampleResult->spectralW,
					sampleResult->spectralHeroAlive, true,
					spectralUpsamplingTable);
#endif

		if (!Spectrum_IsBlack(flux) && (emissionPdfW > 0.f)) {
			flux /= emissionPdfW * pickPdf;

			if (pathTracer->vertexConnect.enabled) {
				// Vertex connection (M6): light-prefix MIS bookkeeping,
				// mirroring BiDirCPURenderThread::TraceLightPath. Both
				// pdfs are scaled by pickPdf on the CPU - it cancels in
				// the dVCM ratio. emissionPdfW here is the (possibly
				// focus-mixed) density of the emitted ray.
				lpi->dVCM = VCMis(directPdfA / emissionPdfW);
				// If the light source is not intersectable, it can not
				// be sampled with BSDF
				if (Light_IsEnvOrIntersectable(light)) {
					const float usedCosLight = Light_IsEnvironmental(light) ?
							1.f : cosThetaAtLight;
					lpi->dVC = VCMis(usedCosLight / (emissionPdfW * pickPdf));
				} else
					lpi->dVC = 0.f;
				// Vertex merging (M7): dVM = dVC * misVcWeightFactor
				// (CPU TraceLightPath - 0 when merging is off)
				lpi->dVM = lpi->dVC *
						pathTracer->vertexConnect.misVcWeightFactor;
			}
		} else
			flux = BLACK;

		lpi->lightIndex = emitLightIndex;
		lpi->lightGroupID = light->lightID;
	}

	// Sample a point on the camera lens (same lens sample for all
	// connects of this path, like CPU dims 6-7)
	float3 lensPoint;
	const bool lensOk = Camera_SampleLens(camera, cameraBokehDistribution,
			time,
			Sampler_GetLightSample(taskConfig, 6 SAMPLER_PARAM),
			Sampler_GetLightSample(taskConfig, 7 SAMPLER_PARAM),
			&lensPoint);
	lpi->lensPointX = lensPoint.x;
	lpi->lensPointY = lensPoint.y;
	lpi->lensPointZ = lensPoint.z;

	if (Spectrum_IsBlack(flux) || !lensOk) {
		// The sample produced nothing: mask the path ray and retry with
		// the next sample on the next iteration
		rays[gid].flags = RAY_FLAGS_MASKED;
		taskState->state = MK_LIGHT_INIT;
	} else {
		VSTORE3F(flux, taskState->throughput.c);
		taskState->throughShadowTransparency = false;
		taskState->state = MK_LIGHT_VERTEX;
	}

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// MK_LIGHT_VERTEX: fused per-vertex consume.
//
//   1. Resolve the pending camera-connect splat (the visibility ray in
//      the lightVisRayBase tail slot was traced by the last pass).
//   2. Consume the traced path ray: Scene_Intersect -> connect to the
//      camera (queues the next visibility ray) -> BSDF continuation.
//
// From: MK_LIGHT_VERTEX
// To: MK_LIGHT_VERTEX (self loop) or MK_LIGHT_INIT (path done)
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_LIGHT_VERTEX(
		KERNEL_ARGS
		KERNEL_ARGS_LIGHT
		KERNEL_ARGS_PGIC_DEPOSIT
		/* SSP eye-side specular tails (path.ssp.enable): sspTails[lt]
		 * holds eye task lt's last recorded specular run - consumed by
		 * the LMNEE chain solver when a camera connect is blocked by a
		 * recorded surface. NULL when disabled. */ \
		, __global const SspTail* restrict sspTails
		) {
	WAVEFRONT_GUARD
	__global GPUTask *task = &tasks[gid];
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_LIGHT_VERTEX(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_LIGHT_VERTEX)
		return;

	//--------------------------------------------------------------------------
	// Start of variables setup
	//--------------------------------------------------------------------------

	Seed seedValue = task->seed;
	Seed *seed = &seedValue;

	__constant const PathTracer* restrict pathTracer = &taskConfig->pathTracer;
	const uint lightIndex = gid - pathTracer->lightTracing.eyeTaskCount;
	__global LightPathInfo *lpi = &lightPathInfos[lightIndex];
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	__constant const Scene* restrict scene = &taskConfig->scene;

	__global Ray *visRay = &rays[pathTracer->lightTracing.lightVisRayBase + lightIndex];
	__global RayHit *visRayHit = &rayHits[pathTracer->lightTracing.lightVisRayBase + lightIndex];

	// Initialize image maps page pointer table
	INIT_IMAGEMAPS_PAGES

	__global float *filmScreenRadianceGroup[FILM_MAX_RADIANCE_GROUP_COUNT];
	filmScreenRadianceGroup[0] = filmScreenRadianceGroup0;
	filmScreenRadianceGroup[1] = filmScreenRadianceGroup1;
	filmScreenRadianceGroup[2] = filmScreenRadianceGroup2;
	filmScreenRadianceGroup[3] = filmScreenRadianceGroup3;
	filmScreenRadianceGroup[4] = filmScreenRadianceGroup4;
	filmScreenRadianceGroup[5] = filmScreenRadianceGroup5;
	filmScreenRadianceGroup[6] = filmScreenRadianceGroup6;
	filmScreenRadianceGroup[7] = filmScreenRadianceGroup7;

	//--------------------------------------------------------------------------
	// End of variables setup
	//--------------------------------------------------------------------------

	//--------------------------------------------------------------------------
	// A camera connect blocked by a delta occluder is being solved by the
	// light-side manifold walk (LMNEE): one solver step per launch on the
	// visibility-ray slot while the light path stalls. On success
	// LMnee_SolveEnd queues the x1 -> lens segment into the same slot and
	// refills pendingSplat, so the regular Stage A machinery below
	// resolves it transparently.
	//--------------------------------------------------------------------------

	if (lpi->mneeActive) {
		__global const SspTail *sspTail = (sspTails &&
				(lightIndex < pathTracer->lightTracing.eyeTaskCount)) ?
				&sspTails[lightIndex] : NULL;
		LMnee_ProcessState(taskConfig, task, &tasksDirectLight[gid], &tasksMnee[gid],
				taskState, lpi, visRay, visRayHit, sampleResult,
				filmWidth, filmHeight,
				filmSubRegion0, filmSubRegion1,
				filmSubRegion2, filmSubRegion3,
				worldRadius, mneeSeeds,
				camera, sspTail
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
				, samplerSharedDataBuff
#endif
				MATERIALS_PARAM);
		task->seed = seedValue;
		return;
	}

	//--------------------------------------------------------------------------
	// Stage A: resolve the pending camera-connect visibility ray
	//--------------------------------------------------------------------------

	if (lpi->pendingSplat.valid) {
		float3 connectionThroughput;
		// The connection pass-through draw is the CPU sampleOffset + 1
		// dimension of the connecting vertex (connectDepth.depth holds
		// the vertex index; only transparentDepth is bumped by the
		// marching below)
		const float passThroughEvent = Sampler_GetLightSample(taskConfig,
				pathTracer->lightTracing.lightSampleBootSize +
				lpi->connectDepth.depth *
				pathTracer->lightTracing.lightSampleStepSize + 1
				SAMPLER_PARAM);

		int connThroughShadow = lpi->connectThroughShadow;
		// SHADOW_RAY: lets the connection ray pass through
		// transparency.shadow materials (e.g. a glass shell around the
		// hit vertex) exactly like an eye-path shadow ray; otherwise a
		// refractive enclosure makes all interior splats impossible
		const bool continueToTrace = Scene_Intersect(taskConfig,
				LIGHT_RAY | CAMERA_RAY | SHADOW_RAY,
				&lpi->connectDepth, lpi->lastBSDFEvent,
				&connThroughShadow,
				&lpi->connectVolInfo,
				&task->tmpHitPoint,
				passThroughEvent,
				visRay, visRayHit,
				&task->tmpBsdf,
				&connectionThroughput, VLOAD3F(taskState->throughput.c),
				sampleResult,
				false
				MATERIALS_PARAM
				);
		lpi->connectThroughShadow = connThroughShadow;

		if (continueToTrace) {
			// The visibility ray keeps marching next iteration.
			// Scene_Intersect resets connectionThroughput to WHITE on
			// every entry, so segment transmittance/pass-through weights
			// accumulate into the stored radiance instead - the record
			// is rebuilt from scratch if the connect later re-blocks
			// (fromMnee/LMNEE paths overwrite radiance), so stale
			// accumulation never leaks into a different estimate.
			lpi->pendingSplat.radianceR *= connectionThroughput.x;
			lpi->pendingSplat.radianceG *= connectionThroughput.y;
			lpi->pendingSplat.radianceB *= connectionThroughput.z;
			task->seed = seedValue;
			return;
		}

		lpi->pendingSplat.valid = false;
		visRay->flags = RAY_FLAGS_MASKED;
		if ((visRayHit->meshIndex == NULL_INDEX) && lpi->pendingSplat.isCaustic) {
			// Nothing blocked the connection: splat the radiance
			float3 radiance = MAKE_FLOAT3(lpi->pendingSplat.radianceR,
					lpi->pendingSplat.radianceG, lpi->pendingSplat.radianceB) *
					connectionThroughput;
#if defined(SLG_SPECTRAL)
			// The pending radiance and the connect throughput are both
			// spectral wavelength bins: multiply first, then project to
			// film RGB exactly like the eye-path splat
			// (SampleResult_ProjectSpectralToRGB).
			radiance = Spectral_ProjectToRGB(radiance,
					sampleResult->spectralW, sampleResult->spectralHeroAlive);
#endif
			Film_SplatLight(lpi->pendingSplat.filmX, lpi->pendingSplat.filmY,
					lpi->pendingSplat.lightGroupID, radiance,
					filmScreenRadianceGroup,
					filmWidth, filmHeight,
					filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3,
					lightFilterLUTs,
					filmCryptoObject, filmCryptoMaterial,
					lpi->pendingSplat.cryptoObjectID,
					lpi->pendingSplat.cryptoMaterialID);

			// Caustic focus cache: a path that crossed a delta surface
			// and connected to the camera is productive - append its
			// first delta vertex to the emitting light's hotspot ring
			// (once per path: the flag is cleared so a later second
			// delta bounce can still be credited)
			if (lpi->hasDeltaVertex && lightFocusCount &&
					pathTracer->lightTracing.focusEnable) {
				lpi->hasDeltaVertex = false;
				const uint ringBase = lpi->lightIndex * LIGHT_FOCUS_K;
				const uint cursor = atomic_inc(&lightFocusCount[lpi->lightIndex]);
				const float aimR = FocusAimRadius(&lightFocus[ringBase],
						min(cursor, (uint)LIGHT_FOCUS_K),
						lpi->firstDeltaPX, lpi->firstDeltaPY, lpi->firstDeltaPZ,
						pathTracer->lightTracing.focusRadiusFrac * worldRadius,
						worldRadius);
				lightFocus[ringBase + (cursor % LIGHT_FOCUS_K)] =
						MAKE_FLOAT4(lpi->firstDeltaPX, lpi->firstDeltaPY,
								lpi->firstDeltaPZ, aimR);
			}

			// Manifold-guided emission: a solved-manifold connect (LMNEE)
			// reaches the camera only through specular interfaces. Credit
			// its receiver x0 into the focus ring so emission is steered
			// onto receivers that are known to reach the camera through a
			// refractive/reflective occluder (the refracted-view hard case).
			if (lpi->pendingSplat.fromMnee && lightFocusCount &&
					pathTracer->lightTracing.focusEnable) {
				const uint ringBase = lpi->lightIndex * LIGHT_FOCUS_K;
				const uint cursor = atomic_inc(&lightFocusCount[lpi->lightIndex]);
				const float aimR = FocusAimRadius(&lightFocus[ringBase],
						min(cursor, (uint)LIGHT_FOCUS_K),
						lpi->pendingSplat.recvPX, lpi->pendingSplat.recvPY,
						lpi->pendingSplat.recvPZ,
						pathTracer->lightTracing.focusRadiusFrac * worldRadius,
						worldRadius);
				lightFocus[ringBase + (cursor % LIGHT_FOCUS_K)] =
						MAKE_FLOAT4(lpi->pendingSplat.recvPX,
								lpi->pendingSplat.recvPY,
								lpi->pendingSplat.recvPZ, aimR);
			}
		} else if (visRayHit->meshIndex != NULL_INDEX) {
			// The connection was blocked by a delta occluder (glass/
			// mirror). A solved-manifold endpoint segment re-blocked
			// (fromMnee) means the occluder has more interfaces than the
			// single-vertex solve models - a slab's exit face - so the
			// multi-vertex chain takes over. A fresh connect tries the
			// cheap single-vertex solve first, falling back to the chain
			// when it cannot start (LMNEE, doc/features/gpu_lighttracing.md).
			int lmRet = 0;
			// Disjoint from the eye paths (CPU ConnectToEye LMNEE gate):
			// lights an eye path can hit by BSDF sampling are already
			// counted through the refracting blocker. (The caustic
			// light-path condition is applied when the probe is queued:
			// lpi has advanced past the receiver by now.)
			// lighttracing.only (no eye tasks) owns every connection.
			const bool lmneeOwned =
					(pathTracer->lightTracing.eyeTaskCount == 0) ||
					!Light_IsEnvOrIntersectable(&lights[lpi->lightIndex]);
			if (!lmneeOwned)
				lmRet = 0;
			else if (lpi->pendingSplat.fromMnee == 1)
				lmRet = LMneeChain_Start(taskConfig, task,
						&tasksDirectLight[gid], &tasksMnee[gid], taskState, visRay, lpi
						MATERIALS_PARAM) ? 1 : 0;
			else if (!lpi->pendingSplat.fromMnee) {
				lmRet = LMnee_Start(taskConfig, task, &tasksDirectLight[gid], &tasksMnee[gid],
						taskState, visRayHit, visRay, lpi, mneeSeeds,
						worldRadius
						MATERIALS_PARAM);
				if (!lmRet && sspTails &&
						(lightIndex < pathTracer->lightTracing.eyeTaskCount))
					// SSP: an eye path recorded a specular run covering
					// the blocker - replay its anchors instead of
					// discovering the chain geometrically
					lmRet = LMneeChain_StartTail(taskConfig, task,
							&tasksDirectLight[gid], &tasksMnee[gid], taskState,
							visRay, lpi, &sspTails[lightIndex],
							task->tmpBsdf.hitPoint.objectID
							MATERIALS_PARAM) ? 1 : 0;
				if (!lmRet)
					lmRet = LMneeChain_Start(taskConfig, task,
							&tasksDirectLight[gid], &tasksMnee[gid], taskState, visRay, lpi
							MATERIALS_PARAM) ? 1 : 0;
			}
			if (lmRet) {
				lpi->mneeActive = true;
				task->seed = seedValue;
				return;
			}
		}
	}

	// A terminated path moves on to the next light sample once the
	// pending splat has been resolved
	if (lpi->pathDone) {
		taskState->state = MK_LIGHT_INIT;
		task->seed = seedValue;
		return;
	}

	//--------------------------------------------------------------------------
	// Stage B: consume the traced light-path ray
	//--------------------------------------------------------------------------

	const uint sampleOffset = pathTracer->lightTracing.lightSampleBootSize +
			lpi->depth.depth * pathTracer->lightTracing.lightSampleStepSize;

	float3 connectionThroughput;
	// CPU draws the path-ray pass-through event at sampleOffset + 0
	const float passThroughEvent = Sampler_GetLightSample(taskConfig,
			sampleOffset SAMPLER_PARAM);

	// Volume the traced segment flies through (pre-intersect), with
	// the same default-world-volume fallback as the CPU deposit path
	const uint flightVolIdx = (lpi->volume.currentVolumeIndex != NULL_INDEX) ?
			lpi->volume.currentVolumeIndex : scene->defaultVolumeIndex;

	int throughShadowTransparency = taskState->throughShadowTransparency;
	const bool continueToTrace = Scene_Intersect(taskConfig,
			LIGHT_RAY | INDIRECT_RAY,
			&lpi->depth, lpi->lastBSDFEvent,
			&throughShadowTransparency,
			&lpi->volume,
			&task->tmpHitPoint,
			passThroughEvent,
			&rays[gid], &rayHits[gid], &taskState->bsdf,
			&connectionThroughput, VLOAD3F(taskState->throughput.c),
			sampleResult,
			false
			MATERIALS_PARAM
			);
	taskState->throughShadowTransparency = throughShadowTransparency;

	if (continueToTrace) {
		// The path ray keeps marching next iteration
		task->seed = seedValue;
		return;
	}


	// The ray was fully resolved
	const bool hit = (rayHits[gid].meshIndex != NULL_INDEX);
	bool terminate = !hit;

	if (hit) {
		__global const BSDF *bsdf = &taskState->bsdf;

		// Direct light sampling takes care of paths through
		// shadow-transparent materials (unless the material overrides:
		// CPU GetPassThroughShadowTransparency().Black() ||
		// GetPassThroughShadowTransparencyOverride())
		const float3 shadowTransparency = BSDF_GetPassThroughShadowTransparency(bsdf
				MATERIALS_PARAM);
		const bool shOverride = BSDF_GetPassThroughShadowTransparencyOverride(bsdf
				MATERIALS_PARAM);
		if (!Spectrum_IsBlack(shadowTransparency) && !shOverride)
			terminate = true;
		else if (lpi->depth.depth == 0) {
			// Light linking: the first light-path vertex receives direct
			// emission - an unlinked receiver carries no energy (volume
			// hits accept all groups through their ~0 mask)
			__global const LightSource* restrict linkLight =
					&lights[lpi->lightIndex];
			terminate = (linkLight->linkMask != 0ull) &&
					((linkLight->linkMask & bsdf->hitPoint.linkAcceptMask) == 0ull);
		}

		if (!terminate) {
			// Something was hit

			//----------------------------------------------------------
			// PhotonGI caustic deposits (B1'): light paths double as
			// photon paths. Mirrors CPU TracePhotonsThread: the specular
			// prefix is lpi->isNearlyS + lpi->firstVertSeen evaluated
			// BEFORE this vertex folds in; beams get pre-segment flux,
			// point photons get post-segment flux.
			//----------------------------------------------------------

			if (pathTracer->pgic.depositEnabled && pgicDepositCounters &&
					lpi->isNearlyS && lpi->firstVertSeen) {
				const float3 lightPathFlux = VLOAD3F(taskState->throughput.c);
				if (isfinite(lightPathFlux.x) && isfinite(lightPathFlux.y) &&
						isfinite(lightPathFlux.z)) {
					const float3 segP0 = VLOAD3F(&rays[gid].o.x);
					const float3 segP1 = VLOAD3F(&bsdf->hitPoint.p.x);
					const float3 segDelta = segP1 - segP0;
					const float segLen = length(segDelta);

					// Beam deposit: homogeneous flight volume, chunked so
					// segment AABBs stay tight (CPU tracephotonsthread.cpp:196)
					// Direct flights only (CPU tracephotonsthread.cpp
					// flightFromMedium): lpi->lastFromVolume marks a flight
					// starting at a medium scattering vertex
					if (pathTracer->pgic.causticVolumeBeams && pgicDepositBeams &&
							!lpi->lastFromVolume &&
							(flightVolIdx != NULL_INDEX) &&
							(mats[flightVolIdx].type == HOMOGENEOUS_VOL) &&
							(segLen > DEFAULT_EPSILON_STATIC)) {
						const float3 segDir = segDelta / segLen;
						const float chunkLen = fmax(
								8.f * pathTracer->pgic.causticLookUpRadius,
								segLen / 16.f);
						const uint nChunks = (uint)ceil(segLen / chunkLen);
						const Ray rayBackup = rays[gid];
						for (uint i = 0; i < nChunks; ++i) {
							const float s0 = i * chunkLen;
							const float s1 = fmin((i + 1) * chunkLen, segLen);
							float3 f = lightPathFlux * ((s1 - s0) / segLen);
							if (s0 > 0.f) {
								rays[gid].o.x = segP0.x; rays[gid].o.y = segP0.y;
								rays[gid].o.z = segP0.z;
								rays[gid].d.x = segDir.x; rays[gid].d.y = segDir.y;
								rays[gid].d.z = segDir.z;
								rays[gid].mint = 0.f;
								rays[gid].maxt = s0;
								rays[gid].time = 0.f;
								f *= Volume_TransmittanceEstimate(
										&mats[flightVolIdx], &rays[gid], s0, .5f,
										&task->tmpHitPoint TEXTURES_PARAM);
							}
							const uint slot = atomic_inc(&pgicDepositCounters[1]);
							if (slot < pathTracer->pgic.depositBeamCapacity) {
								__global PhotonBeam *b = &pgicDepositBeams[slot];
								const float3 a = segP0 + s0 * segDir;
								b->p0.x = a.x; b->p0.y = a.y; b->p0.z = a.z;
								b->d.x = segDir.x; b->d.y = segDir.y; b->d.z = segDir.z;
								b->lightID = lpi->lightGroupID;
								b->alpha.c[0] = f.x; b->alpha.c[1] = f.y; b->alpha.c[2] = f.z;
								b->length = s1 - s0;
								b->volumeIndex = flightVolIdx;
							} else
								atomic_inc(&pgicDepositCounters[3]);
						}
						rays[gid] = rayBackup;
					}

					// Point deposit: receiver material + film frustum
					// (deposit mode only runs with frustum culling - no
					// device visibility map exists)
					// Medium vertices only receive the direct flight (CPU
					// TracePhotonsThread): eye paths query every medium vertex
					if ((lpi->depth.depth > 0) && pgicDepositPhotons &&
							!(bsdf->isVolume && lpi->lastFromVolume) &&
							PhotonGICache_IsPhotonGIEnabled(bsdf,
								pathTracer->pgic.glossinessUsageThreshold,
								pathTracer->pgic.causticVolumeBeams
								MATERIALS_PARAM)) {
						float filmX, filmY;
						if (Camera_ProjectWorldPointToFilm(camera, segP1,
								rays[gid].time, filmHeight, &filmX, &filmY)) {
							const float padX = .1f * filmWidth;
							const float padY = .1f * filmHeight;
							if ((filmX >= -padX) && (filmX < filmWidth + padX) &&
									(filmY >= -padY) && (filmY < filmHeight + padY)) {
								const uint slot = atomic_inc(&pgicDepositCounters[0]);
								if (slot < pathTracer->pgic.depositPhotonCapacity) {
									__global Photon *ph = &pgicDepositPhotons[slot];
									const float3 photonFlux = lightPathFlux *
											connectionThroughput;
									ph->p.x = segP1.x; ph->p.y = segP1.y; ph->p.z = segP1.z;
									ph->d.x = rays[gid].d.x; ph->d.y = rays[gid].d.y;
									ph->d.z = rays[gid].d.z;
									ph->lightID = lpi->lightGroupID;
									ph->alpha.c[0] = photonFlux.x;
									ph->alpha.c[1] = photonFlux.y;
									ph->alpha.c[2] = photonFlux.z;
									// Landing normal flipped against the
									// incoming ray (CPU: landingSurfaceNormal)
									const float3 gn = VLOAD3F(&bsdf->hitPoint.geometryN.x);
									const float flip = (dot(gn, -VLOAD3F(&rays[gid].d.x)) > 0.f) ?
											1.f : -1.f;
									ph->landingSurfaceNormal.x = flip * gn.x;
									ph->landingSurfaceNormal.y = flip * gn.y;
									ph->landingSurfaceNormal.z = flip * gn.z;
									ph->isVolume = bsdf->isVolume;
								} else
									atomic_inc(&pgicDepositCounters[2]);
							}
						}
					}
				}
			}

			VSTORE3F(connectionThroughput * VLOAD3F(taskState->throughput.c),
					taskState->throughput.c);

			if (pathTracer->vertexConnect.enabled) {
				// Vertex connection (M6) light-prefix MIS fold
				// (BiDirCPURenderThread::TraceLightPath:691-696):
				//   dVCM *= t^2 / |cos theta|^2
				//   dVC  *=       1 / |cos theta|^2
				// lpi->depth.depth counts the completed vertices, so the
				// vertex just hit has CPU depth = lpi->depth.depth + 1;
				// the t^2 factor is skipped for the first vertex of an
				// environmental light (its "position" is the scene
				// sphere, t is meaningless there).
				__global const LightSource* restrict emitLight =
						&lights[lpi->lightIndex];
				if ((lpi->depth.depth > 0) || !Light_IsEnvironmental(emitLight))
					lpi->dVCM *= VCMis(rayHits[gid].t * rayHits[gid].t);
				// Medium vertex: no cosine in the area measure
				const float factor = bsdf->isVolume ? 1.f : (1.f / VCMis(fabs(dot(
						VLOAD3F(&bsdf->hitPoint.shadeN.x),
						VLOAD3F(&rays[gid].d.x)))));
				lpi->dVCM *= factor;
				lpi->dVC *= factor;
				lpi->dVM *= factor;
			}

			// Caustic focus cache: remember the first delta-specular
			// vertex of this path - a successful camera connect credits
			// it into the emitting light's hotspot ring
			if (!lpi->hasDeltaVertex && BSDF_IsDelta(bsdf MATERIALS_PARAM)) {
				const float3 hp = VLOAD3F(&bsdf->hitPoint.p.x);
				lpi->firstDeltaPX = hp.x;
				lpi->firstDeltaPY = hp.y;
				lpi->firstDeltaPZ = hp.z;
				lpi->hasDeltaVertex = true;
			}

			const bool isDeltaBsdf = BSDF_IsDelta(bsdf MATERIALS_PARAM);

			//--------------------------------------------------------------
			// Vertex connection (M6): append the non-delta vertex to the
			// light vertex cache (CPU pushes it even for camera-invisible
			// objects - only ConnectToEye is skipped there)
			//--------------------------------------------------------------

			if (pathTracer->vertexConnect.enabled && !isDeltaBsdf) {
				const uint slot = lpi->vcVertexCount;
				const uint slotsPerTask = pathTracer->vertexConnect.slotsPerTask;
				if ((slot < slotsPerTask) && lightVertices) {
					__global VCLightVertex *v = &lightVertices[lightIndex * slotsPerTask + slot];
					// Kernel launches on the device queue are
					// serialized, so a paired MK_VC_CONNECT pass always
					// reads the record whole; seq only marks the slot
					// as written (0 = never, used by the reader skip)
					v->bsdf = *bsdf;
					const float3 tp = VLOAD3F(taskState->throughput.c);
					v->throughputR = tp.x;
					v->throughputG = tp.y;
					v->throughputB = tp.z;
					v->dVCM = lpi->dVCM;
					v->dVC = lpi->dVC;
					v->dVM = lpi->dVM;
					v->lightID = lpi->lightGroupID;
					// 1-based depth (PathVertexVM::depth convention)
					v->depth = lpi->depth.depth + 1;
					v->seq = 1u;
					lpi->vcVertexCount = slot + 1;
				}
			}

			//--------------------------------------------------------------
			// Connect the light path vertex to the camera
			//--------------------------------------------------------------

			const bool cameraInvisible = (bsdf->sceneObjectIndex != NULL_INDEX) &&
					sceneObjs[bsdf->sceneObjectIndex].cameraInvisible;
			// CPU ConnectToEye: skip camera-invisible objects and delta
			// BSDF vertices. Under PhotonGI deposit mode (B1') light
			// tasks may run with lightTracing.enabled = 0: they only
			// append photon records, so the whole connect block -
			// visibility ray, pendingSplat, LMNEE start, focus
			// credits - is skipped here (the Stage-A splat resolve and
			// mneeActive blocks above are unreachable: pendingSplat and
			// mneeActive can only be armed inside this block).
			if (pathTracer->lightTracing.enabled && !cameraInvisible && !isDeltaBsdf) {
				const float3 hitP = VLOAD3F(&bsdf->hitPoint.p.x);
				const float3 lensPoint = MAKE_FLOAT3(lpi->lensPointX,
						lpi->lensPointY, lpi->lensPointZ);
				const float time = rays[gid].time;

				float3 eyeDir;
				float eyeDistance;
				if (camera->type == ORTHOGRAPHIC) {
					// The lens point anchors the camera plane; the ray
					// direction is the camera axis (CPU ConnectToEye).
					// Camera forward is +Z in camera space.
					eyeDir = normalize(Transform_ApplyVector(
							&camera->base.cameraToWorld,
							MAKE_FLOAT3(0.f, 0.f, 1.f)));
					const float D = -dot(eyeDir, lensPoint);
					eyeDistance = fabs(dot(eyeDir, hitP) + D);
				} else {
					eyeDir = hitP - lensPoint;
					eyeDistance = length(eyeDir);
					if (eyeDistance > 0.f)
						eyeDir /= eyeDistance;
				}

				if (eyeDistance > 0.f) {
					// The projection API consumes an eye ray: build it in
					// the visibility-ray slot
					Ray_Init3(visRay,
							(camera->type == ORTHOGRAPHIC) ? hitP : lensPoint,
							eyeDir, eyeDistance, time);
					float filmX, filmY;
					if (LightPath_ProjectToFilm(camera, visRay, &filmX, &filmY,
							filmWidth, filmHeight,
							filmSubRegion0, filmSubRegion1,
							filmSubRegion2, filmSubRegion3
#if defined(RENDER_ENGINE_TILEPATHOCL) || defined(RENDER_ENGINE_RTPATHOCL)
							, samplerSharedDataBuff
#endif
							)) {
						BSDFEvent event;
						float directPdfW;
						const float3 bsdfEval = BSDF_Evaluate(bsdf,
								-VLOAD3F(&visRay->d.x), &event, &directPdfW
								MATERIALS_PARAM);

						// CPU sampleResult.isCaustic + Metropolis
						// addonlycaustics contract: only (nearly-)caustic
						// connections reach the screen channel, so the
						// visibility ray is not even queued otherwise.
						// In lighttracing.only mode (eyeTaskCount == 0)
						// there are no eye paths to own the non-caustic
						// contribution, so every connection is splatted
						// (LIGHTCPU-style output for validation).
						const bool evalBlack = Spectrum_IsBlack(bsdfEval);
						// A black straight-direction eval does not
						// exclude a specular-manifold connection: the
						// receiver may face away from the lens while the
						// refracted segment stays above its horizon
						// (e.g. a table top seen only through a glass
						// sphere). Queue the visibility ray as an LMNEE
						// probe so a delta occluder can start a solve.
						// Caustic light paths only (L S+ D): otherwise the
						// eye path's NEE at the receiver covers it
						const bool mneeProbe = evalBlack &&
								taskConfig->pathTracer.mnee.enabled &&
								((pathTracer->lightTracing.eyeTaskCount == 0) ||
								LightPathInfo_IsCausticPath(lpi, event,
										BSDF_GetGlossiness(bsdf MATERIALS_PARAM),
										pathTracer->hybridBackForward.glossinessThreshold));
						// Vertex connection (M6): in BDPT mode the camera
						// connect is the s=0 strategy - CPU ConnectToEye
						// splats every non-delta connection (the caustic
						// gate is a hybrid back-forward concept only)
						const bool vcEnabled = pathTracer->vertexConnect.enabled;
						if ((!evalBlack && (vcEnabled ||
								(pathTracer->lightTracing.eyeTaskCount == 0) ||
								(pathTracer->hybridBackForward.adaptiveCaustic ?
								LightPathInfo_IsAdaptiveCausticPath(lpi, event,
								pathTracer->hybridBackForward.terminalGlossiness,
								pathTracer->hybridBackForward.connectProb,
								&lights[lpi->lightIndex]) :
								LightPathInfo_IsCausticPath(lpi, event,
								BSDF_GetGlossiness(bsdf MATERIALS_PARAM),
								pathTracer->hybridBackForward.glossinessThreshold)))) ||
								mneeProbe) {

							float pdfW, fluxToRadianceFactor;
							Camera_GetPDF(camera, visRay, eyeDistance,
									&pdfW, &fluxToRadianceFactor);
							if (fluxToRadianceFactor > 0.f) {
								// Queue the reversed visibility ray: it
								// spans the vertex -> lens segment
								const float3 eyeRayD = VLOAD3F(&visRay->d.x);
								const float3 origin = BSDF_GetRayOrigin(bsdf, -eyeRayD);
								const float mint = eyeDistance - visRay->maxt;
								const float maxt = eyeDistance - visRay->mint;
								Ray_Init4(visRay, origin, -eyeRayD, mint, maxt, time);

								float misWeight = 1.f;
								if (vcEnabled) {
									// CPU ConnectToEye MIS
									// (misVmWeightFactor = 0 in BIDIR):
									// weightLight = MIS(cameraPdfA) *
									//   (dVCM + dVC * MIS(bsdfRevPdfW))
									float bsdfRevPdfW;
									BSDF_Pdf(bsdf, -eyeRayD, NULL,
											&bsdfRevPdfW MATERIALS_PARAM);
									const uint vDepth = lpi->depth.depth + 1;
									if (vDepth >= pathTracer->rrDepth)
										bsdfRevPdfW *= RussianRouletteProb(
												pathTracer->rrImportanceCap,
												bsdfEval);
									const float cosToCamera = bsdf->isVolume ? 1.f : dot(
											VLOAD3F(&bsdf->hitPoint.shadeN.x),
											-eyeRayD);
									const float cameraPdfA = PdfWtoA(pdfW,
											eyeDistance, cosToCamera);
									const float weightLight =
											VCMis(cameraPdfA) * (lpi->dVCM +
											lpi->dVC * VCMis(bsdfRevPdfW));
									misWeight = 1.f / (weightLight + 1.f);
									// Volumes do not carry the cosine in
									// Evaluate - fold it back
									fluxToRadianceFactor *= bsdf->isVolume ?
											fabs(cosToCamera) : 1.f;
								}

								lpi->pendingSplat.filmX = filmX;
								lpi->pendingSplat.filmY = filmY;
								const float3 radiance =
										VLOAD3F(taskState->throughput.c) *
										bsdfEval * (misWeight * fluxToRadianceFactor);
								lpi->pendingSplat.radianceR = radiance.x;
								lpi->pendingSplat.radianceG = radiance.y;
								lpi->pendingSplat.radianceB = radiance.z;
								lpi->pendingSplat.lightGroupID = lpi->lightGroupID;
								// A probe queued on a black eval must not
								// splat if the ray turns out unblocked
								lpi->pendingSplat.isCaustic = !evalBlack;
								lpi->pendingSplat.fromMnee = false;
								// The splat's visible surface is the
								// connected light-path vertex
								lpi->pendingSplat.cryptoObjectID = BSDF_GetCryptoObjectID(bsdf);
								lpi->pendingSplat.cryptoMaterialID = BSDF_GetCryptoMaterialID(bsdf
										MATERIALS_PARAM);
								lpi->pendingSplat.valid = true;

								lpi->connectVolInfo = lpi->volume;
								lpi->connectDepth = lpi->depth;
								lpi->connectThroughShadow = false;
							}
						}
					}
					if (!lpi->pendingSplat.valid)
						visRay->flags = RAY_FLAGS_MASKED;
				}
			}

			//--------------------------------------------------------------
			// Path continuation
			//--------------------------------------------------------------

			if (!terminate &&
					(lpi->depth.depth >= pathTracer->maxPathDepth.depth - 1))
				terminate = true;

			if (!terminate) {
				float3 sampledDir;
				float bsdfPdfW, cosSampledDir;
				BSDFEvent bsdfEvent;
				float3 bsdfSample = BSDF_Sample(bsdf,
						Sampler_GetLightSample(taskConfig, sampleOffset + 4 SAMPLER_PARAM),
						Sampler_GetLightSample(taskConfig, sampleOffset + 5 SAMPLER_PARAM),
						&sampledDir, &bsdfPdfW, &cosSampledDir, &bsdfEvent
						MATERIALS_PARAM);
#if defined(SLG_SPECTRAL)
				// A dispersive transmit may have collapsed the alive mask on
				// the hit point: carry it back so the next intersection
				// (which re-copies from the SampleResult) keeps it.
				sampleResult->spectralHeroAlive = bsdf->hitPoint.spectralHeroAlive;
#endif

				if (Spectrum_IsBlack(bsdfSample))
					terminate = true;
				else {
					LightPathInfo_AddVertex(lpi, bsdf, bsdfEvent,
							pathTracer->hybridBackForward.glossinessThreshold
							MATERIALS_PARAM);

					// Hybrid back-forward mode keeps tracing only (nearly)
					// specular light paths. The diffuse cut is only valid
					// while eye tasks exist to own the diffuse
					// contribution: in lighttracing.only mode
					// (eyeTaskCount == 0) the light path is the sole
					// estimator and must run full depth, matching CPU
					// LIGHTCPU.
					// Vertex connection (M6): BDPT needs the full-depth
					// light path - the diffuse cut is a hybrid
					// back-forward concept VC supersedes
					if (pathTracer->hybridBackForward.enabled &&
							!pathTracer->vertexConnect.enabled &&
							(pathTracer->lightTracing.eyeTaskCount > 0) &&
							(pathTracer->hybridBackForward.adaptiveCaustic ?
								!lpi->isAdaptiveS : !lpi->isNearlyS) &&
							(lpi->depth.diffuseDepth + lpi->depth.glossyDepth > 1))
						terminate = true;

					// PhotonGI deposit mode (B1'): once the specular
					// prefix breaks the path can never deposit again -
					// it produces nothing else (no splats, no
					// connects), so drop it immediately. Mirrors the
					// CPU photon tracer stopping at its first
					// non-specular vertex when only caustics remain.
					if (!pathTracer->lightTracing.enabled &&
							pathTracer->pgic.depositEnabled && !lpi->isNearlyS)
						terminate = true;
				}

				if (!terminate && LightPathInfo_UseRR(lpi, pathTracer->rrDepth)) {
					const float rrProb = RussianRouletteProb(
							pathTracer->rrImportanceCap, bsdfSample);
					if (rrProb < Sampler_GetLightSample(taskConfig,
							sampleOffset + 6 SAMPLER_PARAM))
						terminate = true;
					else
						bsdfSample /= rrProb;
				}

				if (!terminate) {
					VSTORE3F(VLOAD3F(taskState->throughput.c) * bsdfSample,
							taskState->throughput.c);

					if (pathTracer->vertexConnect.enabled) {
						// CPU Bounce() MIS update (misVm/misVc = 0):
						//   specular: dVCM = 0, dVC *= MIS(cosSampledDir)
						//   else:     dVC  = MIS(cos/pdfW) *
						//                (dVC * MIS(revPdfW) + dVCM)
						//             dVCM = MIS(1/pdfW)
						float bsdfRevPdfW;
						if (bsdfEvent & SPECULAR)
							bsdfRevPdfW = bsdfPdfW;
						else
							BSDF_Pdf(bsdf, sampledDir, NULL, &bsdfRevPdfW
									MATERIALS_PARAM);
						const float lMisVcW = pathTracer->vertexConnect.
								misVcWeightFactor;
						const float lMisVmW = pathTracer->vertexConnect.
								misVmWeightFactor;
						if (bsdfEvent & SPECULAR) {
							lpi->dVCM = 0.f;
							const float specFactor = bsdf->isVolume ? 1.f : VCMis(cosSampledDir);
							lpi->dVC *= specFactor;
							lpi->dVM *= specFactor;
						} else {
							const float w = VCMis((bsdf->isVolume ? 1.f : cosSampledDir) / bsdfPdfW);
							lpi->dVC = w * (lpi->dVC * VCMis(bsdfRevPdfW) +
									lpi->dVCM + lMisVmW);
							lpi->dVM = w * (lpi->dVM * VCMis(bsdfRevPdfW) +
									lpi->dVCM * lMisVcW + 1.f);
							lpi->dVCM = VCMis(1.f / bsdfPdfW);
						}
					}

					if (isnan(taskState->throughput.c[0]) ||
							isnan(taskState->throughput.c[1]) ||
							isnan(taskState->throughput.c[2]))
						terminate = true;
					else {
						// Emit the continuation ray
						Ray_Init2(&rays[gid], BSDF_GetRayOrigin(bsdf, sampledDir),
								sampledDir, rays[gid].time);
					}
				}
			}
		}
	}

	if (terminate) {
		lpi->pathDone = true;
		rays[gid].flags = RAY_FLAGS_MASKED;
		// The pending splat is resolved first on the next iteration
		if (!lpi->pendingSplat.valid)
			taskState->state = MK_LIGHT_INIT;
	}

	// Save the seed
	task->seed = seedValue;
}

//------------------------------------------------------------------------------
// Evaluation of the Path finite state machine.
//
// From: MK_VC_CONNECT
// To: MK_GENERATE_NEXT_VERTEX_RAY
//
// Vertex connection (M6, GPU BDPT): connects the current eye vertex to
// the stored non-delta vertices of the paired light task's subpath
// (lightVertices[lightTask * slotsPerTask + k], k < vcVertexCount).
// Each connect is weighted by the SmallVCM MIS terms carried on the
// vertex records (misVm/misVc = 0 -> pure BPT); the shadow ray is
// marched inline through Scene_Intersect exactly like MK_RT_DL.
//------------------------------------------------------------------------------

__kernel void AdvancePaths_MK_VC_CONNECT(
		KERNEL_ARGS
		// Vertex connection tail: the paired task's LightPathInfo (for
		// vcVertexCount) and the vertex cache itself
		, __global LightPathInfo *lightPathInfos
		, __global const VCLightVertex* restrict lightVertices
		// M7 efficiency map + vertex merging spatial hash
		// (no parens in comments here - the cl2msl arg parser
		// bracket-matches this list literally)
		, __global float *vcEffStats
		, __global uint *vcMergeHash
		, __global VCReplay *vcReplay
		) {
	WAVEFRONT_GUARD
	__global GPUTaskState *taskState = &tasksState[gid];
	PathState pathState = taskState->state;
#if defined(DEBUG_PRINTF_KERNEL_NAME)
	if (gid == 0)
		printf("Kernel: AdvancePaths_MK_VC_CONNECT(state = %d)\n", pathState);
	else
		return;
#endif
	if (pathState != MK_VC_CONNECT)
		return;

	INIT_IMAGEMAPS_PAGES

	__global GPUTask *task = &tasks[gid];
	__global EyePathInfo *pathInfo = &eyePathInfos[gid];
	__global SampleResult *sampleResult = &sampleResultsBuff[gid];
	__global BSDF *eyeBsdf = &taskState->bsdf;
	__constant const Scene* restrict scene = &taskConfig->scene;

	const uint vcSlotsPerTask = taskConfig->pathTracer.vertexConnect.slotsPerTask;
	const uint lightTaskCount = taskConfig->pathTracer.lightTracing.lightTaskCount;
	const bool vcLive = lightVertices && (vcSlotsPerTask > 0u) &&
			(lightTaskCount > 0u);
	// M7 probabilistic connection: the candidate pool is the union of
	// poolTasks light tasks' vertex caches. Task j of the pool pairs as
	// (gid + j*poolStride) % lightTaskCount - j=0 reproduces the M6
	// gid%lightTaskCount pairing. The flat cursor c indexes
	// [0, poolTasks*slotsPerTask) and decodes to (j, slot) below.
	const uint vcPoolTasks = vcLive ? min(
			taskConfig->pathTracer.vertexConnect.poolTasks,
			lightTaskCount) : 1u;
	const uint vcPoolStride = max(1u, lightTaskCount / vcPoolTasks);
	const uint vcPoolSize = vcLive ? (vcPoolTasks * vcSlotsPerTask) : 0u;
	// Expected connect budget per eye vertex (0 = connect every
	// candidate - the deterministic M6 walk)
	const uint vcConnects = taskConfig->pathTracer.vertexConnect.connects;
	// M7d temporal reuse: candidate index vcPoolSize replays the eye
	// task's stored vertex - the record persists across iterations, so
	// unlike the pool it is NOT re-drawn every pass. It still enters
	// the deterministic sweep with the same MIS weighting (q = 1),
	// which keeps the extra strategy sample unbiased.
	const bool vcHasReplay = vcReplay &&
			taskConfig->pathTracer.vertexConnect.reuse &&
			(vcReplay[gid].vertex.seq != 0u);
	const uint vcCandCount = vcPoolSize + (vcHasReplay ? 1u : 0u);

	// M7 efficiency-aware allocation: the sample's screen tile indexes
	// the CAS-accumulated map (tile lum / spent rays + global pair).
	// After warmup the per-vertex budget is scaled by the tile's
	// measured efficiency relative to the global average - the map is
	// a pure proposal-shaping device (q_i already carries the exact
	// Horvitz-Thompson correction), so any map state stays unbiased.
	const uint vcTilesX = (filmWidth + 15u) >> 4;
	const uint vcTilesY = (filmHeight + 15u) >> 4;
	const uint vcNTiles = vcTilesX * vcTilesY;
	const uint vcTile = min(sampleResult->pixelX >> 4, vcTilesX - 1u) +
			min(sampleResult->pixelY >> 4, vcTilesY - 1u) * vcTilesX;
	float vcK = (float)vcConnects;
	if (vcEffStats && (vcConnects > 0u)) {
		const float gL = vcEffStats[2u * vcNTiles];
		const float gR = vcEffStats[2u * vcNTiles + 1u];
		// Warmup gate: wait until the map averages a few rays per tile
		if (gR > (float)vcNTiles * 4.f) {
			const float tL = vcEffStats[vcTile];
			const float tR = vcEffStats[vcNTiles + vcTile];
			const float gEff = gL / gR;
			const float tEff = (tR > 0.f) ? (tL / tR) : gEff;
			vcK = (float)vcConnects * clamp(tEff / gEff, .125f, 4.f);
		}
	}

	const float3 eyeP = VLOAD3F(&eyeBsdf->hitPoint.p.x);
	const float3 eyeShadeN = VLOAD3F(&eyeBsdf->hitPoint.shadeN.x);
	const float3 eyeThroughput = VLOAD3F(taskState->throughput.c);

	// Multi-subpath normalization: the pool holds vcPoolTasks INDEPENDENT
	// light subpaths, so candidates at the same light depth are multiple
	// draws of the SAME connect strategy. Their contributions must average
	// (divide by the subpath count), not sum - otherwise every strategy is
	// inflated by the pool size. Subpath averaging is exact even when some
	// subpaths are shorter than slotsPerTask (missing slots are draws that
	// contribute 0, still counted in vcPoolTasks). The replay record is one
	// additional draw of its own depth bucket: candidates sharing the
	// replayed vertex's light depth divide by vcPoolTasks + 1.
	const uint vcReplayDepth = vcHasReplay ? vcReplay[gid].vertex.depth : 0u;

	//----------------------------------------------------------------------
	// Resolve the connect shadow ray queued by the previous pass
	//----------------------------------------------------------------------
	if (taskState->vcPending) {
		// Same per-segment pass-through draw as the DL shadow ray path
		Seed seedPassThroughEvent = taskState->seedPassThroughEvent;
		const float passThroughEvent = Rnd_FloatValue(&seedPassThroughEvent);
		taskState->seedPassThroughEvent = seedPassThroughEvent;
		int throughShadowTransparency = 0;
		float3 connectionThroughput;
		const bool continueToTrace = Scene_Intersect(taskConfig,
				LIGHT_RAY | INDIRECT_RAY | SHADOW_RAY,
				NULL, NONE,
				&throughShadowTransparency,
				&directLightVolInfos[gid],
				&task->tmpHitPoint,
				passThroughEvent,
				&rays[gid], &rayHits[gid], &task->tmpBsdf,
				&connectionThroughput, WHITE,
				sampleResult,
				true
				MATERIALS_PARAM);
		// Still marching through pass-through occluders: resolve the
		// next segment in the following trace pass
		if (continueToTrace)
			return;

		taskState->vcPending = 0u;
		++taskState->vcCursor;

		if (rayHits[gid].meshIndex == NULL_INDEX) {
			// The light path vertex is visible - accumulate the deferred
			// contribution (misWeight*geometryTerm*eyeEval*lightEval*
			// lightThroughput) folded with the segment's volume transport
			const float3 landed = connectionThroughput * MAKE_FLOAT3(
					taskState->vcPendingR, taskState->vcPendingG,
					taskState->vcPendingB);
			SampleResult_AddDirectLight(&taskConfig->film,
					sampleResult, taskState->vcPendingLightID,
					taskState->vcPendingEvent,
					eyeThroughput, landed, 1.f);
			// LPE terminal: light-vertex connect at this eye vertex
			LPE_AccumulateVertex(sampleResult, &eyePathInfos[gid],
					LPE_VertexEvent((BSDFEvent)taskState->vcPendingEvent,
						eyeBsdf->isVolume),
					LPE_SYM_L, eyeThroughput * landed LPE_PARAM);
			const float l = fabs(landed.x) + fabs(landed.y) +
					fabs(landed.z);
			// Efficiency map: landed connect luminance on this tile.
			// eyeThroughput is left out on purpose - the map tracks
			// the transport the connect step itself discovers, which
			// is the quantity the allocation should follow.
			if (vcEffStats) {
				AtomicAddFloat(&vcEffStats[vcTile], l);
				AtomicAddFloat(&vcEffStats[2u * vcNTiles], l);
			}
			// Temporal reuse (M7d): a pool candidate that out-scored
			// the stored vertex promotes its staged record into the
			// replay slot (the replay itself - vcPendingCand ==
			// vcPoolSize - can not re-select itself)
			if (vcReplay && taskConfig->pathTracer.vertexConnect.reuse &&
					(taskState->vcPendingCand < vcPoolSize) &&
					(l > vcReplay[gid].score)) {
				vcReplay[gid].vertex = vcReplay[gid].staging;
				vcReplay[gid].score = l;
			}
		}
	}

	//----------------------------------------------------------------------
	// Vertex merging (M7, Georgiev'12 VCM): at this eye vertex's first
	// visit (vcCursor == 0), gather every cached light vertex inside
	// mergeRadius via the spatial hash and evaluate the VM contribution
	// directly - no shadow ray (the merge is a density estimate, not a
	// connection). SmallVCM RangeQuery::Process + the vmNormalization
	// factor of the caller; MIS per tech. rep. (37)-(39).
	//----------------------------------------------------------------------
	const bool vcMerge = vcLive && vcMergeHash &&
			taskConfig->pathTracer.vertexConnect.mergeEnable;
	if (vcMerge && (taskState->vcCursor == 0u) &&
			!BSDF_IsDelta(eyeBsdf MATERIALS_PARAM)) {
		const float r = taskConfig->pathTracer.vertexConnect.mergeRadius;
		const float r2 = r * r;
		const float misVcW = taskConfig->pathTracer.vertexConnect.
				misVcWeightFactor;
		const float vmNorm = taskConfig->pathTracer.vertexConnect.vmNorm;
		const int cx0 = (int)floor((eyeP.x - r) / r);
		const int cx1 = (int)floor((eyeP.x + r) / r);
		const int cy0 = (int)floor((eyeP.y - r) / r);
		const int cy1 = (int)floor((eyeP.y + r) / r);
		const int cz0 = (int)floor((eyeP.z - r) / r);
		const int cz1 = (int)floor((eyeP.z + r) / r);
		for (int cx = cx0; cx <= cx1; ++cx)
		for (int cy = cy0; cy <= cy1; ++cy)
		for (int cz = cz0; cz <= cz1; ++cz) {
			const uint h = VCMergeCellHash(cx, cy, cz);
			const uint cnt = min(vcMergeHash[h], VC_MERGE_CAPACITY);
			for (uint s = 0u; s < cnt; ++s) {
				const uint vIdx = vcMergeHash[VC_MERGE_BUCKETS +
						h * VC_MERGE_CAPACITY + s];
				if (vIdx >= taskConfig->pathTracer.vertexConnect.
						vertexCount)
					continue;
				__global const VCLightVertex *lv = &lightVertices[vIdx];
				if (lv->seq == 0u)
					continue;
				const float3 p2p = VLOAD3F(&lv->bsdf.hitPoint.p.x) - eyeP;
				const float d2 = dot(p2p, p2p);
				if ((d2 <= 0.f) || (d2 > r2))
					continue;
				// The photon direction: the light vertex's incoming
				// direction is where the merged vertex receives light
				// from (SmallVCM WorldDirFix convention)
				const float3 lightDir = VLOAD3F(
						&lv->bsdf.hitPoint.fixedDir.x);
				BSDFEvent eyeEvent;
				float eyePdfW;
				const float3 eyeBsdfEval = BSDF_Evaluate(eyeBsdf,
						lightDir, &eyeEvent, &eyePdfW
						MATERIALS_PARAM);
				if (Spectrum_IsBlack(eyeBsdfEval) || (eyePdfW <= 0.f))
					continue;
				float eyeRevPdfW;
				BSDF_Pdf(eyeBsdf, lightDir, NULL, &eyeRevPdfW
						MATERIALS_PARAM);
				// Continuation-probability folds (SmallVCM
				// ContinuationProb): dirPdfW by the eye vertex's RR
				// prob, revPdfW by the light vertex's - the light-side
				// proxy is its stored throughput (SmallVCM uses a
				// reflectance-based prob we do not carry)
				if (pathInfo->depth.depth + 1u >=
						taskConfig->pathTracer.rrDepth)
					eyePdfW *= RussianRouletteProb(taskConfig->pathTracer.
							rrImportanceCap, eyeBsdfEval);
				if (lv->depth >= taskConfig->pathTracer.rrDepth)
					eyeRevPdfW *= RussianRouletteProb(taskConfig->pathTracer.
							rrImportanceCap, MAKE_FLOAT3(lv->throughputR,
							lv->throughputG, lv->throughputB));
				const float wLight = lv->dVCM * misVcW +
						lv->dVM * VCMis(eyePdfW);
				const float wEye = pathInfo->dVCM * misVcW +
						pathInfo->dVM * VCMis(eyeRevPdfW);
				const float misWeight = 1.f / (wLight + 1.f + wEye);
				const float3 contrib = (misWeight * vmNorm) *
						eyeBsdfEval * MAKE_FLOAT3(lv->throughputR,
						lv->throughputG, lv->throughputB);
				if (isnan(contrib.x) || isinf(contrib.x) ||
						isnan(contrib.y) || isinf(contrib.y) ||
						isnan(contrib.z) || isinf(contrib.z))
					continue;
				SampleResult_AddDirectLight(&taskConfig->film,
						sampleResult, lv->lightID, eyeEvent,
						eyeThroughput, contrib, 1.f);
				// LPE terminal: vertex-merge connect at this eye vertex
				LPE_AccumulateVertex(sampleResult, &eyePathInfos[gid],
						LPE_VertexEvent(eyeEvent, eyeBsdf->isVolume),
						LPE_SYM_L, eyeThroughput * contrib LPE_PARAM);
			}
		}
	}

	//----------------------------------------------------------------------
	// Queue the next connect (the CPU ConnectVertices loop body runs one
	// vertex per iteration - the rays[gid] slot resolves in the next
	// trace pass like the DL shadow ray)
	//----------------------------------------------------------------------
	bool queued = false;
	// CPU ConnectVertices gate: delta eye vertices can not connect
	if ((vcPoolSize > 0u) && !BSDF_IsDelta(eyeBsdf MATERIALS_PARAM)) {
		// First visit at this eye vertex (cursor still at 0): score the
		// whole candidate pool. The scores drive the probabilistic
		// inclusion q_i = clamp(connects*score_i/scoreSum, qFloor, 1);
		// an included candidate's contribution is weighted 1/q_i
		// (Horvitz-Thompson), so the estimator stays unbiased for any
		// positive q_i - the score is only a variance tool. The floor
		// keeps candidates whose proxy underestimates them reachable.
		if ((vcConnects > 0u) && (taskState->vcCursor == 0u)) {
			float scoreSum = 0.f;
			for (uint c = 0u; c < vcPoolSize; ++c) {
				const uint lt = (gid + (c / vcSlotsPerTask) *
						vcPoolStride) % lightTaskCount;
				const uint k = c - (c / vcSlotsPerTask) * vcSlotsPerTask;
				if (k >= min(lightPathInfos[lt].vcVertexCount,
						vcSlotsPerTask))
					continue;
				__global const VCLightVertex *lv =
						&lightVertices[lt * vcSlotsPerTask + k];
				if (lv->seq == 0u)
					continue;
				const float3 p2p = VLOAD3F(&lv->bsdf.hitPoint.p.x) - eyeP;
				const float d2 = dot(p2p, p2p);
				if (d2 <= 0.f)
					continue;
				const float3 dir = p2p * (1.f / sqrt(d2));
				// Cheap contribution proxy: light throughput x |cos|
				// geometry (|cos|, not clamped, so transmissive BSDFs
				// keep a nonzero proposal weight)
				scoreSum += (lv->throughputR + lv->throughputG +
						lv->throughputB) *
						fabs(dot(eyeShadeN, dir)) *
						fabs(dot(VLOAD3F(&lv->bsdf.hitPoint.shadeN.x),
								-dir)) / d2;
			}
			taskState->vcScoreSum = scoreSum;
		}
		const float vcQFloor = .5f / (float)vcPoolSize;
		const uint vcSalt = (gid * 747796405u) ^
				((pathInfo->depth.depth + 1u) * 2654435761u) ^
				(GuidingPass(taskConfig, gid, samplesBuff) * 83492791u);

		uint c = taskState->vcCursor;
		for (; c < vcCandCount; ++c) {
			__global const VCLightVertex *lv;
			if (c < vcPoolSize) {
				const uint j = c / vcSlotsPerTask;
				const uint k = c - j * vcSlotsPerTask;
				const uint lightTask = (gid + j * vcPoolStride) %
						lightTaskCount;
				if (k >= min(lightPathInfos[lightTask].vcVertexCount,
						vcSlotsPerTask))
					continue;
				lv = &lightVertices[lightTask * vcSlotsPerTask + k];
			} else
				// M7d replay candidate: the stored stale vertex record
				lv = &vcReplay[gid].vertex;

			// Skip never-written slots (clamped-region guard - kernel
			// launches are serialized so a written record is whole)
			if (lv->seq == 0u)
				continue;

			const float3 p2p = VLOAD3F(&lv->bsdf.hitPoint.p.x) - eyeP;
			const float p2pDistance2 = dot(p2p, p2p);
			if (p2pDistance2 <= 0.f)
				continue;
			const float p2pDistance = sqrt(p2pDistance2);
			const float3 p2pDir = p2p / p2pDistance;

			const float cosThetaAtCamera = dot(eyeShadeN, p2pDir);
			const float cosThetaAtLight =
					dot(VLOAD3F(&lv->bsdf.hitPoint.shadeN.x), -p2pDir);

			// Probabilistic connection: draw the inclusion test on the
			// cheap score before paying the BSDF evaluations
			float vcQ = 1.f;
			if ((vcConnects > 0u) && (c < vcPoolSize)) {
				const float scoreSum = taskState->vcScoreSum;
				const float score =
						(lv->throughputR + lv->throughputG +
						lv->throughputB) * fabs(cosThetaAtCamera) *
						fabs(cosThetaAtLight) / p2pDistance2;
				vcQ = (scoreSum > 0.f) ?
						clamp(vcK * score / scoreSum,
								vcQFloor, 1.f) : vcQFloor;
				const float u = GuidingHash(vcSalt ^
						(c * 2891336453u)) * (1.f / 4294967296.f);
				if (u >= vcQ)
					continue;
			}

			// Check eye vertex BSDF
			BSDFEvent eyeEvent;
			float eyeBsdfPdfW;
			const float3 eyeBsdfEval = BSDF_Evaluate(eyeBsdf,
					p2pDir, &eyeEvent, &eyeBsdfPdfW
					MATERIALS_PARAM);
			if (Spectrum_IsBlack(eyeBsdfEval))
				continue;
			float eyeBsdfRevPdfW;
			BSDF_Pdf(eyeBsdf, p2pDir, NULL, &eyeBsdfRevPdfW
					MATERIALS_PARAM);

			// Check light vertex BSDF (evaluated on the stored copy)
			BSDFEvent lightEvent;
			float lightBsdfPdfW;
			const float3 lightBsdfEval = BSDF_Evaluate(&lv->bsdf,
					-p2pDir, &lightEvent, &lightBsdfPdfW
					MATERIALS_PARAM);
			if (Spectrum_IsBlack(lightBsdfEval))
				continue;
			float lightBsdfRevPdfW;
			BSDF_Pdf(&lv->bsdf, -p2pDir, NULL, &lightBsdfRevPdfW
					MATERIALS_PARAM);

			// The cosine terms are inside the Evaluate()s (CPU parity)
			const float geometryTerm = 1.f / p2pDistance2;

			//--------------------------------------------------------------
			// MIS weights (CPU ConnectVertices - misVm/misVc = 0)
			//--------------------------------------------------------------
			float eyePdfW = eyeBsdfPdfW;
			float eyeRevPdfW = eyeBsdfRevPdfW;
			float lightPdfW = lightBsdfPdfW;
			float lightRevPdfW = lightBsdfRevPdfW;
			// Eye depth is 0-based here, CPU eyeVertex.depth is +1
			if (pathInfo->depth.depth + 1u >= taskConfig->pathTracer.rrDepth) {
				const float prob = RussianRouletteProb(
						taskConfig->pathTracer.rrImportanceCap, eyeBsdfEval);
				eyePdfW *= prob;
				eyeRevPdfW *= prob;
			}
			// lv->depth is stored in the CPU 1-based convention
			if (lv->depth >= taskConfig->pathTracer.rrDepth) {
				const float prob = RussianRouletteProb(
						taskConfig->pathTracer.rrImportanceCap, lightBsdfEval);
				lightPdfW *= prob;
				lightRevPdfW *= prob;
			}

			// Medium vertices: no cosine in the area measure
			const float eyeBsdfPdfA = PdfWtoA(eyePdfW, p2pDistance,
					lv->bsdf.isVolume ? 1.f : cosThetaAtLight);
			const float lightBsdfPdfA = PdfWtoA(lightPdfW, p2pDistance,
					eyeBsdf->isVolume ? 1.f : cosThetaAtCamera);

			const float vcMisVmW = taskConfig->pathTracer.vertexConnect.
					misVmWeightFactor;
			const float lightWeight = VCMis(eyeBsdfPdfA) *
					(vcMisVmW + lv->dVCM + lv->dVC * VCMis(lightRevPdfW));
			const float eyeWeight = VCMis(lightBsdfPdfA) *
					(vcMisVmW + pathInfo->dVCM + pathInfo->dVC *
					VCMis(eyeRevPdfW));
			const float misWeight = 1.f / (lightWeight + 1.f + eyeWeight);

			//--------------------------------------------------------------
			// Queue the visibility ray and defer the contribution to the
			// resolve phase (CPU folds connectionThroughput in after the
			// unblocked test - the throughput is unknown until then)
			//--------------------------------------------------------------
			const float3 shadowRayOrig = BSDF_GetRayOrigin(eyeBsdf, p2pDir);
			const float3 shadowRayOrigP2P =
					VLOAD3F(&lv->bsdf.hitPoint.p.x) - shadowRayOrig;
			const float shadowRayDistance2 = dot(shadowRayOrigP2P, shadowRayOrigP2P);
			if (shadowRayDistance2 <= 0.f)
				continue;
			const float shadowRayDistance = sqrt(shadowRayDistance2);
			const float3 shadowRayDir = shadowRayOrigP2P / shadowRayDistance;

			Ray_Init4(&rays[gid], shadowRayOrig, shadowRayDir,
					0.f, shadowRayDistance, rays[gid].time);

			// Volume state of the connect ray: a copy of the eye
			// prefix's, with the current volume set by the arrival side
			// of the light vertex (CPU parity)
			directLightVolInfos[gid] = pathInfo->volume;
			const bool connectionIntoObject =
					(dot(VLOAD3F(&lv->bsdf.hitPoint.geometryN.x),
					-shadowRayDir) < 0.f);
			directLightVolInfos[gid].currentVolumeIndex = connectionIntoObject ?
					lv->bsdf.hitPoint.interiorVolumeIndex :
					lv->bsdf.hitPoint.exteriorVolumeIndex;

			// 1/vcQ: Horvitz-Thompson correction for the probabilistic
			// inclusion (identity in the deterministic full walk).
			// 1/vcDiv: subpath-average over the pool (see above).
			const uint vcDiv = vcPoolTasks +
					((vcHasReplay && (lv->depth == vcReplayDepth)) ? 1u : 0u);
			const float3 pending = (misWeight * geometryTerm / (vcQ * vcDiv)) *
					eyeBsdfEval * lightBsdfEval *
					MAKE_FLOAT3(lv->throughputR, lv->throughputG,
							lv->throughputB);
			taskState->vcPendingR = pending.x;
			taskState->vcPendingG = pending.y;
			taskState->vcPendingB = pending.z;
			taskState->vcPendingLightID = lv->lightID;
			taskState->vcPendingEvent = eyeEvent;
			taskState->vcPending = 1u;
			taskState->vcCursor = c;
			taskState->vcPendingCand = c;
			// Stage the record for the replay reservoir: the paired
			// light task may overwrite its cache slot before this
			// shadow ray resolves on the next iteration
			if (vcReplay && taskConfig->pathTracer.vertexConnect.reuse &&
					(c < vcPoolSize))
				vcReplay[gid].staging = *lv;
			queued = true;
			// Efficiency map: one connect ray spent on this tile
			if (vcEffStats) {
				AtomicAddFloat(&vcEffStats[vcNTiles + vcTile], 1.f);
				AtomicAddFloat(&vcEffStats[2u * vcNTiles + 1u], 1.f);
			}
			break;
		}
		if (!queued)
			taskState->vcCursor = vcCandCount;
	}

	if (!queued) {
		// Done connecting: mask the free ray slot and move on - the last
		// vertex splats instead of bouncing (its DL stage was skipped)
		rays[gid].flags = RAY_FLAGS_MASKED;
		taskState->state = sampleResult->lastPathVertex ?
				MK_SPLAT_SAMPLE : MK_GENERATE_NEXT_VERTEX_RAY;
	}
}

//------------------------------------------------------------------------------
// Vertex merging hash rebuild (M7)
//
// Two tiny kernels enqueued once per iteration before MK_VC_CONNECT
// (kernel launches are serialized, so no internal synchronization is
// needed). Reset clears the per-bucket counters; Build re-inserts every
// current vertex record under the cell of its position. Stale index
// slots can not linger: each iteration rewrites the whole table.
//------------------------------------------------------------------------------

__kernel void AdvancePaths_VCResetMergeHash(
		__global uint *vcMergeHash
		) {
	const size_t gid = get_global_id(0);
	if (gid >= VC_MERGE_BUCKETS)
		return;
	vcMergeHash[gid] = 0u;
}

__kernel void AdvancePaths_VCBuildMergeHash(
		__constant const GPUTaskConfiguration* restrict taskConfig
		, __global const LightPathInfo* restrict lightPathInfos
		, __global const VCLightVertex* restrict lightVertices
		, __global uint *vcMergeHash
		) {
	const size_t gid = get_global_id(0);
	if (gid >= taskConfig->pathTracer.vertexConnect.vertexCount)
		return;
	// Only this pass's vertices belong in the grid: slots past the
	// task's current vcVertexCount still hold records written by a
	// previous pass (seq != 0) and must not merge
	const uint slotsPerTask = taskConfig->pathTracer.vertexConnect.
			slotsPerTask;
	const uint task = (uint)gid / slotsPerTask;
	if ((uint)gid - task * slotsPerTask >=
			lightPathInfos[task].vcVertexCount)
		return;
	__global const VCLightVertex *lv = &lightVertices[gid];
	if (lv->seq == 0u)
		return;
	const float r = taskConfig->pathTracer.vertexConnect.mergeRadius;
	const float3 p = VLOAD3F(&lv->bsdf.hitPoint.p.x);
	const uint h = VCMergeCellHash((int)floor(p.x / r),
			(int)floor(p.y / r), (int)floor(p.z / r));
	const uint slot = atomic_inc(&vcMergeHash[h]);
	if (slot < VC_MERGE_CAPACITY)
		vcMergeHash[VC_MERGE_BUCKETS + h * VC_MERGE_CAPACITY + slot] =
				(uint)gid;
}

//------------------------------------------------------------------------------
// Wavefront queue builder (B2/E3 M1+M2)
//
// Runs once per iteration when wavefront queues are enabled. Two
// device passes with a host prefix step in between:
//
//   1. AdvancePaths_BucketHistogram counts each live task into
//      taskQueueCount[state * SLG_SPECTRAL_BINS + lambda] and caches
//      its lambda bucket in taskLambda[gid]. The host reads the
//      counters back, exclusive-prefixes them per state into
//      taskQueueBase (lambda-contiguous segments inside each flat
//      per-state queue region) and uploads the result.
//   2. AdvancePaths_BuildQueues appends every live task to its
//      lambda segment via an atomic cursor on taskQueueBase, so the
//      flat per-state queue ends up grouped by hero wavelength.
//      Lanes of a state launch therefore share the same lambda bin
//      (spectral coherence) without any extra memory: the queue
//      stays NUM_STATES * taskCount.
//
// lambda is the hero-wavelength bin (SampleResult::spectralHeroAlive,
// bits [3..4]); non-spectral builds bucket everything into lambda 0,
// reproducing the M1 flat append order. Tasks in MK_DONE are terminal
// and not queued. BuildQueues also accounts the per-iteration traced
// ray on every task, matching the dense-mode semantics of
// AdvancePaths_MK_RT_NEXT_VERTEX (which skips the increment under
// wavefront).
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE uint Wavefront_TaskLambda(
		__global const SampleResult* restrict sampleResult) {
#if defined(SLG_SPECTRAL)
	const uint hero = (sampleResult->spectralHeroAlive & SLG_SW_HERO_MASK) >>
			SLG_SW_HERO_SHIFT;
	return min(hero, SLG_SPECTRAL_BINS - 1u);
#else
	return 0u;
#endif
}

__kernel void AdvancePaths_BucketHistogram(
		__global GPUTaskState *tasksState,
		__global SampleResult *sampleResultsBuff,
		__global uint *taskQueueCount,
		__global uint *taskLambda
		) {
	const size_t gid = get_global_id(0);

	const uint state = (uint)tasksState[gid].state;
	if (state == MK_DONE)
		return;

	const uint lambda = Wavefront_TaskLambda(&sampleResultsBuff[gid]);
	taskLambda[gid] = lambda;
	atomic_inc(&taskQueueCount[state * SLG_SPECTRAL_BINS + lambda]);
}

__kernel void AdvancePaths_BuildQueues(
		__global GPUTaskState *tasksState,
		__global SampleResult *sampleResultsBuff,
		__global uint *taskQueueBuf,
		__global uint *taskQueueBase,
		__global uint *taskLambda,
		const uint taskQueueStride
		) {
	const size_t gid = get_global_id(0);

	// This has to be done once per iteration for each task while the
	// RT pass is still dense (every task's ray is traced).
	sampleResultsBuff[gid].rayCount += 1;

	const uint state = (uint)tasksState[gid].state;
	if (state == MK_DONE)
		return;

	// taskQueueBase doubles as the append cursor: the uploaded segment
	// base advances on every append, ending at the segment end. The
	// lambda bin is recomputed inline (Wavefront_TaskLambda reads
	// spectralHeroAlive, which nothing mutates between the histogram
	// and this pass), so the flat (non-spectral) path can skip
	// AdvancePaths_BucketHistogram entirely and let the cursor alone
	// produce the per-state total.
	const uint slot = atomic_inc(
			&taskQueueBase[state * SLG_SPECTRAL_BINS +
					Wavefront_TaskLambda(&sampleResultsBuff[gid])]);
	taskQueueBuf[state * taskQueueStride + slot] = gid;
}

// M3a: device-side exclusive prefix + per-state totals, replacing the
// host readback / host prefix / host upload round trip (one blocking
// host<->device sync per iteration on OpenCL, a full queue drain on
// Metal/Vulkan). One work-item per state:
//  - writes each lambda segment's base into taskQueueBase (BuildQueues
//    then advances them as atomic append cursors),
//  - writes the state total into taskQueueTotals (the WAVEFRONT_GUARD
//    lane bound; also read back on a resync cadence for launch sizing),
//  - re-zeroes the histogram counters for the next iteration —
//    taskQueueCount is consumed only by this kernel, so the host
//    zeroing write disappears too.
__kernel void AdvancePaths_QueuePrefix(
		__global uint *taskQueueCount,
		__global uint *taskQueueBase,
		__global uint *taskQueueTotals,
		const uint queueStateCount,
		const uint flatQueues
		) {
	const uint s = get_global_id(0);
	if (s >= queueStateCount)
		return;

	if (flatQueues) {
		// Flat append mode (non-spectral): BuildQueues already ran and
		// each state's lambda-0 cursor ended at the exact queue length.
		// Publish it as the launch bound and reset the cursors for the
		// next iteration's appends - no histogram pass needed.
		taskQueueTotals[s] = taskQueueBase[s * SLG_SPECTRAL_BINS];
		for (uint l = 0; l < SLG_SPECTRAL_BINS; ++l)
			taskQueueBase[s * SLG_SPECTRAL_BINS + l] = 0;
	} else {
		uint base = 0;
		for (uint l = 0; l < SLG_SPECTRAL_BINS; ++l) {
			taskQueueBase[s * SLG_SPECTRAL_BINS + l] = base;
			base += taskQueueCount[s * SLG_SPECTRAL_BINS + l];
			taskQueueCount[s * SLG_SPECTRAL_BINS + l] = 0;
		}
		taskQueueTotals[s] = base;
	}
}

// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
