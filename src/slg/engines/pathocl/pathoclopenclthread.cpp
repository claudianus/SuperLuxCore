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

#include <mutex>
#include <boost/lexical_cast.hpp>

#include "luxrays/core/geometry/transform.h"
#include "luxrays/core/randomgen.h"
#include "luxrays/utils/ocl.h"
#include "luxrays/devices/ocldevice.h"
#include "luxrays/kernels/kernels.h"

#include "slg/slg.h"
#include "slg/kernels/kernels.h"
#include "slg/renderconfig.h"
#include "slg/engines/pathocl/pathocl.h"

using namespace std;
using namespace luxrays;
using namespace slg;
using namespace std::literals::chrono_literals;

//------------------------------------------------------------------------------
// PathOCLRenderThread
//------------------------------------------------------------------------------

PathOCLOpenCLRenderThread::PathOCLOpenCLRenderThread(const u_int index,
		HardwareIntersectionDeviceRef device, PathOCLRenderEngine *re) :
		PathOCLBaseOCLRenderThread(index, device, re) {
}

PathOCLOpenCLRenderThread::~PathOCLOpenCLRenderThread() {
}

void PathOCLOpenCLRenderThread::GetThreadFilmSize(u_int *filmWidth, u_int *filmHeight,
		u_int *filmSubRegion) {
	PathOCLRenderEngine *engine = (PathOCLRenderEngine *)renderEngine;
	FilmConstRef engineFilm = engine->GetFilm();

	*filmWidth = engineFilm.GetWidth();
	*filmHeight = engineFilm.GetHeight();

	const u_int *subRegion = engineFilm.GetSubRegion();
	filmSubRegion[0] = subRegion[0];
	filmSubRegion[1] = subRegion[1];
	filmSubRegion[2] = subRegion[2];
	filmSubRegion[3] = subRegion[3];
}

void PathOCLOpenCLRenderThread::StartRenderThread() {
	PathOCLRenderEngine *engine = (PathOCLRenderEngine *)renderEngine;

	// I have to load the start film otherwise it is overwritten at the first
	// merge of all thread films
	if (engine->hasStartFilm && (threadIndex == 0))
		threadFilms[0]->GetFilm().AddFilm(engine->GetFilm());

	PathOCLBaseOCLRenderThread::StartRenderThread();
}

static void PGICUpdateCallBack(CompiledScene *compiledScene) {
	compiledScene->RecompilePhotonGI();
}

void PathOCLOpenCLRenderThread::RenderThreadImpl(std::stop_token stop_token) {
	//SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Rendering thread started");

	PathOCLRenderEngine *engine = (PathOCLRenderEngine *)renderEngine;
	const u_int taskCount = engine->taskCount;

	intersectionDevice.PushThreadCurrentDevice();

	//----------------------------------------------------------------------
	// Execute initialization kernels
	//----------------------------------------------------------------------

	// Clear the frame buffer
	const u_int filmPixelCount = threadFilms[0]->GetFilm().GetWidth() * threadFilms[0]->GetFilm().GetHeight();
	intersectionDevice.EnqueueKernel(filmClearKernel,
		HardwareDeviceRange(RoundUp<u_int>(filmPixelCount, filmClearWorkGroupSize)),
		HardwareDeviceRange(filmClearWorkGroupSize));

	// Initialize random number generator seeds
	intersectionDevice.EnqueueKernel(initSeedKernel,
			HardwareDeviceRange(engine->taskCount), HardwareDeviceRange(initWorkGroupSize));

	// Initialize the tasks buffer
	intersectionDevice.EnqueueKernel(initKernel,
			HardwareDeviceRange(engine->taskCount), HardwareDeviceRange(initWorkGroupSize));

	// Check if I have to load the start film
	if (engine->hasStartFilm && (threadIndex == 0))
		threadFilms[0]->SendFilm(intersectionDevice);

	//----------------------------------------------------------------------
	// Rendering loop
	//----------------------------------------------------------------------

	// The film refresh time target
	const double targetTime = 0.2; // 200ms

	u_int iterations = 4;
	u_int totalIterations = 0;

	double totalTransferTime = 0.0;
	double totalKernelTime = 0.0;

	const std::function<void()> pgicUpdateCallBack = std::bind(PGICUpdateCallBack, engine->compiledScene);

	// Progressive vertex-merge radius (VCM): the merge pass counter is
	// the mean completed subpaths per light task - the same scale as
	// BIDIRVMCPU's per-iteration schedule since each CPU iteration also
	// traces lightPathsCount sub-paths. Only rewritten when it changes.
	u_int lastVCMergePass = 0;
	double lastPGICDrainLightCount = 0.0;
	// PhotonGI generation counter seen by this thread: a change means a
	// cache swap landed inside Update()'s barrier (per-thread refresh
	// of taskConfig + query buffers; thread 0 is not the only device)
	u_int lastPGICPass = 0;
	// Denoiser kernel-arg state pushed to the GPU last: warmUpDone,
	// sampleScale and radianceChannelScales all mutate together exactly
	// once - at the warm-up transition (filmdenoiser.cpp). Re-setting
	// ~15 kernels x the full arg list every batch just to catch that
	// one flip is wasted host work; refresh only on the flip. Init
	// matches the warmUpDone=false state SetKernelArgs() pushed.
	int lastDenoiserWarmUp = 0;

	while (!stop_token.stop_requested()) {
		//if (threadIndex == 0)
		//	SLG_LOG("[DEBUG] =================================");

		// Check if we are in pause mode
		if (engine->pauseMode) {
			// Check every 100ms if I have to continue the rendering
			while (!stop_token.stop_requested() && engine->pauseMode)
				std::this_thread::sleep_for(100ms);

			if (stop_token.stop_requested())
				break;
		}

		//------------------------------------------------------------------

		const double timeTransferStart = WallClockTime();

		// Transfer the film only if I have already spent enough time running
		// rendering kernels. This is very important when rendering very high
		// resolution images (for instance at 4961x3508)

		if (totalTransferTime < totalKernelTime * (1.0 / 100.0)) {
			// Async. transfer of the Film buffers
			threadFilms[0]->RecvFilm(intersectionDevice);

			// Async. transfer of GPU task statistics
			intersectionDevice.EnqueueReadBuffer(
				taskStatsBuff,
				CL_FALSE,
				sizeof(slg::ocl::pathoclbase::GPUTaskStats) * taskCount,
				gpuTaskStats.get());

			intersectionDevice.FinishQueue();

			// I need to update the film samples count

			// GPU light tracing (doc/features/gpu_lighttracing.md): light
			// tasks feed the screen-normalized channel, so their samples
			// count toward RADIANCE_PER_SCREEN_NORMALIZED, not the
			// per-pixel statistic
			const u_int eyeTaskCount = taskCount - engine->lightTaskCount;
			double eyeSampleCount = 0.0, lightSampleCount = 0.0;
			for (size_t i = 0; i < eyeTaskCount; ++i)
				eyeSampleCount += gpuTaskStats[i].sampleCount;
			for (size_t i = eyeTaskCount; i < taskCount; ++i)
				lightSampleCount += gpuTaskStats[i].sampleCount;
			threadFilms[0]->GetFilm().SetSampleCount(eyeSampleCount + lightSampleCount,
					eyeSampleCount, lightSampleCount);

			// PhotonGI GPU photon generation (B1'): drain the deposit
			// buffers while the queue is synchronized; the light-task
			// sample counter delta feeds the cache's traced-path
			// normalization (paths that produced these deposits).
			if (engine->pgicDepositWanted) {
				const u_int tracedDelta = (u_int)(lightSampleCount -
						lastPGICDrainLightCount);
				lastPGICDrainLightCount = lightSampleCount;
				DrainPGIC(tracedDelta);
			}

			// Progressive vertex-merge radius (VCM schedule): shrink the
			// merge kernel radius and re-derive the SmallVCM constants as
			// the light population accumulates sub-paths. Kernels read the
			// current values from taskConfig; the merge hash cell size
			// tracks mergeRadius so queries stay self-consistent.
			auto &vc = threadTaskConfig.pathTracer.vertexConnect;
			if (vc.enabled && vc.mergeEnable && (vc.mergeAlpha < 1.f) &&
					(engine->lightTaskCount > 0)) {
				const u_int mergePass = (u_int)(lightSampleCount / engine->lightTaskCount);
				if (mergePass != lastVCMergePass) {
					lastVCMergePass = mergePass;
					const float r = Max(vc.mergeStartRadius * 1e-4f,
							vc.mergeStartRadius /
							powf(float(mergePass + 1), .5f * (1.f - vc.mergeAlpha)));
					const float nVM = (float)engine->lightTaskCount;
					const float nVC = (float)Max(1u, vc.poolTasks);
					const float etaVCM = M_PI * r * r * nVM / nVC;
					vc.mergeRadius = r;
					vc.misVcWeightFactor = 1.f / (etaVCM * etaVCM);
					vc.misVmWeightFactor = etaVCM * etaVCM;
					vc.vmNorm = 1.f / (M_PI * r * r * nVM);
					intersectionDevice.EnqueueWriteBuffer(taskConfigBuff,
							CL_TRUE,
							sizeof(slg::ocl::pathoclbase::GPUTaskConfiguration),
							&threadTaskConfig);
				}
			}

			//SLG_LOG("[DEBUG] film transferred");
		}
		const double timeTransferEnd = WallClockTime();
		totalTransferTime += timeTransferEnd - timeTransferStart;

		//------------------------------------------------------------------

		// PSR halflife decay (path.regularization.halflife): kernels seed
		// each path's sigma from taskConfig at init, so the host recomputes
		// the effective sigma from global spp once per batch and re-uploads
		// - paths keep the sigma they were born with (Kaplanyan's scheme:
		// a decaying schedule, not a mid-path mutation)
		if (engine->pathTracer.regularizationHalflife > 0.f) {
			const double spp = engine->GetFilm().GetTotalEyeSampleCount() /
					engine->GetFilm().GetPixelCount();
			const float s = engine->pathTracer.EffectiveRegularizationSigma(spp);
			if (s != threadTaskConfig.pathTracer.regularizationSigma) {
				threadTaskConfig.pathTracer.regularizationSigma = s;
				intersectionDevice.EnqueueWriteBuffer(taskConfigBuff,
						CL_TRUE,
						sizeof(slg::ocl::pathoclbase::GPUTaskConfiguration),
						&threadTaskConfig);
			}
		}

		const double timeKernelStart = WallClockTime();

		// This is required for updating film denoiser parameter - but
		// only when the warm-up state actually flipped (see
		// lastDenoiserWarmUp above).
		if (threadFilms[0]->GetFilm().GetDenoiser().IsEnabled()) {
			const int wu = threadFilms[0]->GetFilm().GetDenoiser().IsWarmUpDone() ? 1 : 0;
			if (wu != lastDenoiserWarmUp) {
				std::unique_lock<std::mutex> lock(engine->setKernelArgsMutex);
				SetAllAdvancePathsKernelArgs(0);
				lastDenoiserWarmUp = wu;
			}
		}

		// Ray slots: taskCount per-task rays + lightTaskCount light
		// camera-visibility rays (lightVisRayBase) + ReSTIR tails.
		// Must match the allocation in InitGPUTaskBuffer().
		const u_int raySlotCount = taskCount + engine->lightTaskCount +
				taskCount * (
				((threadTaskConfig.pathTracer.restir.visCandCount > 0u) ?
				(threadTaskConfig.pathTracer.restir.visCandCount +
				RESTIR_PIXEL_MERGES_MAX) : 0u) +
				2u * threadTaskConfig.pathTracer.restirGI.giCandCount +
				((threadTaskConfig.pathTracer.restirGI.giCandCount > 0u) ?
				1u : 0u) +
				2u * threadTaskConfig.pathTracer.restirPT.ptCandCount +
				((threadTaskConfig.pathTracer.restirPT.ptCandCount > 0u) ?
				3u : 0u));
		for (u_int i = 0; i < iterations; ++i) {
			// Mid-batch abort check: each iteration is ~10ms of queued
			// work, so a scene edit/stop arriving here would otherwise
			// wait for the whole remaining batch (up to 128 iterations)
			// to drain before being applied
			if (stop_token.stop_requested())
				break;

			// Trace rays (tail slots hold the ReSTIR visibility
			// candidate shadow rays)
			intersectionDevice.EnqueueTraceRayBuffer(raysBuff, hitsBuff, raySlotCount);

			// Advance to next path state
			if (wavefrontQueues)
				EnqueueAdvancePathsWavefront();
			else
				EnqueueAdvancePathsKernel();
		}
		totalIterations += iterations;

		intersectionDevice.FinishQueue();

		// Path guiding (P1-3 M2b-2): drain GPU training records once per
		// batch, after the queue drain above. A per-iteration drain
		// serializes the loop - each blocking read flushes the whole
		// queue, so the deep queueing the adaptive batching exists to
		// build never materializes (and the GPU idles between drain and
		// re-enqueue). Records are overwrite-slots (task -> fixed slot),
		// so a lower drain frequency only sub-samples in time - the
		// 16x128 slots still deliver their latest record each drain,
		// and with ~100+ tasks sharing each slot the data was already
		// dominated by overwrite loss. New frozen round + re-upload
		// every 10th drain (see DrainGuide).
		DrainGuide();

		const double timeKernelEnd = WallClockTime();
		totalKernelTime += timeKernelEnd - timeKernelStart;

		/*if (threadIndex == 0)
			SLG_LOG("[DEBUG] transfer time: " << (timeTransferEnd - timeTransferStart) * 1000.0 << "ms "
					"kernel time: " << (timeKernelEnd - timeKernelStart) * 1000.0 << "ms "
					"iterations: " << iterations << " #"<< taskCount << ")");*/

		// Check if I have to adjust the number of kernel enqueued.
		// Converge proportionally instead of +/-1 per batch: the linear
		// ramp took ~120 batches (tens of seconds) to reach the deep
		// queueing this batching exists for, leaving a queue-drain
		// bubble per batch through the entire ramp - exactly when a
		// user is watching the viewport converge.
		const double batchTime = timeKernelEnd - timeKernelStart;
		if (batchTime > targetTime)
			iterations = Max<u_int>((u_int)(iterations * targetTime / batchTime), 1);
		else
			iterations = Min<u_int>(iterations * 2, 128);

		// Check halt conditions
		if (engine->GetFilm().GetConvergence() == 1.f)
			break;

		if (engine->photonGICache) {
			engine->photonGICache->Update(threadIndex, engine->GetTotalEyeSPP(), pgicUpdateCallBack);
			// A generation swap landed (pass counter bumped inside the
			// barrier completion): every render thread detects it here -
			// not only thread 0 - so multi-device sessions refresh each
			// device's taskConfig fields and query buffers alike.
			if (engine->photonGICache->GetCausticPhotonPass() != lastPGICPass) {
				lastPGICPass = engine->photonGICache->GetCausticPhotonPass();
				// B1': flush pending deposits before the query buffers
				// are recompiled (the queue is synchronized here); the
				// append buffers persist.
				if (engine->pgicDepositWanted && (engine->lightTaskCount > 0)) {
					intersectionDevice.EnqueueReadBuffer(taskStatsBuff, CL_TRUE,
							sizeof(slg::ocl::pathoclbase::GPUTaskStats) * taskCount,
							gpuTaskStats.get());
					double lightSampleCount = 0.0;
					for (size_t i = taskCount - engine->lightTaskCount;
							i < taskCount; ++i)
						lightSampleCount += gpuTaskStats[i].sampleCount;
					const u_int tracedDelta = (u_int)(lightSampleCount -
							lastPGICDrainLightCount);
					lastPGICDrainLightCount = lightSampleCount;
					DrainPGIC(tracedDelta);
				}
				// Refresh the device query fields of the new generation
				// (traced-path normalization + shrunken lookup radius) -
				// previously they stayed at their startup values for the
				// whole render. Deposit fields are session config that
				// CompilePhotonGI() resets, so preserve the live values.
				auto &pgicCfg = threadTaskConfig.pathTracer.pgic;
				const auto keepDeposit = pgicCfg.depositEnabled;
				const auto keepPhotonCap = pgicCfg.depositPhotonCapacity;
				const auto keepBeamCap = pgicCfg.depositBeamCapacity;
				pgicCfg = engine->compiledScene->compiledPathTracer.pgic;
				pgicCfg.depositEnabled = keepDeposit;
				pgicCfg.depositPhotonCapacity = keepPhotonCap;
				pgicCfg.depositBeamCapacity = keepBeamCap;
				intersectionDevice.EnqueueWriteBuffer(taskConfigBuff,
						CL_TRUE,
						sizeof(slg::ocl::pathoclbase::GPUTaskConfiguration),
						&threadTaskConfig);
				InitPhotonGI();
				SetKernelArgs();
			}
		}
	} // ~while

		//SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Rendering thread halted");
        if (stop_token.stop_requested())
		SLG_LOG("[PathOCLRenderThread::" << threadIndex << "] Rendering thread halted");

	threadFilms[0]->RecvFilm(intersectionDevice);
	intersectionDevice.FinishQueue();
	
	threadDone = true;

	// This is done to stop threads pending on barrier wait
	// inside engine->photonGICache->Update(). This can happen when an
	// halt condition is satisfied.
	if (engine->photonGICache)
		engine->photonGICache->FinishUpdate(threadIndex);
	
	intersectionDevice.PopThreadCurrentDevice();
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
