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

#include <thread>
#include <mutex>

#include "luxrays/devices/ocldevice.h"

#include "slg/slg.h"
#include "slg/kernels/kernels.h"
#include "slg/engines/tilepathocl/tilepathocl.h"

using namespace std;
using namespace luxrays;
using namespace slg;
using namespace std::chrono_literals;

//------------------------------------------------------------------------------
// TilePathOCLRenderThread
//------------------------------------------------------------------------------

TilePathOCLRenderThread::TilePathOCLRenderThread(const u_int index,
	HardwareIntersectionDeviceRef device, TilePathOCLRenderEngine *re) : 
	PathOCLBaseOCLRenderThread(index, device, re) {
	// Tile kernels can advance multiple states per iteration. Drain active
	// samples after the depth-based initial batch, including extra transparent
	// intersections; the wavefront one-state-per-iteration model differs.
	wavefrontQueues = false;
}

TilePathOCLRenderThread::~TilePathOCLRenderThread() {
}

void TilePathOCLRenderThread::GetThreadFilmSize(u_int *filmWidth, u_int *filmHeight,
		u_int *filmSubRegion) {
	TilePathOCLRenderEngine *engine = (TilePathOCLRenderEngine *)renderEngine;
	*filmWidth = engine->tileRepository->tileWidth;
	*filmHeight = engine->tileRepository->tileHeight;
	filmSubRegion[0] = 0; 
	filmSubRegion[1] = engine->tileRepository->tileWidth - 1;
	filmSubRegion[2] = 0;
	filmSubRegion[3] = engine->tileRepository->tileHeight - 1;
}

void TilePathOCLRenderThread::UpdateSamplerData(const TileWork &tileWork,
		slg::ocl::TilePathSamplerSharedData &sharedData) {
	TilePathOCLRenderEngine *engine = (TilePathOCLRenderEngine *)renderEngine;
	if (engine->oclSampler->type != slg::ocl::TILEPATHSAMPLER)
		throw runtime_error("Wrong sampler in PathOCLBaseRenderThread::UpdateSamplesBuffer(): " +
						ToString(engine->oclSampler->type));

	// Update samplerSharedDataBuff

	// To have an asynchronous EnqueueWriteBuffer(), I need to keep a copy of
	// sharedData around.
	sharedData.cameraFilmWidth = engine->GetFilm().GetWidth();
	sharedData.cameraFilmHeight =  engine->GetFilm().GetHeight();
	sharedData.tileStartX =  tileWork.GetCoord().x;
	sharedData.tileStartY =  tileWork.GetCoord().y;
	sharedData.tileWidth =  tileWork.GetCoord().width;
	sharedData.tileHeight =  tileWork.GetCoord().height;
	sharedData.tilePass =  tileWork.passToRender;
	sharedData.aaSamples =  engine->aaSamples;
	sharedData.multipassIndexToRender = tileWork.multipassIndexToRender;
	// Splat-acceptance halo = filter footprint radius (+ slack for the
	// LUT pixel-center discretization). FILTER_NONE keeps it at 0: a box
	// splat only covers the pixel holding its center.
	FilterRPtr pixelFilter = engine->GetPixelFilter();
	const bool hasFilter = pixelFilter && (pixelFilter->GetType() != FILTER_NONE);
	sharedData.splatMarginX = hasFilter ? (u_int)ceilf(pixelFilter->xWidth * .5f) + 2 : 0;
	sharedData.splatMarginY = hasFilter ? (u_int)ceilf(pixelFilter->yWidth * .5f) + 2 : 0;

	intersectionDevice.EnqueueWriteBuffer(samplerSharedDataBuff, CL_FALSE,
			sizeof(slg::ocl::TilePathSamplerSharedData), &sharedData);
}

void TilePathOCLRenderThread::RenderTileWork(const TileWork &tileWork,
		slg::ocl::TilePathSamplerSharedData &sharedData,
		const u_int filmIndex, std::stop_token stop_token) {
	TilePathOCLRenderEngine *engine = (TilePathOCLRenderEngine *)renderEngine;

	threadFilms[filmIndex]->GetFilm().Reset();
	if (threadFilms[filmIndex]->GetFilm().GetDenoiser().IsEnabled())
		threadFilms[filmIndex]->GetFilm().GetDenoiser().CopyReferenceFilm(FilmPtr(&engine->GetFilm()));

	// Clear the frame buffer
	threadFilms[filmIndex]->ClearFilm(intersectionDevice, filmClearKernel, filmClearWorkGroupSize);

	// Clear the frame buffer
	const u_int filmPixelCount = threadFilms[filmIndex]->GetFilm().GetWidth() * threadFilms[filmIndex]->GetFilm().GetHeight();
	intersectionDevice.EnqueueKernel(filmClearKernel,
		HardwareDeviceRange(RoundUp<u_int>(filmPixelCount, filmClearWorkGroupSize)),
		HardwareDeviceRange(filmClearWorkGroupSize));

	// Update all kernel args
	{
		std::unique_lock<std::mutex> lock(engine->setKernelArgsMutex);

		SetInitKernelArgs(filmIndex);

		SetAllAdvancePathsKernelArgs(filmIndex);
	}

	// Update Sampler shared data
	UpdateSamplerData(tileWork, sharedData);

	// Initialize the tasks buffer
	intersectionDevice.EnqueueKernel(initKernel,
			HardwareDeviceRange(engine->taskCount), HardwareDeviceRange(initWorkGroupSize));

	// There are 2 rays to trace for each path vertex (the last vertex
	// traces only one ray). The ReSTIR visibility path adds one more
	// trace per vertex that samples direct light: MK_DL_ILLUMINATE
	// queues the K candidate shadow rays and the MK_RT_RESTIR resolve
	// can only run after the next trace pass, so a non-last vertex
	// traces eye + candidates + winner shadow ray (3 passes).
	const u_int maxDepth = engine->pathTracer.maxPathDepth.depth;
	const bool visCands =
			(threadTaskConfig.pathTracer.restir.visCandCount > 0u);
	// ReSTIR GI (G1 GPU): the depth-0 vertex needs 2 more trace passes
	// (candidate bounce rays, then their NEE shadow rays) before the
	// resolve hands the winner back to MK_GENERATE_NEXT_VERTEX_RAY.
	// ReSTIR PT (PT-2) needs the same 2 passes (mutually exclusive).
	const u_int giPasses =
			(threadTaskConfig.pathTracer.restirGI.giCandCount > 0u) ?
			2u : 0u;
	const u_int ptPasses =
			(threadTaskConfig.pathTracer.restirPT.ptCandCount > 0u) ?
			2u : 0u;
	const u_int initialIterationCount =
			(maxDepth == 1) ? (visCands ? 3 : 2) + giPasses + ptPasses :
			(maxDepth * (visCands ? 3u : 2u) - (visCands ? 2u : 1u) +
			giPasses + ptPasses);
	auto cancelled = [&]() {
		return stop_token.stop_requested() || tileWorkCancelled.load(std::memory_order_acquire);
	};
	auto advance = [&]() {
		// Trace rays (tail slots hold the light camera-visibility rays
		// - lightVisRayBase - then the ReSTIR visibility candidate
		// shadow rays and the GI/PT bounce/NEE/merge-visibility rays)
		intersectionDevice.EnqueueTraceRayBuffer(raysBuff, hitsBuff,
				engine->taskCount + engine->lightTaskCount +
				engine->taskCount *
				(threadTaskConfig.pathTracer.restir.visCandCount +
				(visCands ? RESTIR_PIXEL_MERGES_MAX : 0u) +
				2u * threadTaskConfig.pathTracer.restirGI.giCandCount +
				((threadTaskConfig.pathTracer.restirGI.giCandCount > 0u) ?
				1u : 0u) +
				2u * threadTaskConfig.pathTracer.restirPT.ptCandCount +
				((threadTaskConfig.pathTracer.restirPT.ptCandCount > 0u) ?
				3u : 0u)));

		// Advance to next path state
		EnqueueAdvancePathsKernel();
	};
	// Bound the GPU backlog for viewport cancellation. The original full
	// iteration count still runs when work is not cancelled: ending the
	// initial batch early when eye paths finish would truncate LIGHT work.
	// Final/standalone sessions retain the existing unbounded submission.
	const bool interactiveGPU = (engine->GetType() == RTPATHOCL) &&
			engine->renderConfig.GetConfig().Get(
					Property("rtpath.interactive.gpu")(false)).Get<bool>();
	for (u_int i = 0; i < initialIterationCount && !cancelled(); ++i) {
		advance();
		if (interactiveGPU && ((i + 1u) % 8u == 0u))
			intersectionDevice.FinishQueue();
	}

	// A depth bound counts BSDF vertices, not transparent intersections,
	// shadow continuations or subsurface steps. A fixed trace count can leave
	// valid paths unsplatted and then discard them at the next tile Init.
	// Read actual completion; do not fill holes, increase sample weights or
	// change the transport/depth limits to make a frame appear complete.
	double eyeCount = 0.0;
	for (;;) {
		intersectionDevice.EnqueueReadBuffer(taskStatsBuff, CL_TRUE,
				sizeof(slg::ocl::pathoclbase::GPUTaskStats) * engine->taskCount,
				gpuTaskStats.get());
		bool pending = false;
		eyeCount = 0.0;
		for (u_int i = 0; i < engine->taskCount; ++i) {
			if (gpuTaskStats[i].activeEyeSample) {
				eyeCount += gpuTaskStats[i].sampleCount;
				pending |= !gpuTaskStats[i].sampleCount;
			}
		}
		if (!pending || cancelled())
			break;
		// Bound cancellation latency without imposing another physical path
		// cutoff. Completed eye tasks remain MK_DONE during these batches.
		for (u_int i = 0; i < 8 && !cancelled(); ++i)
			advance();
	}

	// All completed SampleResults are stable after the blocking stats read.
	// Resolve primary data on the device before this film slot is read back
	// or reused. No extra sample buffer, host transfer, or GPU spin lock.
	if (tileResolveFilmDataKernel) {
		auto &film = *threadFilms[filmIndex];
		u_int arg = 0;
		intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, taskConfigBuff);
		intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, taskStatsBuff);
		intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, sampleResultsBuff);
		intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, engine->eyeTaskCount);
		intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, engine->aaSamples);
		intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, film.GetFilm().GetWidth());
		intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, film.GetFilm().GetHeight());
		for (auto *buffer : {film.channel_DEPTH_Buff, film.channel_POSITION_Buff,
				film.channel_GEOMETRY_NORMAL_Buff, film.channel_SHADING_NORMAL_Buff,
				film.channel_MATERIAL_ID_Buff, film.channel_UV_Buff,
				film.channel_OBJECT_ID_Buff, film.channel_MOTION_VECTOR_Buff})
			intersectionDevice.SetKernelArg(tileResolveFilmDataKernel, arg++, buffer);
		intersectionDevice.EnqueueKernel(tileResolveFilmDataKernel,
				HardwareDeviceRange(RoundUp<u_int>(filmPixelCount, tileResolveFilmDataWorkGroupSize)),
				HardwareDeviceRange(tileResolveFilmDataWorkGroupSize));
	}

	// Async. transfer of the Film buffers (the tile coord lets the
	// engine-channel uploads map tile-local layout onto the film region)
	threadFilms[filmIndex]->RecvFilm(intersectionDevice,
			tileWork.GetCoord().x, tileWork.GetCoord().y);

	// GPU light tracing: snapshot this work's taskStats for the light
	// sample count. The Init kernel resets the counters at the start of
	// every work, so the in-order readback lands between this work's
	// kernels and the next work's reset - a per-work measurement, not
	// an estimate (the number of light paths a task completes per pass
	// depends on path termination and is not knowable on the host).
	if (engine->lightTaskCount > 0) {
		if (gpuTaskStatsPerFilm.size() <= filmIndex) {
			gpuTaskStatsPerFilm.resize(filmIndex + 1);
			gpuTaskStatsPerFilmSize.resize(filmIndex + 1, 0);
			lightSampleCountPending.resize(filmIndex + 1, 0);
		}
		// RTPATHOCL film edits reuse this render-thread object while changing
		// taskCount. The device buffer is resized by InitRender(), but this
		// per-film host snapshot survives Stop()/Start(). Match its capacity
		// before the async readback; a larger task population would otherwise
		// overwrite the heap. A film slot is reused only after FinishQueue().
		if (gpuTaskStatsPerFilmSize[filmIndex] != engine->taskCount) {
			gpuTaskStatsPerFilm[filmIndex] = std::make_unique<
					slg::ocl::pathoclbase::GPUTaskStats[]>(engine->taskCount);
			gpuTaskStatsPerFilmSize[filmIndex] = engine->taskCount;
		}
		intersectionDevice.EnqueueReadBuffer(taskStatsBuff, CL_FALSE,
				sizeof(slg::ocl::pathoclbase::GPUTaskStats) * engine->taskCount,
				gpuTaskStatsPerFilm[filmIndex].get());
		lightSampleCountPending[filmIndex] = 1;
	}

	// Count completed camera samples from the same drained work as the film.
	// Full tile area overstates sparse RT passes by resolutionReduction^2;
	// inactive padding, light tasks and cancelled unsplatted paths do not
	// contribute camera samples. Tile pass/sampling progression is unchanged.
	// This film contains one work item. Set its exact totals: a small edge
	// tile must not lose a pending (<64) CPU-style counter batch at merge.
	// The screen-normalized count is deferred to ConsumeLightSampleCounts().
	threadFilms[filmIndex]->GetFilm().SetSampleCount(eyeCount, eyeCount, 0.0);
}

void TilePathOCLRenderThread::ConsumeLightSampleCounts() {
	TilePathOCLRenderEngine *engine = (TilePathOCLRenderEngine *)renderEngine;
	const u_int eyeTaskCount = engine->taskCount - engine->lightTaskCount;

	for (u_int i = 0; i < lightSampleCountPending.size(); ++i) {
		if (!lightSampleCountPending[i])
			continue;
		lightSampleCountPending[i] = 0;

		// Light tasks occupy the tail gids [eyeTaskCount, taskCount)
		double lightCount = 0.0;
		const slg::ocl::pathoclbase::GPUTaskStats *stats = gpuTaskStatsPerFilm[i].get();
		for (u_int t = eyeTaskCount; t < engine->taskCount; ++t)
			lightCount += stats[t].sampleCount;
		Film &film = threadFilms[i]->GetFilm();
		const double eyeCount = film.GetTotalEyeSampleCount();
		film.SetSampleCount(eyeCount + lightCount, eyeCount, lightCount);
	}
}

static void PGICUpdateCallBack(CompiledScene *compiledScene) {
	compiledScene->RecompilePhotonGI();
}

void TilePathOCLRenderThread::RenderThreadImpl(std::stop_token stop_token) {
	SLG_LOG("[TilePathOCLRenderThread::" << threadIndex << "] Rendering thread started");

	TilePathOCLRenderEngine *engine = (TilePathOCLRenderEngine *)renderEngine;

	intersectionDevice.PushThreadCurrentDevice();

        //----------------------------------------------------------------------
        // Initialization
        //----------------------------------------------------------------------

        const u_int taskCount = engine->taskCount;

        // Initialize random number generator seeds
        intersectionDevice.EnqueueKernel(initSeedKernel,
                        HardwareDeviceRange(taskCount), HardwareDeviceRange(initWorkGroupSize));

        //----------------------------------------------------------------------
        // Extract the tile to render
        //----------------------------------------------------------------------

        vector<TileWork> tileWorks(1);
        vector<slg::ocl::TilePathSamplerSharedData> samplerDatas(1);
        const std::function<void()> pgicUpdateCallBack = std::bind(PGICUpdateCallBack, engine->compiledScene);
        // Caustic generation last seen by this device: the swap lands
        // on the last barrier arrival, so threads != 0 detect it via
        // the pass counter (Update() only returns true on thread 0)
        u_int lastPGICPass = 0;

        while (!stop_token.stop_requested()) {
                // Check if we are in pause mode
                if (engine->pauseMode) {
                        // Check every 100ms if I have to continue the rendering
                        while (!stop_token.stop_requested() && engine->pauseMode)
                                std::this_thread::sleep_for(100ms);

                        if (stop_token.stop_requested())
                                break;
                }

                const double t0 = WallClockTime();

                // Enqueue the rendering of all tiles

                bool allTileDone = true;
                for (u_int i = 0; i < tileWorks.size(); ++i) {
                        if (engine->tileRepository->NextTile(engine->GetFilm(), engine->filmMutex, tileWorks[i], threadFilms[i]->GetFilm())) {
                                //SLG_LOG("[TilePathOCLRenderThread::" << threadIndex << "] TileWork: " << tileWork);

                                // Render the tile
                                RenderTileWork(tileWorks[i], samplerDatas[i], i, stop_token);

                                allTileDone = false;
                        } else
                                tileWorks[i].Reset();
                }

                // RenderTileWork has already read the completed task statistics.
                intersectionDevice.FinishQueue();
				for (auto &threadFilm : threadFilms) threadFilm->CommitDenoisingGuides();

                // Fold the measured light-path counts into the thread
                // films; they are merged into the engine film by the
                // NextTile() call on the next pass
                ConsumeLightSampleCounts();

                const double t1 = WallClockTime();
                const double renderingTime = t1 - t0;
                //SLG_LOG("[TilePathOCLRenderThread::" << threadIndex << "] " << tiles.size() << "xTile(s) rendering time: " << (u_int)(renderingTime * 1000.0) << "ms");

                if (allTileDone)
                        break;

                // Check the the time spent, if it is too small (< 400ms) get more tiles
                // (avoid to increase the number of tiles on CPU devices, it is useless)
                if ((tileWorks.size() < engine->maxTilePerDevice) && (renderingTime < 0.4) && 
                                (intersectionDevice.GetDeviceDesc().GetType() != DEVICE_TYPE_OPENCL_CPU)) {
                        IncThreadFilms();
                        tileWorks.resize(tileWorks.size() + 1);
                        samplerDatas.resize(samplerDatas.size() + 1);

                        SLG_LOG("[TilePathOCLRenderThread::" << threadIndex << "] Increased the number of rendered tiles to: " << tileWorks.size());
                }

                // PSR halflife decay (path.regularization.halflife): recompute
                // the effective sigma once per batch and re-upload taskConfig;
                // already-seeded paths keep their sigma (Kaplanyan decaying
                // schedule, not mid-path mutation)
                if (engine->pathTracer.regularizationHalflife > 0.f) {
                        const double rspp = engine->GetFilm().GetTotalEyeSampleCount() /
                                engine->GetFilm().GetPixelCount();
                        const float s = engine->pathTracer.EffectiveRegularizationSigma(rspp);
                        if (s != threadTaskConfig.pathTracer.regularizationSigma) {
                                threadTaskConfig.pathTracer.regularizationSigma = s;
                                intersectionDevice.EnqueueWriteBuffer(taskConfigBuff,
                                        CL_TRUE,
                                        sizeof(slg::ocl::pathoclbase::GPUTaskConfiguration),
                                        &threadTaskConfig);
                        }
                }

                if (stop_token.stop_requested())
                        break;
                if (engine->photonGICache) {
                        const u_int spp = engine->GetFilm().GetTotalEyeSampleCount() / engine->GetFilm().GetPixelCount();

                        engine->photonGICache->Update(threadIndex, spp, pgicUpdateCallBack);
                        // A generation swap lands on the last barrier
                        // arrival: every render thread refreshes its own
                        // device on the pass bump (Update() itself only
                        // returns true on thread 0)
                        if (engine->photonGICache->GetCausticPhotonPass() != lastPGICPass) {
                                lastPGICPass = engine->photonGICache->GetCausticPhotonPass();
                                // Refresh the device query fields of the
                                // new generation (same pattern as
                                // pathoclopenclthread.cpp): deposit fields
                                // are session config, preserved across the
                                // compile-struct assignment
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
        }
        if (stop_token.stop_requested())
		SLG_LOG("[TilePathOCLRenderThread::" << threadIndex << "] Rendering thread halted");

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
