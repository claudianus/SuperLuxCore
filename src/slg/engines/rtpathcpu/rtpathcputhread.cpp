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

#include <cassert>
#include "luxrays/utils/thread.h"

#include "slg/slg.h"
#include "slg/samplers/rtpathcpusampler.h"
#include "slg/samplers/metropolis.h"
#include "slg/engines/rtpathcpu/rtpathcpu.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// RTPathCPURenderThread
//------------------------------------------------------------------------------

RTPathCPURenderThread::RTPathCPURenderThread(RTPathCPURenderEngine *engine, const u_int index,
			luxrays::IntersectionDeviceRef device) : 
	PathCPURenderThread(engine, index, device) {
}

RTPathCPURenderThread::~RTPathCPURenderThread() {
}

void RTPathCPURenderThread::StartRenderThread() {
	// Avoid to allocate the film thread because I'm going to use the global one

	CPURenderThread::StartRenderThread();
}

void RTPathCPURenderThread::RTRenderFunc(std::stop_token stop_token) {
#ifndef NDEBUG
	SLG_LOG("[RTPathCPURenderEngine::" << threadIndex << "] Rendering thread started");
#endif

	//--------------------------------------------------------------------------
	// Initialization
	//--------------------------------------------------------------------------

	// This is really used only by Windows for 64+ threads support
	SetThreadGroupAffinity(threadIndex);

	RTPathCPURenderEngine *engine = (RTPathCPURenderEngine *)renderEngine;
	const PathTracer &pathTracer = engine->pathTracer;
	// (engine->seedBase + 1) seed is used for sharedRndGen
	auto rndGen = std::make_unique<RandomGenerator>(engine->seedBase + 1 + threadIndex);
	// Setup the sampler
	auto sampler = engine->renderConfig.AllocSampler(
		rndGen, engine->GetFilm(), engine->GetSampleSplatter(),
		engine->samplerSharedData, Properties()
	);
	(static_cast<RTPathCPUSampler *>(sampler.get()))->SetRenderEngine(engine);
	sampler->RequestSamples(PIXEL_NORMALIZED_ONLY, pathTracer.eyeSampleSize);

	// Hybrid light tracing: the RT sampler keeps driving eye samples while
	// Metropolis light paths are interleaved at path.hybridbackforward.partition,
	// mirroring the PATHCPU hybrid loop (PathTracerThreadState) so CPU+LT
	// viewports keep the progressive RT look instead of swapping engines.
	// The sampler is recreated after every pause/edit because FilmPtr rebinding
	// (the resize mechanism) requires a fresh Sampler object.
	SamplerUPtr lightSampler;
	vector<SampleResult> lightSampleResults;
	auto allocLightSampler = [&]() {
		Properties props;
		props <<
			Property("sampler.type")("METROPOLIS") <<
			// Disable image plane meaning for samples 0 and 1
			Property("sampler.imagesamples.enable")(false) <<
			Property("sampler.metropolis.addonlycaustics")(true);
		auto ls = Sampler::FromProperties(props, rndGen,
				FilmPtr(&engine->GetFilm()), engine->lightSampleSplatter,
				engine->lightSamplerSharedData);
		ls->SetThreadIndex(threadIndex);
		ls->RequestSamples(SCREEN_NORMALIZED_ONLY, pathTracer.lightSampleSize);
		return ls;
	};
	if (pathTracer.hybridBackForwardEnable)
		lightSampler = allocLightSampler();
	// Same init as PathTracerThreadState: 0/0 would be NaN, so seed the
	// light count to start with an eye sample
	double eyeSampleCount = 0.0;
	double lightSampleCount = 1.0;

	//--------------------------------------------------------------------------
	// Trace paths
	//--------------------------------------------------------------------------

	vector<SampleResult> sampleResults(1);
	SampleResult &sampleResult = sampleResults[0];
	PathTracer::InitEyeSampleResults(engine->GetFilm(), sampleResults);

	VarianceClamping varianceClamping(pathTracer.sqrtVarianceClampMaxValue,
			pathTracer.varianceClampAdaptive, pathTracer.varianceClampScope,
			pathTracer.varianceClampSigma);

	// SSP: this thread's most recent eye-side specular tail (same
	// thread-local pairing as PathTracerThreadState::sspTail)
	SspTail sspTail;

	for (u_int steps = 0; !stop_token.stop_requested(); ++steps) {
		// Check if we are in pause or edit mode
		if (engine->threadsPauseMode) {
			// Synchronize all threads -> This waits for RTPathCPURenderEngine::PauseThreads()
			engine->threadsSyncBarrier->arrive_and_wait();

			// Wait for the main thread -> This waits for RTPathCPURenderEngine::ResumeThreads()
			engine->threadsSyncBarrier->arrive_and_wait();

			// If the engine was stopped, we break here.
			if (stop_token.stop_requested())
				break;

			(static_cast<RTPathCPUSampler *>(sampler.get()))->Reset(FilmPtr(&engine->GetFilm()));
			if (lightSampler)
				lightSampler = allocLightSampler();
		}

		// Same eye/light partition rule as PathTracer::HasToRenderEyeSample:
		// equilibrium keeps eye:light = partition. Light paths are held
		// back until this thread's coarse first frame is done - the
		// instant-coverage zoom phase is what keeps the viewport
		// responsive, and a missing light pass there is less visible
		// than a delayed first frame
		const double ratio = eyeSampleCount / lightSampleCount;
		const bool renderEye = !lightSampler ||
				!static_cast<RTPathCPUSampler *>(sampler.get())->IsFirstFrameDone() ||
				(pathTracer.hybridBackForwardPartition == 1.f) ||
				(ratio < pathTracer.hybridBackForwardPartition);
		if (renderEye) {
			eyeSampleCount += 1.0;

			pathTracer.RenderEyeSample(device, engine->renderConfig.GetScene(),
					engine->GetFilm(), *sampler, sampleResults,
					pathTracer.sspEnable ? &sspTail : nullptr);

			// Variance clamping
			if (varianceClamping.hasClamping())
				varianceClamping.Clamp(engine->GetFilm(), sampleResult);

			sampler->NextSample(sampleResults);
		} else {
			lightSampleCount += 1.0;

			pathTracer.RenderLightSample(device, engine->renderConfig.GetScene(),
					engine->GetFilm(), *lightSampler, lightSampleResults,
					PathTracer::ConnectToEyeCallBackType(),
					pathTracer.sspEnable ? &sspTail : nullptr);
			lightSampler->NextSample(lightSampleResults);
		}

#ifdef WIN32
		// Work around Windows bad scheduling
        std::this_thread::yield();
#endif
	}


	threadDone = true;

#ifndef NDEBUG
	SLG_LOG("[RTPathCPURenderEngine::" << threadIndex << "] Rendering thread halted");
#endif
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
