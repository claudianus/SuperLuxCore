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

#ifndef _SLG_PATHOCLTHREADBASE_H
#define	_SLG_PATHOCLTHREADBASE_H

#if !defined(LUXRAYS_DISABLE_OPENCL)

#include "luxrays/core/intersectiondevice.h"
#include "luxrays/utils/ocl.h"
#include "luxrays/utils/thread.h"
#include "luxrays/usings.h"

#include "slg/slg.h"
#include "slg/engines/oclrenderengine.h"
#include "slg/engines/pathoclbase/compiledscene.h"

namespace slg {

//------------------------------------------------------------------------------
// OpenCL data types
//------------------------------------------------------------------------------

namespace ocl { namespace pathoclbase {
#include "slg/engines/pathoclbase/kernels/pathoclbase_datatypes.cl"
} }


class PathOCLBaseRenderEngine;

// Number of per-state wavefront task queues: mirrors the PathState
// enum in pathoclbase_datatypes.cl (MK_* states 0..11)
inline constexpr u_int WAVEFRONT_NUM_STATES = 12;
// Spectral hero-wavelength buckets per state queue (B2/E3 M2). Matches
// SLG_SPECTRAL_BINS (3 spectral bins ride in the float3 channels).
// Non-spectral builds bucket everything into lambda 0, which reproduces
// the M1 flat queue layout exactly.
inline constexpr u_int WAVEFRONT_NUM_LAMBDA = 3;

//------------------------------------------------------------------------------
// Path Tracing GPU-only render threads
// (base class for all types of OCL path tracers)
//------------------------------------------------------------------------------

class PathOCLBaseOCLRenderThread {
public:
	PathOCLBaseOCLRenderThread(const u_int index, luxrays::HardwareIntersectionDeviceRef device,
			PathOCLBaseRenderEngine *re);
	virtual ~PathOCLBaseOCLRenderThread();

	virtual void Start();
	virtual void Interrupt();
	virtual void Stop();

	virtual void BeginSceneEdit();
	virtual void EndSceneEdit(const EditActionList &editActions);

	virtual bool HasDone() const;
	virtual void WaitForDone() const;

	friend class PathOCLBaseRenderEngine;

	class ThreadFilm {
	public:
		ThreadFilm(PathOCLBaseOCLRenderThread *renderThread);
		virtual ~ThreadFilm();

		void Init(FilmRef engineFilm,
			const u_int threadFilmWidth, const u_int threadFilmHeight,
			const u_int *threadFilmSubRegion);
		void FreeAllOCLBuffers();
		u_int SetFilmKernelArgs(
			luxrays::HardwareIntersectionDeviceRef intersectionDevice,
			luxrays::HardwareDeviceKernelRPtr filmClearKernel,
			u_int argIndex) const;
		void ClearFilm(luxrays::HardwareIntersectionDeviceRef intersectionDevice,
			luxrays::HardwareDeviceKernelRPtr filmClearKernel,
			const size_t filmClearWorkGroupSize);
		void RecvFilm(luxrays::HardwareIntersectionDeviceRef intersectionDevice);
		void SendFilm(luxrays::HardwareIntersectionDeviceRef intersectionDevice);

		FilmRef GetFilm() { return *film; }
		FilmConstRef GetFilm() const { return *film; }

		// Film buffers
		std::vector<luxrays::HardwareDeviceBuffer *> channel_RADIANCE_PER_PIXEL_NORMALIZEDs_Buff;
		luxrays::HardwareDeviceBuffer *channel_ALPHA_Buff;
		luxrays::HardwareDeviceBuffer *channel_DEPTH_Buff;
		luxrays::HardwareDeviceBuffer *channel_POSITION_Buff;
		luxrays::HardwareDeviceBuffer *channel_GEOMETRY_NORMAL_Buff;
		luxrays::HardwareDeviceBuffer *channel_SHADING_NORMAL_Buff;
		luxrays::HardwareDeviceBuffer *channel_MATERIAL_ID_Buff;
		luxrays::HardwareDeviceBuffer *channel_DIRECT_DIFFUSE_Buff;
		luxrays::HardwareDeviceBuffer *channel_DIRECT_DIFFUSE_REFLECT_Buff;
		luxrays::HardwareDeviceBuffer *channel_DIRECT_DIFFUSE_TRANSMIT_Buff;
		luxrays::HardwareDeviceBuffer *channel_DIRECT_GLOSSY_Buff;
		luxrays::HardwareDeviceBuffer *channel_DIRECT_GLOSSY_REFLECT_Buff;
		luxrays::HardwareDeviceBuffer *channel_DIRECT_GLOSSY_TRANSMIT_Buff;
		luxrays::HardwareDeviceBuffer *channel_EMISSION_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_DIFFUSE_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_DIFFUSE_REFLECT_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_DIFFUSE_TRANSMIT_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_GLOSSY_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_GLOSSY_REFLECT_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_GLOSSY_TRANSMIT_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_SPECULAR_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_SPECULAR_REFLECT_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_SPECULAR_TRANSMIT_Buff;
		luxrays::HardwareDeviceBuffer *channel_MATERIAL_ID_MASK_Buff;
		luxrays::HardwareDeviceBuffer *channel_DIRECT_SHADOW_MASK_Buff;
		luxrays::HardwareDeviceBuffer *channel_INDIRECT_SHADOW_MASK_Buff;
		luxrays::HardwareDeviceBuffer *channel_UV_Buff;
		luxrays::HardwareDeviceBuffer *channel_RAYCOUNT_Buff;
		luxrays::HardwareDeviceBuffer *channel_BY_MATERIAL_ID_Buff;
		luxrays::HardwareDeviceBuffer *channel_IRRADIANCE_Buff;
		luxrays::HardwareDeviceBuffer *channel_OBJECT_ID_Buff;
		luxrays::HardwareDeviceBuffer *channel_OBJECT_ID_MASK_Buff;
		luxrays::HardwareDeviceBuffer *channel_BY_OBJECT_ID_Buff;
		luxrays::HardwareDeviceBuffer *channel_SAMPLECOUNT_Buff;
		luxrays::HardwareDeviceBuffer *channel_CONVERGENCE_Buff;
		luxrays::HardwareDeviceBuffer *channel_MATERIAL_ID_COLOR_Buff;
		luxrays::HardwareDeviceBuffer *channel_ALBEDO_Buff;
		luxrays::HardwareDeviceBuffer *channel_AVG_SHADING_NORMAL_Buff;
		luxrays::HardwareDeviceBuffer *channel_NOISE_Buff;
		luxrays::HardwareDeviceBuffer *channel_USER_IMPORTANCE_Buff;
		
		// Denoiser sample accumulator buffers
		luxrays::HardwareDeviceBuffer *denoiser_NbOfSamplesImage_Buff;
		luxrays::HardwareDeviceBuffer *denoiser_SquaredWeightSumsImage_Buff;
		luxrays::HardwareDeviceBuffer *denoiser_MeanImage_Buff;
		luxrays::HardwareDeviceBuffer *denoiser_CovarImage_Buff;
		luxrays::HardwareDeviceBuffer *denoiser_HistoImage_Buff;

	private:
		FilmUPtr film;
		FilmPtr engineFilm;
		PathOCLBaseOCLRenderThread *renderThread;
	};

protected:
	// Implementation specific methods
	virtual void RenderThreadImpl(std::stop_token stop_token) = 0;
	virtual void GetThreadFilmSize(u_int *filmWidth, u_int *filmHeight, u_int *filmSubRegion) = 0;

	virtual void StartRenderThread();
	virtual void StopRenderThread();

	void IncThreadFilms();
	void ClearThreadFilms();
	void TransferThreadFilms(luxrays::HardwareIntersectionDeviceRef intersectionDevice);
	void FreeThreadFilmsOCLBuffers();
	void FreeThreadFilms();

	void InitRender();

	void InitFilm();
	void InitCamera();
	void InitGeometry();
	void InitImageMaps();
	void InitTextures();
	void InitMaterials();
	void InitSceneObjects();
	void InitLights();
	void InitPhotonGI();
	void InitGuide();
	// Path guiding (P1-3 M2b-2): drain GPU training records into the
	// CPU cache, swap a training round and re-upload the coarse chunks.
	// Called once per outer render iteration (device idle).
	void DrainGuide();
	void InitKernels();
	void InitGPUTaskBuffer();
	void InitSamplerSharedDataBuffer();
	void InitSamplesBuffer();
	void InitSampleDataBuffer();
	void InitSampleResultsBuffer();

	void SetInitKernelArgs(const u_int filmIndex);
	void SetAdvancePathsKernelArgs(luxrays::HardwareDeviceKernelRPtr advancePathsKernel, const u_int filmIndex, const u_int queueState = 0);
	void SetAllAdvancePathsKernelArgs(const u_int filmIndex);
	void SetKernelArgs();


	std::tuple<luxrays::HardwareDeviceKernelUPtr, size_t> CompileKernel(
		luxrays::HardwareIntersectionDeviceRef device,
		luxrays::HardwareDeviceProgramRef program,
		const std::string &name
	);

	void EnqueueAdvancePathsKernel();
	void EnqueueAdvancePathsWavefront();

	static luxrays::oclKernelCache *AllocKernelCache(const std::string &type);
	static void GetKernelParamters(std::vector<std::string> &params,
			luxrays::HardwareIntersectionDeviceRef intersectionDevice,
			const std::string renderEngineType,
			const float epsilonMin, const float epsilonMax,
			const bool spectralEnable);
	static std::string GetKernelSources();

	u_int threadIndex;
	luxrays::HardwareIntersectionDeviceRef intersectionDevice;
	PathOCLBaseRenderEngine *renderEngine;

	// OpenCL variables
	std::string kernelSrcHash;
	luxrays::HardwareDeviceKernelUPtr filmClearKernel;
	size_t filmClearWorkGroupSize;

	// Scene buffers
	luxrays::HardwareDeviceBuffer *materialsBuff;
	luxrays::HardwareDeviceBuffer *materialEvalOpsBuff;
	luxrays::HardwareDeviceBuffer *materialEvalStackBuff;
	luxrays::HardwareDeviceBuffer *texturesBuff;
	luxrays::HardwareDeviceBuffer *textureEvalOpsBuff;
	luxrays::HardwareDeviceBuffer *textureEvalStackBuff;
	luxrays::HardwareDeviceBuffer *meshIDBuff;
	luxrays::HardwareDeviceBuffer *meshDescsBuff;
	luxrays::HardwareDeviceBuffer *scnObjsBuff;
	luxrays::HardwareDeviceBuffer *lightsBuff;
	luxrays::HardwareDeviceBuffer *envLightIndicesBuff;
	luxrays::HardwareDeviceBuffer *lightsDistributionBuff;
	luxrays::HardwareDeviceBuffer *infiniteLightSourcesDistributionBuff;
	luxrays::HardwareDeviceBuffer *dlscAllEntriesBuff;
	luxrays::HardwareDeviceBuffer *dlscDistributionsBuff;
	luxrays::HardwareDeviceBuffer *dlscBVHNodesBuff;
	luxrays::HardwareDeviceBuffer *elvcAllEntriesBuff;
	luxrays::HardwareDeviceBuffer *elvcDistributionsBuff;
	luxrays::HardwareDeviceBuffer *elvcTileDistributionOffsetsBuff;
	luxrays::HardwareDeviceBuffer *elvcBVHNodesBuff;
	luxrays::HardwareDeviceBuffer *envLightDistributionsBuff;
	luxrays::HardwareDeviceBuffer *vertsBuff;
	luxrays::HardwareDeviceBuffer *normalsBuff;
	luxrays::HardwareDeviceBuffer *triNormalsBuff;
	luxrays::HardwareDeviceBuffer *uvsBuff;
	luxrays::HardwareDeviceBuffer *colsBuff;
	luxrays::HardwareDeviceBuffer *alphasBuff;
	luxrays::HardwareDeviceBuffer *vertexAOVBuff;
	luxrays::HardwareDeviceBuffer *triAOVBuff;
	luxrays::HardwareDeviceBuffer *trianglesBuff;
	luxrays::HardwareDeviceBuffer *interpolatedTransformsBuff;
	// Native curve primitives (Metal HWRT; dev-tools/metal_curve_design.md)
	luxrays::HardwareDeviceBuffer *curveCpsBuff;
	luxrays::HardwareDeviceBuffer *curveSegIndicesBuff;
	luxrays::HardwareDeviceBuffer *curveCpAttrsBuff;
	luxrays::HardwareDeviceBuffer *cameraBuff;
	luxrays::HardwareDeviceBuffer *cameraBokehDistributionBuff;
	luxrays::HardwareDeviceBuffer *lightIndexOffsetByMeshIndexBuff;
	luxrays::HardwareDeviceBuffer *lightIndexByTriIndexBuff;
	luxrays::HardwareDeviceBuffer *imageMapDescsBuff;
	std::vector<luxrays::HardwareDeviceBuffer *> imageMapsBuff;
	luxrays::HardwareDeviceBuffer *pgicRadiancePhotonsBuff;
	luxrays::HardwareDeviceBuffer *pgicRadiancePhotonsValuesBuff;
	luxrays::HardwareDeviceBuffer *pgicRadiancePhotonsBVHNodesBuff;
	luxrays::HardwareDeviceBuffer *pgicCausticPhotonsBuff;
	luxrays::HardwareDeviceBuffer *pgicCausticPhotonsBVHNodesBuff;
	// Path guiding (P1-3 M2b): 16 frozen coarse-table chunks (4224B each)
	luxrays::HardwareDeviceBuffer *guideChunkBuff[16];
	// Guiding stats (validation)
	luxrays::HardwareDeviceBuffer *guideDbgBuff;
	// Path guiding (P1-3 M2b-2): per-task training records (float4/task)
	luxrays::HardwareDeviceBuffer *guideRecBuff[16];

	// OpenCL task related buffers
	luxrays::HardwareDeviceBuffer *raysBuff;
	luxrays::HardwareDeviceBuffer *hitsBuff;
	luxrays::HardwareDeviceBuffer *taskConfigBuff;
	luxrays::HardwareDeviceBuffer *tasksBuff;
	luxrays::HardwareDeviceBuffer *tasksDirectLightBuff;
	luxrays::HardwareDeviceBuffer *tasksStateBuff;
	luxrays::HardwareDeviceBuffer *samplerSharedDataBuff;
	luxrays::HardwareDeviceBuffer *samplesBuff;
	luxrays::HardwareDeviceBuffer *sampleDataBuff;
	luxrays::HardwareDeviceBuffer *sampleResultsBuff;
	luxrays::HardwareDeviceBuffer *taskStatsBuff;
	luxrays::HardwareDeviceBuffer *eyePathInfosBuff;
	// ReSTIR DI per-pixel temporal reservoirs (filmWidth * filmHeight)
	luxrays::HardwareDeviceBuffer *restirReservoirsBuff;
	luxrays::HardwareDeviceBuffer *directLightVolInfosBuff;
	luxrays::HardwareDeviceBuffer *pixelFilterBuff;

	u_int initKernelArgsCount;
	std::string kernelsParameters;

	luxrays::JThreadUPtr renderThread;

	std::vector<std::shared_ptr<ThreadFilm> > threadFilms;

	// OpenCL kernels
	luxrays::HardwareDeviceKernelUPtr initSeedKernel;
	luxrays::HardwareDeviceKernelUPtr initKernel;
	size_t initWorkGroupSize;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_RT_NEXT_VERTEX;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_HIT_NOTHING;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_HIT_OBJECT;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_RT_DL;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_DL_ILLUMINATE;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_DL_SAMPLE_BSDF;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_MNEE_NEXT_VERTEX;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_GENERATE_NEXT_VERTEX_RAY;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_SPLAT_SAMPLE;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_NEXT_SAMPLE;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_MK_GENERATE_CAMERA_RAY;
	// Wavefront per-state task queues (B2/E3): BuildQueues refills the
	// queues once per iteration from the authoritative taskState->state;
	// BucketHistogram counts the per-(state, lambda) task population
	// first so the host can compute the lambda-segment bases.
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_BuildQueues;
	luxrays::HardwareDeviceKernelUPtr advancePathsKernel_BucketHistogram;
	size_t advancePathsWorkGroupSize;

	// Wavefront queues (B2/E3): taskQueueBuff holds
	// WAVEFRONT_NUM_STATES * taskCount uint task indices laid out flat
	// per state, with the M2 lambda-bucketed ordering inside each
	// segment. taskQueueCountBuff holds WAVEFRONT_NUM_STATES *
	// WAVEFRONT_NUM_LAMBDA counters (histogram output); taskQueueBaseBuff
	// holds the same-shaped per-(state, lambda) segment bases uploaded
	// by the host and consumed as atomic cursors by BuildQueues;
	// taskLambdaBuff caches the per-task lambda bucket written by the
	// histogram kernel. Allocated only when wavefrontQueues is enabled
	// (env LUXRAYS_WAVEFRONT_QUEUES=1).
	luxrays::HardwareDeviceBuffer *taskQueueBuff;
	luxrays::HardwareDeviceBuffer *taskQueueCountBuff;
	luxrays::HardwareDeviceBuffer *taskQueueBaseBuff;
	luxrays::HardwareDeviceBuffer *taskLambdaBuff;
	bool wavefrontQueues;
	// Host-side snapshot of the per-(state, lambda) queue counters,
	// refreshed by EnqueueAdvancePathsWavefront each iteration;
	// wavefrontQueueTotals caches the per-state sums used for launch
	// sizing.
	std::vector<u_int> wavefrontQueueCounts;
	std::vector<u_int> wavefrontQueueTotals;

	std::unique_ptr<slg::ocl::pathoclbase::GPUTaskStats[]> gpuTaskStats;

	bool started, editMode, threadDone;
};


// These usings cannot be gathered in slg/usings.h, as C++ does not allow
// forward declarations on nested classes
using ThreadFilmRPtr = std::shared_ptr<PathOCLBaseOCLRenderThread::ThreadFilm>;
using ThreadFilmConstPtr = std::shared_ptr<const PathOCLBaseOCLRenderThread::ThreadFilm>;

}

#endif

#endif	/* _SLG_PATHOCLTHREADBASE_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
