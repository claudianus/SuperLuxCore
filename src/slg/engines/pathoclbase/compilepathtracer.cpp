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

#if !defined(LUXRAYS_DISABLE_OPENCL)

#include "slg/engines/pathoclbase/compiledscene.h"
#include "slg/lights/strategies/restirdi.h"

using namespace std;
using namespace luxrays;
using namespace slg;

void CompiledScene::CompilePathTracer() {
	compiledPathTracer.eyeSampleBootSize = pathTracer->eyeSampleBootSize;
	compiledPathTracer.eyeSampleStepSize = pathTracer->eyeSampleStepSize;
	compiledPathTracer.eyeSampleSize = pathTracer->eyeSampleSize;

	compiledPathTracer.maxPathDepth.depth = pathTracer->maxPathDepth.depth;
	compiledPathTracer.maxPathDepth.diffuseDepth = pathTracer->maxPathDepth.diffuseDepth;
	compiledPathTracer.maxPathDepth.glossyDepth = pathTracer->maxPathDepth.glossyDepth;
	compiledPathTracer.maxPathDepth.specularDepth = pathTracer->maxPathDepth.specularDepth;
	
	compiledPathTracer.rrDepth = pathTracer->rrDepth;
	compiledPathTracer.rrImportanceCap = pathTracer->rrImportanceCap;
	
	compiledPathTracer.sqrtVarianceClampMaxValue = pathTracer->sqrtVarianceClampMaxValue;

	compiledPathTracer.hybridBackForward.enabled = pathTracer->hybridBackForwardEnable;
	compiledPathTracer.hybridBackForward.glossinessThreshold = pathTracer->hybridBackForwardGlossinessThreshold;
	compiledPathTracer.hybridBackForward.adaptiveCaustic = pathTracer->hybridBackForwardAdaptiveCaustic;
	compiledPathTracer.hybridBackForward.terminalGlossiness = pathTracer->hybridBackForwardTerminalGlossiness;
	compiledPathTracer.hybridBackForward.connectProb = pathTracer->hybridBackForwardConnectProb;

	// GPU light tracing (doc/features/gpu_lighttracing.md): a tail task
	// population traces light sub-paths and splats their vertices into
	// RADIANCE_PER_SCREEN_NORMALIZED via camera projection. eyeTaskCount,
	// lightTaskCount and lightVisRayBase are filled at device init once
	// the task count is known.
	compiledPathTracer.lightTracing.enabled = pathTracer->lightTracingEnable;
	compiledPathTracer.lightTracing.eyeTaskCount = 0;
	compiledPathTracer.lightTracing.lightTaskCount = 0;
	compiledPathTracer.lightTracing.lightVisRayBase = 0;
	compiledPathTracer.lightTracing.lightSampleBootSize = pathTracer->lightSampleBootSize;
	compiledPathTracer.lightTracing.lightSampleStepSize = pathTracer->lightSampleStepSize;
	compiledPathTracer.lightTracing.lightSampleSize = pathTracer->lightSampleSize;
	compiledPathTracer.lightTracing.focusEnable = pathTracer->lightFocusEnable;
	compiledPathTracer.lightTracing.focusRatio = pathTracer->lightFocusRatio;
	compiledPathTracer.lightTracing.focusRadiusFrac = pathTracer->lightFocusRadiusFrac;

	// Distant-light caustic focusing (CPU PathTracer::LightFocusEmitDistant
	// parity): bounding spheres of every delta-specular object steer the
	// distant light's emit origin onto their projected disc. Uploaded to
	// the device appended after the per-light hotspot rings.
	lightFocusCasters.clear();
	if (pathTracer->lightFocusEnable) {
		for (u_int i = 0; i < scene.GetObjects().GetSize(); ++i) {
			SceneObjectConstRef obj = scene.GetObjects().GetSceneObject(i);
			MaterialConstRef mat = obj.GetMaterial();
			if (!mat.IsDelta() || !(mat.GetEventTypes() & SPECULAR))
				continue;
			const BBox &bb = obj.GetExtMesh().GetBBox();
			const Point c = (bb.pMin + bb.pMax) * .5f;
			lightFocusCasters.push_back(c.x);
			lightFocusCasters.push_back(c.y);
			lightFocusCasters.push_back(c.z);
			lightFocusCasters.push_back((bb.pMax - c).Length());
		}
	}
	compiledPathTracer.lightTracing.focusCasterCount = lightFocusCasters.size() / 4;
	
	compiledPathTracer.forceBlackBackground = pathTracer->forceBlackBackground;

	// Hero-wavelength spectral transport: the kernel-side path wavelengths
	// live in SampleResult/HitPoint; the extra boot dimension comes from
	// IDX_WAVELENGTH (sampler_types.cl) under -D SLG_SPECTRAL.
	compiledPathTracer.spectralEnable = pathTracer->spectralEnable;

	// RIS product guiding (M4b): K-candidate resampling against the
	// f|cos|*Lhat product. The kernel draws the candidates in
	// MK_HIT_OBJECT (post-BSDF, pre-DL - same ordering as the CPU) and
	// folds zHatMis into the DL-side MIS density.
	compiledPathTracer.guidingRisK = Min(Max(pathTracer->guidingRisK, 0), 8);

	// Portal bounce proposal (M5): the rects are uploaded once per device
	// (they are static scene data); only the scalars ride taskConfig.
	compiledPathTracer.portalCount = (u_int)pathTracer->portals.size();
	compiledPathTracer.portalShare = pathTracer->portalShare;
	compiledPathTracer.portalSideGate = pathTracer->portalSideGate;
	compiledPathTracer.portalAdapt = pathTracer->portalAdapt ? 1u : 0u;

	// Vertex connection (M6): the vertex cache geometry (slotsPerTask,
	// vertexCount) is filled at device init once lightTaskCount is known.
	compiledPathTracer.vertexConnect.enabled = pathTracer->vertexConnectEnable;
	compiledPathTracer.vertexConnect.slotsPerTask = 0;
	compiledPathTracer.vertexConnect.vertexCount = 0;
	compiledPathTracer.vertexConnect.connects = pathTracer->vertexConnectBudget;
	compiledPathTracer.vertexConnect.poolTasks = pathTracer->vertexConnectPoolTasks;
	compiledPathTracer.vertexConnect.adaptive = pathTracer->vertexConnectAdaptive;
	// Vertex merging (M7): the absolute radius and the MIS constants
	// need lightTaskCount + the scene radius - filled at device init.
	compiledPathTracer.vertexConnect.mergeEnable =
			(pathTracer->vertexConnectMergeRadius > 0.f) ? 1 : 0;
	compiledPathTracer.vertexConnect.mergeRadius = pathTracer->vertexConnectMergeRadius;
	compiledPathTracer.vertexConnect.misVcWeightFactor = 0.f;
	compiledPathTracer.vertexConnect.misVmWeightFactor = 0.f;
	compiledPathTracer.vertexConnect.vmNorm = 0.f;
	// Temporal connect reuse (M7d): per-eye-task vertex replay slot
	compiledPathTracer.vertexConnect.reuse = pathTracer->vertexConnectReuse ? 1 : 0;

	// MNEE specular caustics (pathtracer_mnee.cpp): the kernel port runs the
	// same single vertex solver (path.mnee.enable / path.mnee.maxiterations).
	// The multi-specular chain port (MNEEMultiDirectSampling, MNEE_PHASE_MS_*
	// in pathoclbase_funcs.cl) is validated against the CPU on the closed
	// glass slab (PATHOCL MS mean within 0.9% of PATHCPU MS and 0.07% of
	// light tracing; the earlier value shortfall was a reproject mint
	// mismatch, fixed for CPU parity in MneeChain_WriteReprojectRay).
	// maxSpecular is forwarded so the chain solver runs.
	compiledPathTracer.mnee.enabled = pathTracer->mneeEnable;
	compiledPathTracer.mnee.maxIterations = pathTracer->mneeMaxIterations;
	compiledPathTracer.mnee.maxSpecular = pathTracer->mneeMaxSpecular;
	compiledPathTracer.mnee.seedCacheEnable = pathTracer->mneeSeedCacheEnable;
	if (pathTracer->mneeEnable && (pathTracer->mneeMaxSpecular > 1))
		SLG_LOG("WARNING: path.mnee.maxspecular = " << pathTracer->mneeMaxSpecular <<
				" (multi-specular MNEE chains) is supported by the GPU kernels "
				"(validated against PATHCPU on the closed glass slab, "
				"see dev-tools/sota_p1_mnee_ms_test.py).");

	// ReSTIR DI: enable the kernel-side RIS reservoir when the scene's
	// illuminate light strategy is the ReSTIR one. The proposal q is the
	// already compiled lightsDistribution (LogPower); the kernel uses it
	// as the proposal of the reservoir. The candidate count mirrors the
	// CPU adaptive heuristic (restirdi.cpp Preprocess()).
	const auto& illuminateStrategy = scene.GetLightSources().GetIlluminateLightStrategy();
	const auto restirStrategy =
		dynamic_cast<const LightStrategyRestirDI *>(&illuminateStrategy);
	if (restirStrategy) {
		compiledPathTracer.restir.enabled = true;
		compiledPathTracer.restir.candidateCount =
			restirStrategy->GetEffectiveCandidateCount();
		compiledPathTracer.restir.temporalEnable =
			restirStrategy->IsTemporalReuseEnabled();
		compiledPathTracer.restir.spatialEnable =
			restirStrategy->IsSpatialReuseEnabled();
		compiledPathTracer.restir.visibilityEnable =
			restirStrategy->IsVisibilityEnabled();
	} else {
		compiledPathTracer.restir.enabled = false;
		compiledPathTracer.restir.candidateCount = 0;
		compiledPathTracer.restir.temporalEnable = false;
		compiledPathTracer.restir.spatialEnable = false;
		compiledPathTracer.restir.visibilityEnable = false;
	}
	// Filled at device init once the film sub-region is known
	compiledPathTracer.restir.reservoirCount = 0;
	// Filled at device init once the task count is known (the
	// candidate region of rays[]/rayHits[] starts at taskCount)
	compiledPathTracer.restir.visCandCount = 0;
	compiledPathTracer.restir.visCandRayBase = 0;
	compiledPathTracer.restir.visCandDataOffset = 0;

	// ReSTIR GI (G1 GPU): mirrors the pathTracer->restirGI* settings
	// parsed from path.restir.gi.*. The buffer offsets and the effective
	// candidate count are filled at device init (they depend on the film
	// sub-region and the task count).
	compiledPathTracer.restirGI.enabled = pathTracer->restirGIEnable;
	compiledPathTracer.restirGI.candidateCount = pathTracer->restirGICandidates;
	compiledPathTracer.restirGI.temporalEnable = pathTracer->restirGITemporalEnable;
	compiledPathTracer.restirGI.spatialEnable = pathTracer->restirGISpatialEnable;
	compiledPathTracer.restirGI.reservoirCount = 0;
	compiledPathTracer.restirGI.giReservoirOffset = 0;
	compiledPathTracer.restirGI.giCandDataOffset = 0;
	compiledPathTracer.restirGI.giCandRayBase = 0;
	compiledPathTracer.restirGI.giCandCount = 0;

	CompilePhotonGI();

	compiledPathTracer.albedo.specularSetting = (slg::ocl::AlbedoSpecularSetting)pathTracer->albedoSpecularSetting;
	compiledPathTracer.albedo.specularGlossinessThreshold = pathTracer->albedoSpecularGlossinessThreshold;
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
