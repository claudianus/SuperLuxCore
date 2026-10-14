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

#include "slg/slg.h"
#include "slg/samplers/rtpathcpusampler.h"
#include <memory>
#include "slg/engines/rtpathcpu/rtpathcpu.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// RTPathCPURenderEngine
//------------------------------------------------------------------------------

RTPathCPURenderEngine::RTPathCPURenderEngine(RenderConfigRef rcfg) :
		PathCPURenderEngine(rcfg) {
	threadsSyncBarrier = new std::barrier(renderThreads.size() + 1, completion_t());
}

RTPathCPURenderEngine::~RTPathCPURenderEngine() {
	delete threadsSyncBarrier;
}

void RTPathCPURenderEngine::StartLockLess() {
	auto& cfg = renderConfig.GetConfig();
	zoomFactor = (u_int)Max(1, cfg.Get(GetDefaultProps()->Get("rtpathcpu.zoomphase.size")).Get<int>());
	zoomWeight = Max(0.0001, cfg.Get(GetDefaultProps()->Get("rtpathcpu.zoomphase.weight")).Get<double>());

	threadsPauseMode = false;
	threadsKeepFrame = false;
	firstFrameDone = false;
	firstFrameThreadDoneCount = 0;

	PathCPURenderEngine::StartLockLess();

	photonGICachePoll = (photonGICache != nullptr);
}

void RTPathCPURenderEngine::StopLockLess() {
	// RenderEngine::Stop() still calls UpdateFilmLockLess() once the
	// threads are joined: a swap there would pause threads that are gone
	photonGICachePoll = false;

	// We need to resume paused sessions first,
	// otherwise it will hang on a thread barrier. Threads parked by a
	// scene/film edit (threadsPauseMode set while pauseMode stays false)
	// must be released exactly the same way.
	if (pauseMode)
		Resume();
	else if (threadsPauseMode)
		ResumeThreads();
	PathCPURenderEngine::StopLockLess();
}

void RTPathCPURenderEngine::SetRuntimeResolutionReduction(const u_int reduction) {
	// The decimation factor is read live by the samplers on both the
	// first-frame coarse pass and the normal path
	zoomFactor = Max(1u, reduction);
	// Keep the shared-data copy in sync so the next Reset() rebuilds the
	// coarse first-frame sequence with the new factor
	if (auto *sd = dynamic_cast<RTPathCPUSamplerSharedData *>(samplerSharedData.get()))
		sd->SetZoomFactor(reduction);
}

void RTPathCPURenderEngine::WaitNewFrame() {
	// !started: no render threads will ever signal the first frame
	if (started && !firstFrameDone && !pauseMode && !editMode) {
		// Wait for the signal from all the rendering threads
		std::unique_lock<std::mutex> lock(firstFrameMutex);
		while (firstFrameThreadDoneCount < renderThreads.size())
			firstFrameCondition.wait(lock);

		firstFrameDone = true;
	}
}

void RTPathCPURenderEngine::PauseThreads() {
	// Serialize against StopLockLess(): the barrier expects one arrival
	// per render thread, so pausing once the threads have already been
	// joined (stale session handle, failed start) would block the caller
	// on arrive_and_wait() forever - the parties that must arrive no
	// longer exist. Render threads never take engineMutex, so holding it
	// across the wait cannot deadlock them.
	std::lock_guard<std::recursive_mutex> lock(engineMutex);
	if (!started)
		return;

	// Tell the threads to pause the rendering
	threadsPauseMode = true;

	// Wait for the threads
	threadsSyncBarrier->arrive_and_wait();
}

void RTPathCPURenderEngine::ResumeThreads() {
	std::lock_guard<std::recursive_mutex> lock(engineMutex);
	if (!started)
		return;

	threadsPauseMode = false;
	threadsKeepFrame = false;
	firstFrameDone = false;
	firstFrameThreadDoneCount = 0;

	// Let's the threads to resume the rendering
	threadsSyncBarrier->arrive_and_wait();
}

void RTPathCPURenderEngine::ResumeThreadsKeepFrame() {
	std::lock_guard<std::recursive_mutex> lock(engineMutex);
	if (!started)
		return;

	// Written while every thread is parked between the two barrier
	// phases; they read it after the second one
	threadsPauseMode = false;
	threadsKeepFrame = true;

	threadsSyncBarrier->arrive_and_wait();
}

void RTPathCPURenderEngine::Pause() {
	PathCPURenderEngine::Pause();

	PauseThreads();
}

void RTPathCPURenderEngine::Resume() {
	PathCPURenderEngine::Resume();

	ResumeThreads();	
}

void RTPathCPURenderEngine::BeginSceneEditLockLess() {
	// Check if the threads are already suspended for pause
	if (!pauseMode)
		PauseThreads();

	// The background photon trace reads the scene (the camera too,
	// for frustum culling): join it before the edit mutates anything
	if (photonGICache)
		photonGICache->AbortUpdate();
}

void RTPathCPURenderEngine::EndSceneEditLockLess(const EditActionList &editActions) {
	// On a stopped engine samplerSharedData is already released
	if (!started)
		return;

	GetFilm().Reset();
	samplerSharedData->Reset();
	if (lightSamplerSharedData)
		lightSamplerSharedData->Reset();

	if (photonGICache) {
		if (editActions.HasOnly(CAMERA_EDIT)) {
			// Photons do not depend on the camera: keep the live
			// generation (orbiting the viewport keeps its caustics)
			photonGICache->RestartUpdateClock();
		} else {
			// Geometry, material or light edits invalidate the photons.
			// The new cache renders like no cache (path-traced SDS)
			// until its first background generation lands.
			delete photonGICache;
			photonGICache = NewPhotonGICache();
			if (photonGICache) {
				photonGICache->SetMneeSeedCache(pathTracer.mneeSeeds.get());
				photonGICache->MergeMneeSeeds();
			}
			pathTracer.SetPhotonGICache(photonGICache);
			photonGICachePoll = (photonGICache != nullptr);
		}
	}

	// Check if the threads were already suspended for pause
	if (!pauseMode)
		ResumeThreads();
}

void RTPathCPURenderEngine::UpdateFilmLockLess() {
	// Render threads use directly the engine Film: the only work here is
	// the viewport caustic cache, polled from the thread that draws the
	// viewport (RenderSession::UpdateStats -> UpdateFilm, engineMutex
	// held). The render threads never enter the cache's own barrier.
	if (!photonGICachePoll || pauseMode || editMode || threadsPauseMode)
		return;
	// Threads leave the loop on a halt condition (convergence is only
	// raised by film->RunTests(), after this call): pausing with a
	// party gone would block forever
	if (GetFilm().GetConvergence() >= 1.f)
		return;
	for (auto &t : renderThreads)
		if (!t || t->HasDone())
			return;

	const u_int pixelCount = Max(1u, GetFilm().GetPixelCount());
	const u_int spp = (u_int)(GetFilm().GetTotalEyeSampleCount() / pixelCount);
	if (!photonGICache->PollUpdate(spp))
		return;

	const bool firstGeneration = !photonGICache->IsCausticReady();

	PauseThreads();
	photonGICache->ApplyPendingUpdate();

	if (firstGeneration && photonGICache->IsCausticReady()) {
		// Everything rendered so far path-traced the SDS paths the cache
		// now replaces (fireflies that would linger in the average):
		// restart the frame on the cache, coarse first frame included
		GetFilm().Reset();
		samplerSharedData->Reset();
		if (lightSamplerSharedData)
			lightSamplerSharedData->Reset();
		photonGICache->RestartUpdateClock();
		ResumeThreads();
	} else {
		// Later generations refine the same estimate (progressive
		// photon mapping): keep accumulating
		ResumeThreadsKeepFrame();
	}
}

// A fast path for film resize
void RTPathCPURenderEngine::BeginFilmEdit() {
	// Check if the threads are already suspended for pause
	if (!pauseMode)
		PauseThreads();
}

// A fast path for film resize
void RTPathCPURenderEngine::EndFilmEdit(FilmRef flm, std::mutex *flmMutex) {
	std::lock_guard<std::recursive_mutex> lock(engineMutex);

	// Update the film pointer
	film = &flm;
	filmMutex = flmMutex;

	// Nothing else to do on a stopped engine (samplerSharedData is gone)
	if (!started)
		return;

	InitFilm();

	static_cast<RTPathCPUSamplerSharedData *>(samplerSharedData.get())->Reset( GetFilmPtr());

	// Check if the threads were already suspended for pause
	if (!pauseMode)
		ResumeThreads();
}

//------------------------------------------------------------------------------
// Static methods used by RenderEngineRegistry
//------------------------------------------------------------------------------

PropertiesUPtr RTPathCPURenderEngine::ToProperties(const Properties &cfg) {
	PropertiesUPtr props = PathCPURenderEngine::ToProperties(cfg);
	
	*props <<
				//------------------------------------------------------------------
			// Overwrite some PathCPURenderEngine property
			//------------------------------------------------------------------
			cfg.Get(GetDefaultProps()->Get("renderengine.type")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.total")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.diffuse")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.glossy")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.specular")) <<
			//------------------------------------------------------------------
			cfg.Get(GetDefaultProps()->Get("rtpathcpu.zoomphase.size")) <<
			cfg.Get(GetDefaultProps()->Get("rtpathcpu.zoomphase.weight"));
	
	return props;
}

RenderEngine *RTPathCPURenderEngine::FromProperties(RenderConfigRef rcfg) {
	return new RTPathCPURenderEngine(rcfg);
}

PropertiesUPtr RTPathCPURenderEngine::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			PathCPURenderEngine::GetDefaultProps() <<
			//------------------------------------------------------------------
			// Overwrite some PathCPURenderEngine property
			//------------------------------------------------------------------
			Property("renderengine.type")(GetObjectTag()) <<
			Property("path.pathdepth.total")(5) <<
			Property("path.pathdepth.diffuse")(3) <<
			Property("path.pathdepth.glossy")(3) <<
			Property("path.pathdepth.specular")(3) <<
			//------------------------------------------------------------------
			Property("rtpathcpu.zoomphase.size")(4) <<
			Property("rtpathcpu.zoomphase.weight")(.1f);

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
