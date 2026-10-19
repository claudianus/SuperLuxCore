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

#ifndef _SLG_RTPATHOCL_H
#define	_SLG_RTPATHOCL_H

#if !defined(LUXRAYS_DISABLE_OPENCL)

#include <deque>
#include <functional>
#include <exception>
#include "slg/engines/tilepathocl/tilepathocl.h"
#include "slg/film/denoiser/denoisingframe.h"
#include <condition_variable>

namespace slg {

class RTPathOCLRenderEngine;

//------------------------------------------------------------------------------
// Real-Time path tracing GPU-only render threads
//------------------------------------------------------------------------------

class RTPathOCLRenderThread : public TilePathOCLRenderThread {
public:
	RTPathOCLRenderThread(const u_int index, luxrays::HardwareIntersectionDeviceRef device,
			TilePathOCLRenderEngine *re);
	virtual ~RTPathOCLRenderThread();

	virtual void Interrupt();

	virtual void BeginSceneEdit();
	virtual void EndSceneEdit(const EditActionList &editActions);

	friend class RTPathOCLRenderEngine;

protected:
	virtual void RenderThreadImpl(std::stop_token stop_token);

	void UpdateOCLBuffers(const EditActionList &updateActions);
	void UpdateAllThreadsOCLBuffers();
	
	void UpdateCameraOCLBuffer();
	void UpdateAllCameraThreadsOCLBuffers();

	TileWork tileWork;
};

//------------------------------------------------------------------------------
// Real-Time path tracing 100% OpenCL render engine
//------------------------------------------------------------------------------

typedef enum {
	SYNCTYPE_NONE,
	SYNCTYPE_STOP,
	SYNCTYPE_ENDSCENEEDIT,
	SYNCTYPE_BEGINFILMEDIT
} RTPathOCLSyncType;


class RTPathOCLRenderEngine : public TilePathOCLRenderEngine {
public:
	RTPathOCLRenderEngine(RenderConfigRef cfg);
	virtual ~RTPathOCLRenderEngine();

	virtual RenderEngineType GetType() const { return GetObjectType(); }
	virtual std::string GetTag() const { return GetObjectTag(); }

	double GetFrameTime() const { return frameTime; }

	virtual void BeginSceneEdit() override;
	virtual void EndSceneEdit(const EditActionList &editActions);
	void SetViewportCameraMetadata(const luxrays::Properties &metadata);
	luxrays::PropertiesUPtr ReadViewportOutput(std::function<luxrays::PropertiesUPtr(Film &)> read);
	void ServiceViewportReadbacks();
	void CancelViewportReadbacks();

	void GetViewportCameraEpochs(uint64_t &requested, uint64_t &applied, uint64_t &metadata) const {
		requested = viewportRequestedEpoch.load();
		applied = viewportAppliedEpoch.load();
		metadata = viewportMetadataEpoch.load();
	}


	virtual void BeginFilmEdit();
	virtual void EndFilmEdit(FilmRef film, std::mutex *flmMutex);

	virtual void WaitNewFrame();
	bool HasDone() const override;
	void WaitForDone() const override;
	FilmRef GetDenoisingFrameFilm() { return denoisingFramesEnabled ? *denoisingFrameFilm : GetFilm(); }
	std::shared_ptr<const DenoisingFrameSnapshot> GetDenoisingFrame(const uint64_t afterId, const u_int timeoutMs);
	void AcknowledgeDenoisingFrame(const std::shared_ptr<const DenoisingFrameSnapshot> &frame, const bool keepLossless);
	void ReleaseDenoisingFrameDelivery();
	// Called by RenderSession::Parse with the session film lock held.
	void SetDenoisingFrameHaltSPP(const uint64_t samples);
	void InvalidateDenoisingImagePipeline(const uint64_t revision);
	void InitDenoisingFrameFilm();
	void PublishDenoisingFrame(const slg::ocl::Camera &camera);
	void AdvanceDenoisingFrameJitter();

	// Runtime resolution-reduction override (see RenderEngine). Just an
	// atomic store: render threads pick it up at the next frame boundary
	// and re-upload their taskConfig buffer.
	virtual void SetRuntimeResolutionReduction(const u_int reduction) {
		if (!denoisingFramesEnabled) runtimeResolutionReduction = reduction;
	}

	//--------------------------------------------------------------------------
	// Static methods used by RenderEngineRegistry
	//--------------------------------------------------------------------------

	static RenderEngineType GetObjectType() { return RTPATHOCL; }
	static std::string GetObjectTag() { return "RTPATHOCL"; }
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static RenderEngine *FromProperties(RenderConfigRef rcfg);

	friend class TilePathOCLRenderEngine;
	friend class RTPathOCLRenderThread;

	// Must be a power of 2
	u_int previewResolutionReduction, previewResolutionReductionStep;
	u_int resolutionReduction;

	// Requested runtime override of resolutionReduction; 0 means "use the
	// configured value". Only ever set from the host thread, only ever
	// read by render threads at frame boundaries.
	std::atomic<u_int> runtimeResolutionReduction;

    struct completion_t {
        void operator()() noexcept { }
    };

protected:
	void UpdateResolutionReduction();
	FilmUPtr denoisingFrameFilm;
	bool denoisingImagePipelineEnabled = false;
	bool denoisingImagePipelineAccumulated = false;
	unsigned denoisingInputReduction = 1;
	std::atomic<bool> denoisingLosslessDelivery{false};
	uint64_t denoisingExactHaltSPP = 0;
	uint64_t denoisingImagePipelineMaxBytes = 256ull * 1024 * 1024;
	uint64_t denoisingImagePipelineRevision = 0;
	uint64_t denoisingRequiredPipelineRevision = 0;
	mutable std::mutex denoisingFrameMutex;
	mutable std::condition_variable denoisingFrameCondition;
	std::shared_ptr<const DenoisingFrameSnapshot> latestDenoisingFrame;
	uint64_t denoisingFrameId = 0, denoisingSceneEpoch = 1;
	uint64_t denoisingAcknowledgedFrameId = 0;
	bool denoisingFramesStopped = true;
	std::atomic<bool> denoisingFrameHalt{false};
	static luxrays::PropertiesUPtr GetDefaultProps();

	virtual void InitGPUTaskConfiguration();
	virtual bool IsRTMode() const { return true; }

	virtual PathOCLBaseOCLRenderThread *CreateOCLThread(const u_int index,
			luxrays::HardwareIntersectionDeviceRef device);

	virtual void StartLockLess();
	virtual void StopLockLess();
	virtual void UpdateFilmLockLess();

	void PauseThreads();
	void ResumeThreads();

	EditActionList updateActions;
	bool useFastCameraEditPath, cameraIsUsingCustomBokeh;
	// Order: camera mutex, then film mutex. Snapshot readers take only film mutex.
	std::mutex viewportCameraMutex;
	std::atomic<uint64_t> viewportRequestedEpoch{0}, viewportAppliedEpoch{0};
	std::atomic<uint64_t> viewportMetadataEpoch{~uint64_t(0)};
	luxrays::Properties pendingViewportMetadata;
	uint64_t pendingViewportMetadataEpoch = ~uint64_t(0);
	uint64_t pendingViewportCameraEpoch = 0;
	void ApplyViewportCameraMetadataLockLess();
	struct ViewportReadRequest {
		std::function<luxrays::PropertiesUPtr(Film &)> read;
		luxrays::PropertiesUPtr result;
		std::exception_ptr error;
		std::mutex mutex;
		std::condition_variable condition;
		bool started = false, done = false, cancelled = false;
	};
	std::mutex viewportReadsMutex;
	std::deque<std::shared_ptr<ViewportReadRequest>> viewportReads;
	std::atomic<bool> viewportReadbackStopped{true}, viewportDemandReadback{false};



	// Used by RTPathOCLRenderEngine code to sync. with render thread 0
	std::barrier<completion_t> *syncBarrier;
	std::atomic<RTPathOCLSyncType> syncType;
	// True only after the start handshake with render thread 0 completed.
	// When Start() fails halfway (i.e. a kernel compilation error) the
	// render thread never enters its sync loop and a barrier wait inside
	// StopLockLess() would block forever.
	bool syncThreadsRunning;

	// Used by all render threads to sync.
	std::barrier<completion_t> *frameBarrier;

	std::atomic<double> frameTime;
};

}

#endif

#endif	/* _SLG_RTPATHOCL_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
