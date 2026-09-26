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

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <cstdio>
#include <vector>

#include "luxrays/core/geometry/transform.h"
#include "luxrays/utils/ocl.h"
#include "luxrays/devices/ocldevice.h"
#include "luxrays/kernels/kernels.h"

#include "luxcore/cfg.h"

#include "slg/slg.h"
#include "slg/kernels/kernels.h"
#include "slg/renderconfig.h"
#include "slg/engines/pathoclbase/pathoclbase.h"
#include "slg/samplers/sobol.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PathOCLBaseOCLRenderThread
//------------------------------------------------------------------------------

PathOCLBaseOCLRenderThread::PathOCLBaseOCLRenderThread(const u_int index,
		HardwareIntersectionDeviceRef device, PathOCLBaseRenderEngine *re) :
	intersectionDevice(device)
{
	threadIndex = index;
	renderEngine = re;

	renderThread = nullptr;
	started = false;
	editMode = false;
	threadDone = false;

	kernelSrcHash = "";
	filmClearKernel = nullptr;

	// Scene buffers
	materialsBuff = nullptr;
	materialEvalOpsBuff = nullptr;
	materialEvalStackBuff = nullptr;
	texturesBuff = nullptr;
	textureEvalOpsBuff = nullptr;
	textureEvalStackBuff = nullptr;
	meshDescsBuff = nullptr;
	meshIDBuff = nullptr;
	scnObjsBuff = nullptr;
	lightsBuff = nullptr;
	envLightIndicesBuff = nullptr;
	lightsDistributionBuff = nullptr;
	infiniteLightSourcesDistributionBuff = nullptr;
	dlscAllEntriesBuff = nullptr;
	dlscDistributionsBuff = nullptr;
	dlscBVHNodesBuff = nullptr;
	elvcAllEntriesBuff = nullptr;
	elvcDistributionsBuff = nullptr;
	elvcTileDistributionOffsetsBuff = nullptr;
	elvcBVHNodesBuff = nullptr;
	envLightDistributionsBuff = nullptr;
	vertsBuff = nullptr;
	normalsBuff = nullptr;
	triNormalsBuff = nullptr;
	uvsBuff = nullptr;
	colsBuff = nullptr;
	alphasBuff = nullptr;
	vertexAOVBuff = nullptr;
	triAOVBuff = nullptr;
	trianglesBuff = nullptr;
	interpolatedTransformsBuff = nullptr;
	curveCpsBuff = nullptr;
	curveSegIndicesBuff = nullptr;
	curveCpAttrsBuff = nullptr;
	cameraBuff = nullptr;
	cameraBokehDistributionBuff = nullptr;
	lightIndexOffsetByMeshIndexBuff = nullptr;
	lightIndexByTriIndexBuff = nullptr;
	imageMapDescsBuff = nullptr;
	pgicRadiancePhotonsBuff = nullptr;
	pgicRadiancePhotonsValuesBuff = nullptr;
	pgicRadiancePhotonsBVHNodesBuff = nullptr;
	pgicCausticPhotonsBuff = nullptr;
	pgicCausticPhotonsBVHNodesBuff = nullptr;
	guideDbgBuff = nullptr;
	for (u_int i = 0u; i < 16u; ++i)
		guideRecBuff[i] = nullptr;
	for (u_int i = 0u; i < 16u; ++i)
		guideChunkBuff[i] = nullptr;

	// OpenCL task related buffers
	raysBuff = nullptr;
	hitsBuff = nullptr;
	taskConfigBuff = nullptr;
	tasksBuff = nullptr;
	tasksDirectLightBuff = nullptr;
	tasksStateBuff = nullptr;
	samplerSharedDataBuff = nullptr;
	samplesBuff = nullptr;
	sampleDataBuff = nullptr;
	sampleResultsBuff = nullptr;
	taskStatsBuff = nullptr;
	eyePathInfosBuff = nullptr;
	restirReservoirsBuff = nullptr;
	directLightVolInfosBuff = nullptr;
	pixelFilterBuff = nullptr;

	// OpenCL kernels
	initSeedKernel = nullptr;
	initKernel = nullptr;
	advancePathsKernel_MK_RT_NEXT_VERTEX = nullptr;
	advancePathsKernel_MK_HIT_NOTHING = nullptr;
	advancePathsKernel_MK_HIT_OBJECT = nullptr;
	advancePathsKernel_MK_RT_DL = nullptr;
	advancePathsKernel_MK_DL_ILLUMINATE = nullptr;
	advancePathsKernel_MK_DL_SAMPLE_BSDF = nullptr;
	advancePathsKernel_MK_MNEE_NEXT_VERTEX = nullptr;
	advancePathsKernel_MK_GENERATE_NEXT_VERTEX_RAY = nullptr;
	advancePathsKernel_MK_SPLAT_SAMPLE = nullptr;
	advancePathsKernel_MK_NEXT_SAMPLE = nullptr;
	advancePathsKernel_MK_GENERATE_CAMERA_RAY = nullptr;

	initKernelArgsCount  = 0;

	gpuTaskStats = nullptr;
}

PathOCLBaseOCLRenderThread::~PathOCLBaseOCLRenderThread() {
	if (editMode)
		EndSceneEdit(EditActionList());
	if (started)
		Stop();

	FreeThreadFilms();
}

void PathOCLBaseOCLRenderThread::Start() {
	started = true;

	try {
		InitRender();
		StartRenderThread();
	} catch (...) {
		// Kernel compilation may have failed before any buffer existed:
		// leave the thread "not started" so the destructor does not run
		// the full teardown (film transfer/buffer frees) over a
		// half-initialized thread.
		started = false;
		throw;
	}
}

void PathOCLBaseOCLRenderThread::Interrupt() {
	if (renderThread)
		renderThread->request_stop();
}

void PathOCLBaseOCLRenderThread::Stop() {
	StopRenderThread();

	// Guiding stats (try/ok accumulators for validation)
	if (guideDbgBuff) {
		u_int counts[2] = {0u, 0u};
		intersectionDevice.EnqueueReadBuffer(guideDbgBuff, CL_TRUE, 2 * sizeof(u_int), counts);
		FILE *f = fopen("/tmp/pgcount.txt", "a");
		if (f) {
			fprintf(f, "try=%u ok=%u\n", counts[0], counts[1]);
			fclose(f);
		}
	}
	intersectionDevice.FreeBuffer(&pgicRadiancePhotonsBuff);
	intersectionDevice.FreeBuffer(&pgicRadiancePhotonsValuesBuff);
	intersectionDevice.FreeBuffer(&pgicRadiancePhotonsBVHNodesBuff);
	intersectionDevice.FreeBuffer(&pgicCausticPhotonsBuff);
	intersectionDevice.FreeBuffer(&pgicCausticPhotonsBVHNodesBuff);
	for (u_int i = 0u; i < 16u; ++i)
		intersectionDevice.FreeBuffer(&guideChunkBuff[i]);
	intersectionDevice.FreeBuffer(&guideDbgBuff);
	for (u_int i = 0u; i < 16u; ++i)
		intersectionDevice.FreeBuffer(&guideRecBuff[i]);

	// OpenCL task related buffers
	intersectionDevice.FreeBuffer(&raysBuff);
	intersectionDevice.FreeBuffer(&hitsBuff);
	intersectionDevice.FreeBuffer(&taskConfigBuff);
	intersectionDevice.FreeBuffer(&tasksBuff);
	intersectionDevice.FreeBuffer(&tasksDirectLightBuff);
	intersectionDevice.FreeBuffer(&tasksStateBuff);
	intersectionDevice.FreeBuffer(&samplerSharedDataBuff);
	intersectionDevice.FreeBuffer(&samplesBuff);
	intersectionDevice.FreeBuffer(&sampleDataBuff);
	intersectionDevice.FreeBuffer(&sampleResultsBuff);
	intersectionDevice.FreeBuffer(&taskStatsBuff);
	intersectionDevice.FreeBuffer(&eyePathInfosBuff);
	intersectionDevice.FreeBuffer(&restirReservoirsBuff);
	intersectionDevice.FreeBuffer(&directLightVolInfosBuff);
	intersectionDevice.FreeBuffer(&pixelFilterBuff);

	// Compiled-scene buffers. These are allocated by the Init*() functions
	// (InitRender()/EndSceneEdit()) and persist across scene edits, so they are
	// only released here at full teardown - otherwise every render leaks the
	// whole uploaded scene (geometry, materials, textures, lights, image maps).
	intersectionDevice.FreeBuffer(&vertsBuff);
	intersectionDevice.FreeBuffer(&normalsBuff);
	intersectionDevice.FreeBuffer(&triNormalsBuff);
	intersectionDevice.FreeBuffer(&uvsBuff);
	intersectionDevice.FreeBuffer(&colsBuff);
	intersectionDevice.FreeBuffer(&alphasBuff);
	intersectionDevice.FreeBuffer(&vertexAOVBuff);
	intersectionDevice.FreeBuffer(&triAOVBuff);
	intersectionDevice.FreeBuffer(&trianglesBuff);
	intersectionDevice.FreeBuffer(&interpolatedTransformsBuff);
	intersectionDevice.FreeBuffer(&curveCpsBuff);
	intersectionDevice.FreeBuffer(&curveSegIndicesBuff);
	intersectionDevice.FreeBuffer(&curveCpAttrsBuff);
	intersectionDevice.FreeBuffer(&meshDescsBuff);
	intersectionDevice.FreeBuffer(&meshIDBuff);
	intersectionDevice.FreeBuffer(&scnObjsBuff);
	intersectionDevice.FreeBuffer(&cameraBuff);
	intersectionDevice.FreeBuffer(&cameraBokehDistributionBuff);
	intersectionDevice.FreeBuffer(&materialsBuff);
	intersectionDevice.FreeBuffer(&materialEvalOpsBuff);
	intersectionDevice.FreeBuffer(&materialEvalStackBuff);
	intersectionDevice.FreeBuffer(&texturesBuff);
	intersectionDevice.FreeBuffer(&textureEvalOpsBuff);
	intersectionDevice.FreeBuffer(&textureEvalStackBuff);
	intersectionDevice.FreeBuffer(&lightsBuff);
	intersectionDevice.FreeBuffer(&envLightIndicesBuff);
	intersectionDevice.FreeBuffer(&lightIndexOffsetByMeshIndexBuff);
	intersectionDevice.FreeBuffer(&lightIndexByTriIndexBuff);
	intersectionDevice.FreeBuffer(&envLightDistributionsBuff);
	intersectionDevice.FreeBuffer(&lightsDistributionBuff);
	intersectionDevice.FreeBuffer(&infiniteLightSourcesDistributionBuff);
	intersectionDevice.FreeBuffer(&dlscAllEntriesBuff);
	intersectionDevice.FreeBuffer(&dlscDistributionsBuff);
	intersectionDevice.FreeBuffer(&dlscBVHNodesBuff);
	intersectionDevice.FreeBuffer(&elvcAllEntriesBuff);
	intersectionDevice.FreeBuffer(&elvcDistributionsBuff);
	intersectionDevice.FreeBuffer(&elvcTileDistributionOffsetsBuff);
	intersectionDevice.FreeBuffer(&elvcBVHNodesBuff);
	intersectionDevice.FreeBuffer(&imageMapDescsBuff);
	for (HardwareDeviceBuffer *&b : imageMapsBuff)
		intersectionDevice.FreeBuffer(&b);
	imageMapsBuff.clear();

	started = false;

	// Film is deleted in the destructor to allow image saving after
	// the rendering is finished
}

void PathOCLBaseOCLRenderThread::StartRenderThread() {
	threadDone = false;

	// Create the thread for the rendering
	renderThread = std::make_unique<luxrays::JThread>(
		std::bind_front(&PathOCLBaseOCLRenderThread::RenderThreadImpl, this)
	);

	SetThreadName(renderThread, "LxPathOCL_OCL");
}

void PathOCLBaseOCLRenderThread::StopRenderThread() {
	if (renderThread) {
		renderThread->request_stop();
		renderThread->join();
	}
}

void PathOCLBaseOCLRenderThread::BeginSceneEdit() {
	StopRenderThread();
}

void PathOCLBaseOCLRenderThread::EndSceneEdit(const EditActionList &editActions) {
	//--------------------------------------------------------------------------
	// Update OpenCL buffers
	//
	// Note: if you edit this, you have probably to edit
	// RTPathOCLRenderThread::UpdateOCLBuffers() too.
	//--------------------------------------------------------------------------

	CompiledScene *cscene = renderEngine->compiledScene;

	if (cscene->wasCameraCompiled) {
		// Update Camera
		InitCamera();
	}

	if (cscene->wasGeometryCompiled) {
		// Update Scene Geometry
		InitGeometry();
	}

	if (cscene->wasImageMapsCompiled) {
		// Update Image Maps
		InitImageMaps();
	}

	if (cscene->wasMaterialsCompiled) {
		// Update Scene Textures and Materials
		InitTextures();
		InitMaterials();
	}

	if (cscene->wasSceneObjectsCompiled) {
		// Update Mesh <=> Material relation
		InitSceneObjects();
	}

	if  (cscene->wasLightsCompiled) {
		// Update Scene Lights
		InitLights();
	}

	if (cscene->wasPhotonGICompiled) {
		// Update PhotonGI cache
		InitPhotonGI();
	}

	//--------------------------------------------------------------------------
	// Recompile Kernels if required
	//--------------------------------------------------------------------------

	// The following actions can require a kernel re-compilation:
	// - Dynamic code generation of textures and materials;
	// - Material types edit;
	// - Light types edit;
	// - Image types edit;
	// - Geometry type edit;
	// - etc.
	InitKernels();

	if (editActions.HasAnyAction()) {
		SetKernelArgs();

		//----------------------------------------------------------------------
		// Execute initialization kernels
		//----------------------------------------------------------------------

		// Clear the frame buffers
		ClearThreadFilms();
	}

	// Reset statistics in order to be more accurate
	intersectionDevice.ResetPerformaceStats();

	StartRenderThread();
}

bool PathOCLBaseOCLRenderThread::HasDone() const {
	return (renderThread == nullptr) || threadDone;
}

void PathOCLBaseOCLRenderThread::WaitForDone() const {
	if (renderThread)
		renderThread->join();
}

void PathOCLBaseOCLRenderThread::IncThreadFilms() {
	threadFilms.push_back(std::make_shared<ThreadFilm>(this));

	// Initialize the new thread film
	u_int threadFilmWidth, threadFilmHeight, threadFilmSubRegion[4];
	GetThreadFilmSize(&threadFilmWidth, &threadFilmHeight, threadFilmSubRegion);

	threadFilms.back()->Init(renderEngine->GetFilm(), threadFilmWidth, threadFilmHeight,
			threadFilmSubRegion);
}

void PathOCLBaseOCLRenderThread::ClearThreadFilms() {
	// Clear all thread films
	for(ThreadFilmRPtr threadFilm: threadFilms)
		threadFilm->ClearFilm(intersectionDevice, filmClearKernel, filmClearWorkGroupSize);
}

void PathOCLBaseOCLRenderThread::TransferThreadFilms(HardwareIntersectionDeviceRef intersectionDevice) {
	// Clear all thread films
	for(ThreadFilmRPtr threadFilm: threadFilms)
		threadFilm->RecvFilm(intersectionDevice);
}

void PathOCLBaseOCLRenderThread::FreeThreadFilmsOCLBuffers() {
	for(ThreadFilmRPtr threadFilm: threadFilms)
		threadFilm->FreeAllOCLBuffers();
}

void PathOCLBaseOCLRenderThread::FreeThreadFilms() {
	threadFilms.clear();
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
