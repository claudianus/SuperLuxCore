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

#include "luxrays/utils/properties.h"
#include <memory>
#include <cstring>
#if !defined(LUXRAYS_DISABLE_OPENCL)

#include "slg/slg.h"
#include "slg/cameras/perspective.h"
#include "slg/film/imagepipeline/imagepipeline.h"
#include "slg/engines/rtpathocl/rtpathocl.h"
#include "slg/film/denoiser/denoisingguide.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// RTPathOCLRenderEngine
//------------------------------------------------------------------------------

RTPathOCLRenderEngine::RTPathOCLRenderEngine(RenderConfigRef rcfg) :
		TilePathOCLRenderEngine(rcfg, false) {
	if (nativeRenderThreadCount > 0)
		throw runtime_error("opencl.native.threads.count must be 0 for RTPATHOCL");

	syncBarrier = new std::barrier(2, completion_t());
	syncThreadsRunning = false;
	if (renderOCLThreads.size() > 1)
		frameBarrier = new std::barrier(renderOCLThreads.size(), completion_t());
	else
		frameBarrier = nullptr;

	runtimeResolutionReduction = 0;
	frameTime = 0.f;
}

RTPathOCLRenderEngine::~RTPathOCLRenderEngine() {
	delete frameBarrier;
}

void RTPathOCLRenderEngine::InitGPUTaskConfiguration() {
	TilePathOCLRenderEngine::InitGPUTaskConfiguration();

	taskConfig.renderEngine.rtpathocl.previewResolutionReduction = previewResolutionReduction;
	taskConfig.renderEngine.rtpathocl.previewResolutionReductionStep = previewResolutionReductionStep;
	taskConfig.renderEngine.rtpathocl.resolutionReduction = resolutionReduction;
}

PathOCLBaseOCLRenderThread *RTPathOCLRenderEngine::CreateOCLThread(const u_int index,
	HardwareIntersectionDeviceRef device) {
	return new RTPathOCLRenderThread(index, device, this);
}

void RTPathOCLRenderEngine::UpdateResolutionReduction() {
	auto &cfg = renderConfig.GetConfig();
	previewResolutionReduction = RoundUpPow2(Min(Max(1, cfg.Get(GetDefaultProps()->Get("rtpath.resolutionreduction.preview")).Get<int>()), 64));
	previewResolutionReductionStep = Min(Max(1, cfg.Get(GetDefaultProps()->Get("rtpath.resolutionreduction.preview.step")).Get<int>()), 64);
	resolutionReduction = RoundUpPow2(Min(Max(1, cfg.Get(GetDefaultProps()->Get("rtpath.resolutionreduction")).Get<int>()), 64));
	if (denoisingFramesEnabled)
		previewResolutionReduction = resolutionReduction = 1;
}

void RTPathOCLRenderEngine::StartLockLess() {
	//--------------------------------------------------------------------------
	// Rendering parameters
	//--------------------------------------------------------------------------
	
	// Disable denoiser statistics collection
	GetFilm().GetDenoiser().SetEnabled(false);

	auto& cfg = renderConfig.GetConfig();
	denoisingFramesEnabled = cfg.Get(Property("rtpath.denoisingframes.enable")(false)).Get<bool>();
	denoisingImagePipelineEnabled = cfg.Get(Property("rtpath.denoisingframes.imagepipeline.enable")(false)).Get<bool>();
	denoisingImagePipelineAccumulated = cfg.Get(Property("rtpath.denoisingframes.imagepipeline.accumulated")(false)).Get<bool>();
	denoisingInputReduction = cfg.Get(Property("rtpath.denoisingframes.input.reduction")(1u)).Get<u_int>();
	if (denoisingInputReduction != 1u && denoisingInputReduction != 2u)
		throw runtime_error("Canonical denoising input reduction must be 1 or 2");
	if (denoisingInputReduction > 1u && (!denoisingFramesEnabled || !denoisingImagePipelineEnabled ||
			GetFilm().GetWidth() % 2u || GetFilm().GetHeight() % 2u ||
			GetFilm().GetWidth() < 32u || GetFilm().GetHeight() < 32u))
		throw runtime_error("Reduced neural input requires coherent pipeline capture and an even target raster of at least 32x32");
	denoisingLosslessDelivery = cfg.Get(Property("rtpath.denoisingframes.lossless")(false)).Get<bool>();
	denoisingExactHaltSPP = cfg.Get(Property("rtpath.denoisingframes.haltspp")(u_longlong(0))).Get<u_longlong>();
	denoisingImagePipelineMaxBytes = cfg.Get(Property("rtpath.denoisingframes.imagepipeline.maxbytes")(u_longlong(256) * 1024 * 1024)).Get<u_longlong>();
	denoisingImagePipelineRevision = GetFilm().GetImagePipelineRevision();
	denoisingRequiredPipelineRevision = denoisingImagePipelineRevision;
	if (denoisingImagePipelineEnabled && !denoisingFramesEnabled)
		throw runtime_error("Coherent image-pipeline contexts require independent denoising frames");
	if ((denoisingLosslessDelivery || denoisingImagePipelineAccumulated || denoisingExactHaltSPP > 0) && !denoisingFramesEnabled)
		throw runtime_error("Lossless final delivery requires independent denoising frames");
	if (denoisingImagePipelineAccumulated && !denoisingImagePipelineEnabled)
		throw runtime_error("Accumulated presentation requires coherent image-pipeline capture");
	if (denoisingFramesEnabled && cfg.Get(Property("path.lighttracing.only")(false)).Get<bool>())
		throw runtime_error("Independent denoising frames require camera samples, not light-only tracing");

	UpdateResolutionReduction();
	if (denoisingFramesEnabled) {
		runtimeResolutionReduction = 0;
		InitDenoisingFrameFilm();
		std::lock_guard<std::mutex> lock(denoisingFrameMutex);
		latestDenoisingFrame.reset();
		denoisingAcknowledgedFrameId = denoisingFrameId;
		denoisingFramesStopped = false;
		denoisingFrameHalt = false;
		++denoisingSceneEpoch;
	}

	TilePathOCLRenderEngine::StartLockLess();

	// Force to use only 1 tile for each device
	maxTilePerDevice = 1;

	tileRepository->enableRenderingDonePrint = false;
	// RT tiles contain real scattered samples, not replicated preview blocks.
	// Start/edit/resize already clear the film explicitly. Replacing pass 1
	// would discard the preceding valid pass while retaining its counters.
	tileRepository->enableFirstPassClear = false;

	updateActions.Reset();
	useFastCameraEditPath = false;
	cameraIsUsingCustomBokeh = (renderConfig.GetScene().GetCamera().GetType() == Camera::PERSPECTIVE) &&
			(dynamic_cast<const PerspectiveCamera*>(&renderConfig.GetScene().GetCamera()))->bokehDistributionImageMap;

	// To synchronize the start of all threads
	syncType = SYNCTYPE_NONE;
	syncBarrier->arrive_and_wait();

	// The handshake succeeded: render thread 0 is in its sync loop and
	// StopLockLess() may wait on the barrier (a failed start never
	// reaches this point and must not block).
	syncThreadsRunning = true;
}

void RTPathOCLRenderEngine::StopLockLess() {
	{
		std::lock_guard<std::mutex> lock(denoisingFrameMutex);
		denoisingFramesStopped = true;
		denoisingFrameCondition.notify_all();
	}
	// Only handshake when the render threads actually entered their sync
	// loop: after a failed Start (i.e. a kernel compilation error) the
	// barrier partner does not exist and waiting would hang forever.
	if (syncThreadsRunning) {
		syncThreadsRunning = false;

		syncType = SYNCTYPE_STOP;
		for (auto *thread : renderOCLThreads)
			static_cast<RTPathOCLRenderThread *>(thread)->CancelTileWork();
		syncBarrier->arrive_and_wait();

		// All render threads are now suspended and I can set the interrupt signal
		for (size_t i = 0; i < renderOCLThreads.size(); ++i)
			((RTPathOCLRenderThread *)renderOCLThreads[i])->renderThread->request_stop();

		syncType = SYNCTYPE_NONE;
		syncBarrier->arrive_and_wait();
	}

	// Render threads will now detect the interruption

	TilePathOCLRenderEngine::StopLockLess();
	denoisingFrameFilm.reset();
	{
		std::lock_guard<std::mutex> lock(denoisingFrameMutex);
		latestDenoisingFrame.reset();
	}
}

void RTPathOCLRenderEngine::EndSceneEdit(const EditActionList &editActions) {
	// Check if I can use the fast camera edit path
	if (!denoisingFramesEnabled && editActions.HasOnly(CAMERA_EDIT) &&
			(renderConfig.GetScene().GetCamera().GetType() == Camera::PERSPECTIVE) &&
			// Camera is not using custom bokeh
			!(dynamic_cast<const PerspectiveCamera*>(&renderConfig.GetScene().GetCamera()))->bokehDistributionImageMap &&
			// Camera was not using custom bokeh
			!cameraIsUsingCustomBokeh) {
		TilePathOCLRenderEngine::EndSceneEdit(editActions);
		useFastCameraEditPath = true;
		for (auto *thread : renderOCLThreads)
			static_cast<RTPathOCLRenderThread *>(thread)->CancelTileWork();
	} else {
		syncType = SYNCTYPE_ENDSCENEEDIT;
		updateActions.AddActions(editActions.GetActions());
		for (auto *thread : renderOCLThreads)
			static_cast<RTPathOCLRenderThread *>(thread)->CancelTileWork();
		syncBarrier->arrive_and_wait();

		TilePathOCLRenderEngine::EndSceneEdit(editActions);
		cameraIsUsingCustomBokeh = (renderConfig.GetScene().GetCamera().GetType() == Camera::PERSPECTIVE) &&
				(dynamic_cast<const PerspectiveCamera*>(&renderConfig.GetScene().GetCamera()))->bokehDistributionImageMap;
		syncBarrier->arrive_and_wait();
		
		// Here, rendering thread 0 will update all OpenCL buffers here

		syncType = SYNCTYPE_NONE;
		syncBarrier->arrive_and_wait();
	}
}

// A fast path for film resize
void RTPathOCLRenderEngine::BeginFilmEdit() {
	syncType = SYNCTYPE_BEGINFILMEDIT;
	for (auto *thread : renderOCLThreads)
		static_cast<RTPathOCLRenderThread *>(thread)->CancelTileWork();
	syncBarrier->arrive_and_wait();

	// All render threads are now suspended and I can set the interrupt signal
	for (size_t i = 0; i < renderOCLThreads.size(); ++i)
		((RTPathOCLRenderThread *)renderOCLThreads[i])->renderThread->request_stop();

	syncType = SYNCTYPE_NONE;
	syncBarrier->arrive_and_wait();

	// Render threads will now detect the interruption
	for (size_t i = 0; i < renderOCLThreads.size(); ++i)
		renderOCLThreads[i]->Stop();
}

// A fast path for film resize
void RTPathOCLRenderEngine::EndFilmEdit(FilmRef flm, std::mutex *flmMutex) {
	// Threads are stopped by BeginFilmEdit. A viewport resize can cross
	// the configured sampling-budget boundary too: size task buffers for
	// the new policy before restarting them, without rebuilding the scene.
	UpdateResolutionReduction();
	taskConfig.renderEngine.rtpathocl.previewResolutionReduction = previewResolutionReduction;
	taskConfig.renderEngine.rtpathocl.previewResolutionReductionStep = previewResolutionReductionStep;
	taskConfig.renderEngine.rtpathocl.resolutionReduction = resolutionReduction;
	// Update the film pointer
	film = &flm;
	filmMutex = flmMutex;
	InitFilm();
	ResizeDenoisingGuides();
	if (denoisingFramesEnabled) {
		InitDenoisingFrameFilm();
		++denoisingSceneEpoch;
		denoisingFrameHalt = false;
		std::lock_guard<std::mutex> lock(denoisingFrameMutex);
		latestDenoisingFrame.reset();
	}

	// Disable denoiser statistics collection
	GetFilm().GetDenoiser().SetEnabled(false);

	// Create a tile repository based on the new film
	InitTileRepository();
	tileRepository->enableRenderingDonePrint = false;
	tileRepository->enableFirstPassClear = false;

	// The camera has been updated too
	EditActionList a;
	a.AddActions(CAMERA_EDIT);
	compiledScene->Recompile(a);

	// Re-start all rendering threads. The handshake is re-established
	// below: a thread failing to start (kernel compilation error) must
	// not leave StopLockLess() waiting on the barrier.
	syncThreadsRunning = false;
	for (size_t i = 0; i < renderOCLThreads.size(); ++i)
		renderOCLThreads[i]->Start();

	// To synchronize the start of all threads
	syncType = SYNCTYPE_NONE;
	syncBarrier->arrive_and_wait();
	syncThreadsRunning = true;
}

void RTPathOCLRenderEngine::UpdateFilmLockLess() {
	// Nothing to do: the render threads are in charge of updating the film
}

void RTPathOCLRenderEngine::InitDenoisingFrameFilm() {
	const Film &source = GetFilm();
	if (denoisingInputReduction > 1u) {
		const auto region = source.GetSubRegion();
		if (source.GetWidth() % 2u || source.GetHeight() % 2u ||
				source.GetWidth() < 32u || source.GetHeight() < 32u ||
				region[0] || region[2] || region[1] != source.GetWidth() - 1u || region[3] != source.GetHeight() - 1u)
			throw runtime_error("Canonical reduced input requires an even complete target raster");
	}
	denoisingFrameFilm = Film::Create(source.GetWidth(), source.GetHeight(), source.GetSubRegion());
	denoisingFrameFilm->CopyDynamicSettings(source);
	denoisingFrameFilm->hwEnable = false;
	denoisingFrameFilm->GetDenoiser().SetEnabled(false);
	denoisingFrameFilm->Init();
}

std::shared_ptr<const DenoisingFrameSnapshot> RTPathOCLRenderEngine::GetDenoisingFrame(
		const uint64_t afterId, const u_int timeoutMs) {
	std::unique_lock<std::mutex> lock(denoisingFrameMutex);
	if (!denoisingFramesEnabled) throw runtime_error("Independent denoising frames are disabled");
	denoisingFrameCondition.wait_for(lock, std::chrono::milliseconds(Min(timeoutMs, 1000u)), [&] {
		return denoisingFramesStopped || (latestDenoisingFrame && latestDenoisingFrame->frameId > afterId);
	});
	return latestDenoisingFrame && latestDenoisingFrame->frameId > afterId ? latestDenoisingFrame : nullptr;
}

void RTPathOCLRenderEngine::InvalidateDenoisingImagePipeline(const uint64_t revision) {
	if (!denoisingFramesEnabled) return;
	std::lock_guard<std::mutex> lock(denoisingFrameMutex);
	denoisingRequiredPipelineRevision = revision;
	latestDenoisingFrame.reset();
	denoisingFrameCondition.notify_all();
}

void RTPathOCLRenderEngine::AcknowledgeDenoisingFrame(const std::shared_ptr<const DenoisingFrameSnapshot> &frame, const bool keepLossless) {
	std::lock_guard<std::mutex> lock(denoisingFrameMutex);
	if (!denoisingLosslessDelivery || denoisingFramesStopped)
		throw runtime_error("Lossless denoising-frame delivery is not active");
	if (!frame || latestDenoisingFrame != frame)
		throw runtime_error("Only the currently published denoising frame may be acknowledged");
	denoisingAcknowledgedFrameId = frame->frameId;
	if (!keepLossless) denoisingLosslessDelivery = false;
	denoisingFrameCondition.notify_all();
}

void RTPathOCLRenderEngine::ReleaseDenoisingFrameDelivery() {
	std::lock_guard<std::mutex> lock(denoisingFrameMutex);
	denoisingLosslessDelivery = false;
	denoisingFrameCondition.notify_all();
}

void RTPathOCLRenderEngine::SetDenoisingFrameHaltSPP(const uint64_t samples) {
	if (!denoisingFramesEnabled)
		throw runtime_error("Independent denoising frames are disabled");
	// The caller holds filmMutex, matching the render thread's cap read.
	// Lock order remains film -> frame. A parked completed frame can resume
	// after raising/disabling the cap, without discarding its raw accumulation.
	std::lock_guard<std::mutex> lock(denoisingFrameMutex);
	denoisingExactHaltSPP = samples;
	denoisingFrameHalt = false;
	denoisingFrameCondition.notify_all();
}

void RTPathOCLRenderEngine::PublishDenoisingFrame(const slg::ocl::Camera &camera) {
	// All devices have finished and merged their tiles at the frame barrier.
	// This film contains only this frame; the ordinary film still accumulates
	// every contribution and AOV for raw/final/diagnostic consumers.
	auto snapshot = std::make_shared<DenoisingFrameSnapshot>();
	snapshot->frameId = ++denoisingFrameId;
	snapshot->sceneEpoch = denoisingSceneEpoch;
	snapshot->losslessDelivery = denoisingLosslessDelivery;
	snapshot->width = GetFilm().GetWidth(); snapshot->height = GetFilm().GetHeight();
	snapshot->targetWidth = snapshot->width; snapshot->targetHeight = snapshot->height;
	snapshot->inputReduction = denoisingInputReduction;
	snapshot->jitterX = taskConfig.pathTracer.pixelJitterX;
	snapshot->jitterY = taskConfig.pathTracer.pixelJitterY;
	const size_t pixels = size_t(snapshot->width) * snapshot->height;
	snapshot->color.resize(pixels * 4);
	snapshot->sampleCount.resize(pixels);
	denoisingFrameFilm->GetOutput<u_int>(FilmOutputs::SAMPLECOUNT, snapshot->sampleCount.data(), 0, false);
	snapshot->guides = GetDenoisingGuideSnapshot();
	if (denoisingInputReduction > 1u) {
		snapshot->targetHoldout.resize(pixels);
		for (size_t i = 0; i < pixels; ++i)
			snapshot->targetHoldout[i] = snapshot->guides[i * DENOISING_GUIDE_STRIDE + 7] == 5.f;
	}
	snapshot->complete = std::all_of(snapshot->sampleCount.begin(), snapshot->sampleCount.end(), [](const auto n) {return n == 1u;});
	snapshot->motionCameraCompatible =
		camera.base.motionSystem.interpolatedTransformFirstIndex == NULL_INDEX &&
		((camera.type == slg::ocl::PERSPECTIVE && camera.persp.projCamera.lensRadius <= 0.f && !camera.persp.enableOculusRiftBarrel) ||
		 (camera.type == slg::ocl::ORTHOGRAPHIC && camera.ortho.projCamera.lensRadius <= 0.f));
	static_assert(sizeof(snapshot->cameraToWorld) == sizeof(camera.base.cameraToWorld.m));
	std::memcpy(snapshot->cameraToWorld, &camera.base.cameraToWorld.m, sizeof(snapshot->cameraToWorld));
	std::memcpy(snapshot->worldToCamera, &camera.base.cameraToWorld.mInv, sizeof(snapshot->worldToCamera));
	std::memcpy(snapshot->cameraToRaster, &camera.base.rasterToCamera.mInv, sizeof(snapshot->cameraToRaster));
	snapshot->completedTime = WallClockTime();
	{
		std::unique_lock<std::mutex> lock(*filmMutex);
		const Film &source = GetFilm();
		// LightMix belongs to the input HDR merge and must follow hot edits.
		// The later presentation path applies plugins without merging/scaling
		// this already merged HDR a second time.
		denoisingFrameFilm->CopyRadianceChannelScales(source);
		// Final delivery needs the AOV/alpha context of the same accumulated
		// raw prefix. Merge each physical frame exactly once, as in latest mode.
		if (denoisingImagePipelineAccumulated)
			GetFilm().AddFilm(*denoisingFrameFilm);
		if (denoisingImagePipelineRevision != source.GetImagePipelineRevision()) {
			denoisingImagePipelineRevision = source.GetImagePipelineRevision();
			++denoisingSceneEpoch;
		}
		snapshot->sceneEpoch = denoisingSceneEpoch;
		snapshot->presentationRevision = denoisingImagePipelineRevision;
		if (denoisingImagePipelineEnabled) {
			try {
				if (source.GetImagePipelineCount() == 0)
					throw runtime_error("Coherent presentation requires a primary image pipeline");
				const auto *primary = source.GetImagePipeline(0);
				Film::FilmChannels selected = source.GetChannels();
				const auto &plugins = primary->GetPlugins();
				// Hardware-capable plugins declare every auxiliary channel they
				// read for upload. Use that same contract only when every plugin
				// also has a native implementation. CPU-only/unknown pipelines
				// conservatively retain all raw channels.
				const bool primaryOnly = std::all_of(plugins.begin(), plugins.end(), [](const auto *plugin) {
						return plugin->CanUseHW() && plugin->CanUseNative();
					});
				if (primaryOnly) {
					selected = {Film::RADIANCE_PER_PIXEL_NORMALIZED, Film::RADIANCE_PER_SCREEN_NORMALIZED,
						Film::ALPHA, Film::IMAGEPIPELINE};
					primary->AddHWChannelsUsed(selected);
				}
				const size_t extraAlpha = source.HasChannel(Film::ALPHA) ? 0 : pixels * 2 * sizeof(float);
				const size_t extraImage = source.HasChannel(Film::IMAGEPIPELINE) ? 0 : pixels * 3 * sizeof(float);
				const size_t channelBytes = source.GetChannelStorageSize(selected,
					primaryOnly ? 1 : source.GetImagePipelineCount()) + extraAlpha + extraImage;
				const size_t coverageBytes = denoisingInputReduction > 1u ? pixels * (4 * sizeof(float) + sizeof(uint8_t)) : 0;
				if (channelBytes + coverageBytes > denoisingImagePipelineMaxBytes)
					throw runtime_error("Coherent image-pipeline channels exceed rtpath.denoisingframes.imagepipeline.maxbytes");
				auto context = Film::Create(source.GetWidth(), source.GetHeight(), source.GetSubRegion());
				context->CopyDynamicSettings(source);
				// Only pipeline zero presents reconstructed beauty. Raw AOVs and
				// their other image pipelines stay intact in the canonical film.
				if (primaryOnly) context->SetImagePipelines(primary->Copy());
				for (const auto channel: source.GetChannels())
					if (!selected.count(channel)) context->RemoveChannel(channel);
				context->AddChannel(Film::ALPHA);
				context->AddChannel(Film::IMAGEPIPELINE);
				context->hwEnable = false;
				context->GetDenoiser().SetEnabled(false);
				context->Init();
				if (context->GetChannelStorageSize() > channelBytes)
					throw runtime_error("Coherent presentation storage exceeded its preflight estimate");
				const Film &contextSource = denoisingImagePipelineAccumulated ? source : *denoisingFrameFilm;
				context->SetFilm(contextSource);
				if (!contextSource.HasChannel(Film::ALPHA)) {
					float *alpha = context->channel_ALPHA->GetPixels();
					std::fill(alpha, alpha + pixels * 2, 1.f);
				}
				snapshot->presentationChannelBytes = context->GetChannelStorageSize();
				if (denoisingInputReduction > 1u) {
					snapshot->targetAlpha.resize(pixels);
					context->GetOutput<float>(FilmOutputs::ALPHA, snapshot->targetAlpha.data(), 0, false);
					if (std::any_of(snapshot->targetHoldout.begin(), snapshot->targetHoldout.end(), [](const auto n) { return n != 0; })) {
						snapshot->targetHoldoutRGB.resize(pixels * 3);
						context->GetOutput<float>(FilmOutputs::RGB, snapshot->targetHoldoutRGB.data(), 0, false);
					}
				}
				snapshot->presentationFilm = std::shared_ptr<Film>(std::move(context));
			} catch (const std::exception &e) {
				// Display allocation failure must leave canonical rendering alive.
				snapshot->presentationError = e.what();
			}
		}
		denoisingFrameFilm->GetOutput<float>(FilmOutputs::RGBA, snapshot->color.data(), 0, false);
		if (!denoisingImagePipelineAccumulated)
			GetFilm().AddFilm(*denoisingFrameFilm);
		snapshot->presentationAccumulated = denoisingImagePipelineAccumulated;
		snapshot->accumulatedEyeSamples = source.GetTotalEyeSampleCount() / double(pixels);
	}
	denoisingFrameFilm->Reset();
	if (denoisingInputReduction > 1u) {
		// Keep the exact GPU-traced target coverage and raw AOV context. The
		// source is an actual subgrid of those rays, never interpolated colour,
		// normals, depth or visibility. Rendering workload stays full-resolution.
		const unsigned reduction = denoisingInputReduction;
		const unsigned w = snapshot->width / reduction, h = snapshot->height / reduction;
		std::vector<float> color(size_t(w) * h * 4), guides(size_t(w) * h * DENOISING_GUIDE_STRIDE);
		std::vector<unsigned> counts(size_t(w) * h);
		for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
			const size_t source = size_t(y * reduction + reduction / 2) * snapshot->width + x * reduction + reduction / 2;
			const size_t target = size_t(y) * w + x;
			std::copy_n(snapshot->color.data() + source * 4, 4, color.data() + target * 4);
			std::copy_n(snapshot->guides.data() + source * DENOISING_GUIDE_STRIDE, DENOISING_GUIDE_STRIDE,
					guides.data() + target * DENOISING_GUIDE_STRIDE);
			guides[target * DENOISING_GUIDE_STRIDE + 16] = x;
			guides[target * DENOISING_GUIDE_STRIDE + 17] = y;
			counts[target] = snapshot->sampleCount[source];
		}
		snapshot->color.swap(color); snapshot->guides.swap(guides); snapshot->sampleCount.swap(counts);
		// A selected child centre is +.25 source pixels at reduction2. NGX
		// receives this actual offset; its projection matrix stays unjittered.
		snapshot->jitterX = (snapshot->jitterX + reduction / 2 + .5f) / reduction - .5f;
		snapshot->jitterY = (snapshot->jitterY + reduction / 2 + .5f) / reduction - .5f;
		for (unsigned i = 0; i < 8; ++i) snapshot->cameraToRaster[i] /= reduction;
		snapshot->width = w; snapshot->height = h;
	}
	{
		std::lock_guard<std::mutex> lock(denoisingFrameMutex);
		// Parse can invalidate the display settings between the film merge
		// and publication. Never relabel that old snapshot as the new look.
		if (snapshot->presentationRevision == denoisingRequiredPipelineRevision)
			latestDenoisingFrame = std::move(snapshot);
	}
	denoisingFrameCondition.notify_all();
}

void RTPathOCLRenderEngine::AdvanceDenoisingFrameJitter() {
	const double n = double(denoisingFrameId);
	taskConfig.pathTracer.pixelJitterX = std::fmod(n * .7548776662466927 + .5, 1.) - .5;
	taskConfig.pathTracer.pixelJitterY = std::fmod(n * .5698402909980532 + .5, 1.) - .5;
	for (auto *baseThread : renderOCLThreads) {
		auto *thread = static_cast<RTPathOCLRenderThread *>(baseThread);
		thread->threadTaskConfig.pathTracer.pixelJitterEnable = 1;
		thread->threadTaskConfig.scene = taskConfig.scene;
		thread->threadTaskConfig.pathTracer.pixelJitterX = taskConfig.pathTracer.pixelJitterX;
		thread->threadTaskConfig.pathTracer.pixelJitterY = taskConfig.pathTracer.pixelJitterY;
		thread->intersectionDevice.PushThreadCurrentDevice();
		thread->intersectionDevice.EnqueueWriteBuffer(thread->taskConfigBuff, true,
				sizeof(thread->threadTaskConfig), &thread->threadTaskConfig);
		thread->intersectionDevice.PopThreadCurrentDevice();
	}
}

bool RTPathOCLRenderEngine::HasDone() const {
	return denoisingFramesEnabled ? denoisingFrameHalt.load() : TilePathOCLRenderEngine::HasDone();
}

void RTPathOCLRenderEngine::WaitForDone() const {
	if (!denoisingFramesEnabled) { TilePathOCLRenderEngine::WaitForDone(); return; }
	std::unique_lock<std::mutex> lock(denoisingFrameMutex);
	denoisingFrameCondition.wait(lock, [&] { return denoisingFramesStopped || denoisingFrameHalt.load(); });
}

void RTPathOCLRenderEngine::WaitNewFrame() {
	// Avoid to move forward rendering threads if I'm in pause
	if (!pauseMode) {
		// Update the statistics
		UpdateCounters();
	}
}

//------------------------------------------------------------------------------
// Static methods used by RenderEngineRegistry
//------------------------------------------------------------------------------

PropertiesUPtr RTPathOCLRenderEngine::ToProperties(const Properties &cfg) {
	auto props_ptr = std::make_unique<Properties>();
	auto& props = *props_ptr;

	props <<
			TilePathOCLRenderEngine::ToProperties(cfg) <<
			//------------------------------------------------------------------
			// Overwrite some TilePathOCLRenderEngine property
			//------------------------------------------------------------------
			cfg.Get(GetDefaultProps()->Get("renderengine.type")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.total")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.diffuse")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.glossy")) <<
			cfg.Get(GetDefaultProps()->Get("path.pathdepth.specular")) <<
			cfg.Get(GetDefaultProps()->Get("tilepath.sampling.aa.size")) <<
			cfg.Get(GetDefaultProps()->Get("tilepathocl.devices.maxtiles")) <<
			//------------------------------------------------------------------
			cfg.Get(GetDefaultProps()->Get("rtpath.resolutionreduction.preview")) <<
			cfg.Get(GetDefaultProps()->Get("rtpath.resolutionreduction.preview.step")) <<
			cfg.Get(GetDefaultProps()->Get("rtpath.resolutionreduction")) <<
			cfg.Get(GetDefaultProps()->Get("rtpath.denoisingframes.input.reduction")) <<
			cfg.Get(GetDefaultProps()->Get("rtpath.interactive.gpu"));
	return props_ptr;
}

RenderEngine *RTPathOCLRenderEngine::FromProperties(RenderConfigRef rcfg) {
	return new RTPathOCLRenderEngine(rcfg);
}

PropertiesUPtr RTPathOCLRenderEngine::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			TilePathOCLRenderEngine::GetDefaultProps() <<
			//------------------------------------------------------------------
			// Overwrite some TilePathOCLRenderEngine property
			//------------------------------------------------------------------
			Property("renderengine.type")(GetObjectTag()) <<
			Property("path.pathdepth.total")(5) <<
			Property("path.pathdepth.diffuse")(3) <<
			Property("path.pathdepth.glossy")(3) <<
			Property("path.pathdepth.specular")(3) <<
			Property("tilepath.sampling.aa.size")(1) <<
			Property("tilepathocl.devices.maxtiles")(1) <<
			//------------------------------------------------------------------
			Property("rtpath.resolutionreduction.preview")(4) <<
			Property("rtpath.resolutionreduction.preview.step")(8) <<
			Property("rtpath.resolutionreduction")(4) <<
			Property("rtpath.denoisingframes.input.reduction")(1u) <<
			Property("rtpath.interactive.gpu")(false);

	return props;
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
