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

#include "luxcore/cfg.h"
#include "luxrays/core/geometry/transform.h"
#include "luxrays/core/color/spectral.h"
#include "luxrays/utils/ocl.h"
#include "luxrays/devices/ocldevice.h"
#include "luxrays/kernels/kernels.h"

#include "slg/slg.h"
#include "slg/kernels/kernels.h"
#include "slg/renderconfig.h"
#include "slg/engines/pathoclbase/pathoclbase.h"
#include "slg/samplers/sobol.h"
#include "slg/samplers/pmj02.h"
#include "slg/film/filters/filter.h"
#include "slg/film/filters/filterdistribution.h"
#include "slg/utils/pathinfo.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PathOCLBaseOCLRenderThread initialization methods
//------------------------------------------------------------------------------

void PathOCLBaseOCLRenderThread::InitFilm() {
	if (threadFilms.size() == 0)
		IncThreadFilms();

	u_int threadFilmWidth, threadFilmHeight, threadFilmSubRegion[4];
	GetThreadFilmSize(&threadFilmWidth, &threadFilmHeight, threadFilmSubRegion);

	for(ThreadFilmRPtr threadFilm: threadFilms)
		threadFilm->Init(renderEngine->GetFilm(), threadFilmWidth, threadFilmHeight,
			threadFilmSubRegion);
}

void PathOCLBaseOCLRenderThread::InitCamera() {
	CompiledScene *cscene = renderEngine->compiledScene;

	intersectionDevice.AllocBufferRO(&cameraBuff, &cscene->camera,
			sizeof(slg::ocl::Camera), "Camera");
	if (not cscene->cameraBokehDistribution.empty())
		intersectionDevice.AllocBufferRO(&cameraBokehDistributionBuff, cscene->cameraBokehDistribution.data(),
				cscene->cameraBokehDistributionSize, "CameraBokehDistribution");
	else
		intersectionDevice.FreeBuffer(&cameraBokehDistributionBuff);
}

void PathOCLBaseOCLRenderThread::InitGeometry() {
	CompiledScene *cscene = renderEngine->compiledScene;

	const BufferType memTypeFlags = renderEngine->ctx->GetUseOutOfCoreBuffers() ?
		((BufferType)(BUFFER_TYPE_READ_ONLY | BUFFER_TYPE_OUT_OF_CORE)) :
		BUFFER_TYPE_READ_ONLY;

	if (cscene->normals.size() > 0)
		intersectionDevice.AllocBuffer(&normalsBuff,
				memTypeFlags,
				&cscene->normals[0],
				sizeof(Normal) * cscene->normals.size(), "Normals");
	else
		intersectionDevice.FreeBuffer(&normalsBuff);

	if (cscene->uvs.size() > 0)
		intersectionDevice.AllocBuffer(&uvsBuff,
				memTypeFlags,
				&cscene->uvs[0],
				sizeof(UV) * cscene->uvs.size(), "UVs");
	else
		intersectionDevice.FreeBuffer(&uvsBuff);

	if (cscene->cols.size() > 0)
		intersectionDevice.AllocBuffer(&colsBuff,
				memTypeFlags,
				&cscene->cols[0],
				sizeof(Spectrum) * cscene->cols.size(), "Colors");
	else
		intersectionDevice.FreeBuffer(&colsBuff);

	if (cscene->alphas.size() > 0)
		intersectionDevice.AllocBuffer(&alphasBuff,
				memTypeFlags,
				&cscene->alphas[0],
				sizeof(float) * cscene->alphas.size(), "Alphas");
	else
		intersectionDevice.FreeBuffer(&alphasBuff);

	if (cscene->vertexAOVs.size() > 0)
		intersectionDevice.AllocBuffer(&vertexAOVBuff,
				memTypeFlags,
				&cscene->vertexAOVs[0],
				sizeof(float) * cscene->vertexAOVs.size(), "Vertex AOVs");
	else
		intersectionDevice.FreeBuffer(&vertexAOVBuff);

	if (cscene->triAOVs.size() > 0)
		intersectionDevice.AllocBuffer(&triAOVBuff,
				memTypeFlags,
				&cscene->triAOVs[0],
				sizeof(float) * cscene->triAOVs.size(), "Triangle AOVs");
	else
		intersectionDevice.FreeBuffer(&triAOVBuff);

	intersectionDevice.AllocBuffer(&triNormalsBuff,
			memTypeFlags,
			&cscene->triNormals[0],
			sizeof(Normal) * cscene->triNormals.size(), "Triangle normals");

	intersectionDevice.AllocBuffer(&vertsBuff,
			memTypeFlags,
			&cscene->verts[0],
			sizeof(Point) * cscene->verts.size(), "Vertices");

	intersectionDevice.AllocBuffer(&trianglesBuff,
			memTypeFlags,
			&cscene->tris[0],
			sizeof(Triangle) * cscene->tris.size(), "Triangles");

	if (cscene->interpolatedTransforms.size() > 0) {
		intersectionDevice.AllocBuffer(&interpolatedTransformsBuff,
				memTypeFlags,
				&cscene->interpolatedTransforms[0],
				sizeof(luxrays::ocl::InterpolatedTransform) * cscene->interpolatedTransforms.size(), "Interpolated transformations");
	} else
		intersectionDevice.FreeBuffer(&interpolatedTransformsBuff);

	// Native curve primitives (Metal HWRT): the three buffers are only
	// allocated when at least one mesh carries curve data; kernels must not
	// dereference them otherwise (no mesh then has curveSegsOffset set).
	if (!cscene->curveSegIndices.empty()) {
		intersectionDevice.AllocBuffer(&curveCpsBuff,
				memTypeFlags,
				&cscene->curveCps[0],
				sizeof(CurveControlPoint) * cscene->curveCps.size(), "Curve control points");
		intersectionDevice.AllocBuffer(&curveSegIndicesBuff,
				memTypeFlags,
				&cscene->curveSegIndices[0],
				sizeof(u_int) * cscene->curveSegIndices.size(), "Curve segment indices");
		intersectionDevice.AllocBuffer(&curveCpAttrsBuff,
				memTypeFlags,
				&cscene->curveCpAttrs[0],
				sizeof(CurveCpAttr) * cscene->curveCpAttrs.size(), "Curve cp attributes");
	} else {
		intersectionDevice.FreeBuffer(&curveCpsBuff);
		intersectionDevice.FreeBuffer(&curveSegIndicesBuff);
		intersectionDevice.FreeBuffer(&curveCpAttrsBuff);
	}

	intersectionDevice.AllocBufferRO(&meshDescsBuff, &cscene->meshDescs[0],
			sizeof(slg::ocl::ExtMesh) * cscene->meshDescs.size(), "Mesh description");
}

void PathOCLBaseOCLRenderThread::InitMaterials() {
	const size_t materialsCount = renderEngine->compiledScene->mats.size();
	intersectionDevice.AllocBufferRO(&materialsBuff, &renderEngine->compiledScene->mats[0],
			sizeof(slg::ocl::Material) * materialsCount, "Materials");

	intersectionDevice.AllocBufferRO(&materialEvalOpsBuff, &renderEngine->compiledScene->matEvalOps[0],
			sizeof(slg::ocl::MaterialEvalOp) * renderEngine->compiledScene->matEvalOps.size(), "Material evaluation ops");

	const u_int taskCount = renderEngine->taskCount;
	intersectionDevice.AllocBufferRW(&materialEvalStackBuff, 
			nullptr, sizeof(float) * renderEngine->compiledScene->maxMaterialEvalStackSize *
			taskCount, "Material evaluation stacks");

}

void PathOCLBaseOCLRenderThread::InitSceneObjects() {
	const BufferType memTypeFlags = renderEngine->ctx->GetUseOutOfCoreBuffers() ?
		((BufferType)(BUFFER_TYPE_READ_ONLY | BUFFER_TYPE_OUT_OF_CORE)) :
		BUFFER_TYPE_READ_ONLY;

	const u_int sceneObjsCount = renderEngine->compiledScene->sceneObjs.size();
	intersectionDevice.AllocBuffer(&scnObjsBuff, memTypeFlags,
			&renderEngine->compiledScene->sceneObjs[0],
			sizeof(slg::ocl::SceneObject) * sceneObjsCount, "Scene objects");
}

void PathOCLBaseOCLRenderThread::InitTextures() {
	const size_t texturesCount = renderEngine->compiledScene->texs.size();
	intersectionDevice.AllocBufferRO(&texturesBuff, &renderEngine->compiledScene->texs[0],
			sizeof(slg::ocl::Texture) * texturesCount, "Textures");

	intersectionDevice.AllocBufferRO(&textureEvalOpsBuff, &renderEngine->compiledScene->texEvalOps[0],
			sizeof(slg::ocl::TextureEvalOp) * renderEngine->compiledScene->texEvalOps.size(), "Texture evaluation ops");

	const u_int taskCount = renderEngine->taskCount;
	intersectionDevice.AllocBufferRW(&textureEvalStackBuff, 
			nullptr, sizeof(float) * renderEngine->compiledScene->maxTextureEvalStackSize *
			taskCount, "Texture evaluation stacks");

	// JH2019 spectral upsampling table (path.spectral.upsampling=jh2019):
	// upload the embedded coefficient table as a packed
	// [scale[res] | coeffs[9*res^3]] float buffer. The kernel pointer
	// doubles as the model switch (NULL = Smits basis). ~1.15 MB at
	// res=32 -- too large for __constant program scope, hence a
	// read-only __global buffer.
	if (renderEngine->pathTracer.spectralEnable &&
			renderEngine->pathTracer.spectralUpsamplingJH2019) {
		const u_int res = Spectral::JH2019TableRes();
		const u_int tableFloats = res + 9 * res * res * res;
		std::vector<float> table(tableFloats);
		std::copy_n(Spectral::JH2019TableScale(), res, table.begin());
		std::copy_n(Spectral::JH2019TableCoeffs(), 9 * res * res * res,
				table.begin() + res);
		intersectionDevice.AllocBufferRO(&spectralUpsamplingTableBuff,
				table.data(), tableFloats * sizeof(float),
				"JH2019 spectral upsampling table");
	} else
		intersectionDevice.FreeBuffer(&spectralUpsamplingTableBuff);
}

void PathOCLBaseOCLRenderThread::InitLights() {
	CompiledScene *cscene = renderEngine->compiledScene;

	intersectionDevice.AllocBufferRO(&lightsBuff, &cscene->lightDefs[0],
		sizeof(slg::ocl::LightSource) * cscene->lightDefs.size(), "Lights");
	if (cscene->envLightIndices.size() > 0) {
		intersectionDevice.AllocBufferRO(&envLightIndicesBuff, &cscene->envLightIndices[0],
				sizeof(u_int) * cscene->envLightIndices.size(), "Env. light indices");
	} else
		intersectionDevice.FreeBuffer(&envLightIndicesBuff);

	if (cscene->lightIndexOffsetByMeshIndex.size() > 0) {
		intersectionDevice.AllocBufferRO(&lightIndexOffsetByMeshIndexBuff, &cscene->lightIndexOffsetByMeshIndex[0],
			sizeof(u_int) * cscene->lightIndexOffsetByMeshIndex.size(), "Light offsets (Part I)");
	} else {
		intersectionDevice.FreeBuffer(&lightIndexOffsetByMeshIndexBuff);
	}
	if (cscene->lightIndexByTriIndex.size() > 0) {
		intersectionDevice.AllocBufferRO(&lightIndexByTriIndexBuff, &cscene->lightIndexByTriIndex[0],
			sizeof(u_int) * cscene->lightIndexByTriIndex.size(), "Light offsets (Part II)");
	} else {
		intersectionDevice.FreeBuffer(&lightIndexByTriIndexBuff);
	}


	if (cscene->envLightDistributions.size() > 0) {
		intersectionDevice.AllocBufferRO(&envLightDistributionsBuff, &cscene->envLightDistributions[0],
			sizeof(float) * cscene->envLightDistributions.size(), "Env. light distributions");
	} else
		intersectionDevice.FreeBuffer(&envLightDistributionsBuff);

	if (cscene->lightsDistributionSize > 0) {
		intersectionDevice.AllocBufferRO(
			&lightsDistributionBuff,
			cscene->lightsDistribution.data(),
			cscene->lightsDistributionSize,
			"LightsDistribution"
		);
	} else {
		intersectionDevice.FreeBuffer(&lightsDistributionBuff);
	}

	if (cscene->infiniteLightSourcesDistributionSize > 0) {
		intersectionDevice.AllocBufferRO(&infiniteLightSourcesDistributionBuff, cscene->infiniteLightSourcesDistribution.data(),
			cscene->infiniteLightSourcesDistributionSize, "InfiniteLightSourcesDistribution");
	} else {
		intersectionDevice.FreeBuffer(&infiniteLightSourcesDistributionBuff);
	}

	// GPU light tracing emit distribution (only needed when light tasks exist)
	if ((renderEngine->lightTaskCount > 0) && (cscene->emitLightsDistributionSize > 0)) {
		intersectionDevice.AllocBufferRO(&emitLightsDistributionBuff, cscene->emitLightsDistribution.data(),
			cscene->emitLightsDistributionSize, "EmitLightsDistribution");
	} else {
		intersectionDevice.FreeBuffer(&emitLightsDistributionBuff);
	}

	if (cscene->dlscAllEntries.size() > 0) {
		intersectionDevice.AllocBufferRO(&dlscAllEntriesBuff, &cscene->dlscAllEntries[0],
			cscene->dlscAllEntries.size() * sizeof(slg::ocl::DLSCacheEntry), "DLSC all entries");
		intersectionDevice.AllocBufferRO(&dlscDistributionsBuff, &cscene->dlscDistributions[0],
			cscene->dlscDistributions.size() * sizeof(float), "DLSC distributions table");
		intersectionDevice.AllocBufferRO(&dlscBVHNodesBuff, &cscene->dlscBVHArrayNode[0],
			cscene->dlscBVHArrayNode.size() * sizeof(luxrays::ocl::IndexBVHArrayNode), "DLSC BVH nodes");
	} else {
		intersectionDevice.FreeBuffer(&dlscAllEntriesBuff);
		intersectionDevice.FreeBuffer(&dlscDistributionsBuff);
		intersectionDevice.FreeBuffer(&dlscBVHNodesBuff);
	}
	
	if (cscene->elvcAllEntries.size() > 0) {
		intersectionDevice.AllocBufferRO(&elvcAllEntriesBuff, &cscene->elvcAllEntries[0],
			cscene->elvcAllEntries.size() * sizeof(slg::ocl::ELVCacheEntry), "ELVC all entries");
		intersectionDevice.AllocBufferRO(&elvcDistributionsBuff, &cscene->elvcDistributions[0],
			cscene->elvcDistributions.size() * sizeof(float), "ELVC distributions table");
		if (cscene->elvcTileDistributionOffsets.size() > 0) {
			intersectionDevice.AllocBufferRO(&elvcTileDistributionOffsetsBuff, &cscene->elvcTileDistributionOffsets[0],
					cscene->elvcTileDistributionOffsets.size() * sizeof(u_int), "ELVC tile distribution offsets table");
		} else
			intersectionDevice.FreeBuffer(&elvcTileDistributionOffsetsBuff);
		intersectionDevice.AllocBufferRO(&elvcBVHNodesBuff, &cscene->elvcBVHArrayNode[0],
			cscene->elvcBVHArrayNode.size() * sizeof(luxrays::ocl::IndexBVHArrayNode), "ELVC BVH nodes");
	} else {
		intersectionDevice.FreeBuffer(&elvcAllEntriesBuff);
		intersectionDevice.FreeBuffer(&elvcDistributionsBuff);
		intersectionDevice.FreeBuffer(&elvcTileDistributionOffsetsBuff);
		intersectionDevice.FreeBuffer(&elvcBVHNodesBuff);
	}
}

void PathOCLBaseOCLRenderThread::InitPhotonGI() {
	CompiledScene *cscene = renderEngine->compiledScene;

	const BufferType memTypeFlags = renderEngine->ctx->GetUseOutOfCoreBuffers() ?
		((BufferType)(BUFFER_TYPE_READ_ONLY | BUFFER_TYPE_OUT_OF_CORE)) :
		BUFFER_TYPE_READ_ONLY;

	if (cscene->pgicRadiancePhotons.size() > 0) {
		intersectionDevice.AllocBuffer(&pgicRadiancePhotonsBuff, memTypeFlags, &cscene->pgicRadiancePhotons[0],
			cscene->pgicRadiancePhotons.size() * sizeof(slg::ocl::RadiancePhoton), "PhotonGI indirect cache all entries");
		intersectionDevice.AllocBuffer(&pgicRadiancePhotonsValuesBuff, memTypeFlags, &cscene->pgicRadiancePhotonsValues[0],
			cscene->pgicRadiancePhotonsValues.size() * sizeof(slg::ocl::Spectrum), "PhotonGI indirect cache all entry values");
		intersectionDevice.AllocBuffer(&pgicRadiancePhotonsBVHNodesBuff, memTypeFlags, &cscene->pgicRadiancePhotonsBVHArrayNode[0],
			cscene->pgicRadiancePhotonsBVHArrayNode.size() * sizeof(luxrays::ocl::IndexBVHArrayNode), "PhotonGI indirect cache BVH nodes");
	} else {
		intersectionDevice.FreeBuffer(&pgicRadiancePhotonsBuff);
		intersectionDevice.FreeBuffer(&pgicRadiancePhotonsValuesBuff);
		intersectionDevice.FreeBuffer(&pgicRadiancePhotonsBVHNodesBuff);
	}

	if (cscene->pgicCausticPhotons.size() > 0) {
		intersectionDevice.AllocBuffer(&pgicCausticPhotonsBuff, memTypeFlags, &cscene->pgicCausticPhotons[0],
			cscene->pgicCausticPhotons.size() * sizeof(slg::ocl::Photon), "PhotonGI caustic cache all entries");
		intersectionDevice.AllocBuffer(&pgicCausticPhotonsBVHNodesBuff, memTypeFlags, &cscene->pgicCausticPhotonsBVHArrayNode[0],
			cscene->pgicCausticPhotonsBVHArrayNode.size() * sizeof(luxrays::ocl::IndexBVHArrayNode), "PhotonGI caustic cache BVH nodes");
	} else {
		intersectionDevice.FreeBuffer(&pgicCausticPhotonsBuff);
		intersectionDevice.FreeBuffer(&pgicCausticPhotonsBVHNodesBuff);
	}
}

void PathOCLBaseOCLRenderThread::InitGuide() {
	// Path guiding table chunks (16 x 4224B): small uploads land reliably
	if (renderEngine->guideHasTable && renderEngine->guideTable.size() > 0) {
		for (u_int i = 0u; i < 16u; ++i) {
			intersectionDevice.AllocBufferRO(&guideChunkBuff[i],
					&renderEngine->guideTable[i * 32u * 33u],
					32u * 33u * sizeof(float), "Path guiding table chunk");
		}
	} else {
		for (u_int i = 0u; i < 16u; ++i)
			intersectionDevice.FreeBuffer(&guideChunkBuff[i]);
	}

	// Path guiding (P1-3 M2b-2): 16 training-record buffers (4KB each,
	// 256 float4 records; 4KB is the reliable transfer size on this
	// backend). Zeroed at init so the drain only sees fresh writes.
	if (renderEngine->guideHasTable) {
		static float recZeros[1024] = { 0.f };
		for (u_int i = 0u; i < 16u; ++i) {
			intersectionDevice.AllocBufferRW(&guideRecBuff[i], nullptr,
				256u * 4u * sizeof(float), "Path guiding training records");
			intersectionDevice.EnqueueWriteBuffer(guideRecBuff[i], CL_TRUE,
					1024u * sizeof(float), recZeros);
		}
	} else {
		for (u_int i = 0u; i < 16u; ++i)
			intersectionDevice.FreeBuffer(&guideRecBuff[i]);
	}

	// Guiding stats (always allocated when guiding is on)
	if (renderEngine->guideHasTable) {
		u_int initVals[4] = {0u, 0u, 0u, 0u};
		intersectionDevice.AllocBuffer(&guideDbgBuff, BUFFER_TYPE_READ_WRITE, initVals,
			4 * sizeof(u_int), "Path guiding debug counters");
	} else {
		intersectionDevice.FreeBuffer(&guideDbgBuff);
	}
}

void PathOCLBaseOCLRenderThread::DrainGuide() {
	if (!renderEngine->guideHasTable || !renderEngine->guideCache)
		return;
	// Read back the 16 record buffers (first 1K floats = 256 records each;
	// reads above ~4KB silently fail on this backend) and apply to the
	// CPU-side write side (RecordBin validates + clamps).
	for (u_int b = 0u; b < 16u; ++b) {
		if (!guideRecBuff[b])
			continue;
		float rec[1024];
		intersectionDevice.EnqueueReadBuffer(guideRecBuff[b], CL_TRUE,
				1024u * sizeof(float), rec);
		for (u_int t = 0u; t < 256u; ++t) {
			const float *r = &rec[(size_t)t * 4u];
			if (!(r[3] > .5f && r[3] < 1.5f))
				continue;
			const u_int cell = (u_int)r[0];
			const u_int bin = (u_int)r[1];
			renderEngine->guideCache->RecordBin(cell, bin, r[2]);
		}

	}

	// New training round every 10 drains + re-upload coarse chunks
	// (small uploads land). 10 drains x 4K records warms the table.
	{
		static u_int drainCount = 0u;
		if (++drainCount >= 10u) {
			drainCount = 0u;
			renderEngine->guideCache->ForceSwap();
			std::vector<float> coarse;
			renderEngine->guideCache->SnapshotCoarseTable(&coarse);
			for (u_int i = 0u; i < 16u; ++i) {
				if (!guideChunkBuff[i])
					continue;
				intersectionDevice.EnqueueWriteBuffer(guideChunkBuff[i], CL_TRUE,
						32u * 33u * sizeof(float),
						&coarse[(size_t)i * 32u * 33u]);
			}
		}
	}
}

void PathOCLBaseOCLRenderThread::InitImageMaps() {
	CompiledScene *cscene = renderEngine->compiledScene;

	if (cscene->imageMapDescs.size() > 0) {
		intersectionDevice.AllocBufferRO(&imageMapDescsBuff,
				&cscene->imageMapDescs[0],
				sizeof(slg::ocl::ImageMap) * cscene->imageMapDescs.size(), "ImageMap descriptions");

		// Free unused pages
		for (u_int i = cscene->imageMapMemBlocks.size(); i < imageMapsBuff.size(); ++i)
			intersectionDevice.FreeBuffer(&imageMapsBuff[i]);
		imageMapsBuff.resize(cscene->imageMapMemBlocks.size(), NULL);

		const BufferType memTypeFlags = renderEngine->ctx->GetUseOutOfCoreBuffers() ?
			((BufferType)(BUFFER_TYPE_READ_ONLY | BUFFER_TYPE_OUT_OF_CORE)) :
			BUFFER_TYPE_READ_ONLY;

		for (u_int i = 0; i < imageMapsBuff.size(); ++i) {
			intersectionDevice.AllocBuffer(&(imageMapsBuff[i]),
					memTypeFlags,
					&(cscene->imageMapMemBlocks[i][0]),
					sizeof(float) * cscene->imageMapMemBlocks[i].size(), "ImageMaps");
		}
	} else {
		intersectionDevice.FreeBuffer(&imageMapDescsBuff);
		for (u_int i = 0; i < imageMapsBuff.size(); ++i)
			intersectionDevice.FreeBuffer(&imageMapsBuff[i]);
		imageMapsBuff.resize(0);
	}
}

void PathOCLBaseOCLRenderThread::InitGPUTaskBuffer() {
	const u_int taskCount = renderEngine->taskCount;

	//--------------------------------------------------------------------------
	// Allocate tasksConfigBuff
	//--------------------------------------------------------------------------

	// The per-pixel ReSTIR reservoir count (one slot per film pixel);
	// the spatial merge is screen-space (E2b), so no separate grid
	// region follows them.
	{
		const u_int *subRegion = renderEngine->GetFilm().GetSubRegion();
		renderEngine->taskConfig.pathTracer.restir.reservoirCount =
				(subRegion[3] + 1) * renderEngine->GetFilm().GetWidth();
	}

	// GPU light tracing (doc/features/gpu_lighttracing.md): light tasks
	// occupy the tail gids [eyeTaskCount, taskCount). Each light task
	// gets one extra ray slot in the tail region
	// rays[lightVisRayBase + (gid - eyeTaskCount)] for the
	// camera-visibility ray (dual-slot layout: one iteration per vertex).
	// The ReSTIR/GI tails start after that region.
	auto &lt = renderEngine->taskConfig.pathTracer.lightTracing;
	lt.eyeTaskCount = renderEngine->eyeTaskCount;
	lt.lightTaskCount = renderEngine->lightTaskCount;
	lt.lightVisRayBase = taskCount;
	const u_int rayTailBase = taskCount + renderEngine->lightTaskCount;

	// ReSTIR visibility-weighted target (E2a): the K candidate shadow
	// rays per task share the tail of raysBuff/hitsBuff starting at
	// taskCount, so a single EnqueueTraceRayBuffer pass over
	// taskCount * (1 + K) rays resolves them. The per-candidate records
	// (RestirReservoir aliasing) are appended to restirReservoirsBuff
	// after the per-pixel reservoirs.
	// K is capped at 8: candidate shadow rays are the dominant memory
	// cost (taskCount * K * (sizeof(Ray) + sizeof(RayHit))).
	if (renderEngine->taskConfig.pathTracer.restir.enabled &&
			renderEngine->taskConfig.pathTracer.restir.visibilityEnable) {
		auto &restir = renderEngine->taskConfig.pathTracer.restir;
		restir.visCandCount = Min(8u, restir.candidateCount);

		// Low-resource guard: the candidate tail costs
		// taskCount * (K + RESTIR_PIXEL_MERGES_MAX) *
		// (sizeof(Ray) + sizeof(RayHit)) of extra device memory plus the
		// same count of RestirVisCandidate records (the 2 merge slots
		// carry the visibility-aware spatial-shift rays, E2d). Cap K so
		// the tail stays under a fixed byte budget; on huge task counts
		// this trades visibility candidates for render stability.
		// All arithmetic in 64-bit: u_int products can overflow.
		const u_int candBytesPerTask =
				(u_int)(sizeof(Ray) + sizeof(RayHit) +
				sizeof(slg::ocl::pathoclbase::RestirVisCandidate));
		const size_t candByteBudget = (size_t)256 * 1024 * 1024;
		const u_int maxKByMem = (candBytesPerTask > 0 && taskCount > 0) ?
				(u_int)Max<long long>(0ll, (long long)Min<size_t>(8u +
				RESTIR_PIXEL_MERGES_MAX, candByteBudget /
				((size_t)taskCount * candBytesPerTask)) -
				RESTIR_PIXEL_MERGES_MAX) : 0u;
		if (restir.visCandCount > maxKByMem) {
			SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex <<
					"] ReSTIR visibility candidates clamped from " <<
					restir.visCandCount << " to " << maxKByMem <<
					" (taskCount=" << taskCount << ", budget=" <<
					candByteBudget / 1024 / 1024 << "MB)");
			restir.visCandCount = maxKByMem;
		}
		if (restir.visCandCount == 0u)
			restir.visibilityEnable = false;

		restir.visCandRayBase = rayTailBase;
		// Candidate records are appended right after the per-pixel
		// reservoirs (the world-space spatial grid was removed in E2b:
		// screen-space neighbour-pixel merge replaces it).
		restir.visCandDataOffset = restir.reservoirCount;
	} else {
		renderEngine->taskConfig.pathTracer.restir.visibilityEnable = false;
		renderEngine->taskConfig.pathTracer.restir.visCandCount = 0;
		renderEngine->taskConfig.pathTracer.restir.visCandRayBase = rayTailBase;
		renderEngine->taskConfig.pathTracer.restir.visCandDataOffset = 0;
	}

	// ReSTIR GI (G1 GPU): the GI state shares restirReservoirsBuff -
	// per-pixel GI reservoirs then per-task candidate/result records,
	// appended after the DI regions. The bounce + NEE candidate rays
	// ride a second tail of rays[]/rayHits[] (2K slots per task). All
	// offsets below are in RestirReservoir-slot units; byte sizes are
	// rounded up to whole slots like the DI candidate region.
	{
		namespace podt = slg::ocl::pathoclbase;
		auto &gi = renderEngine->taskConfig.pathTracer.restirGI;
		auto &restir = renderEngine->taskConfig.pathTracer.restir;
		const u_int reservoirCount = restir.reservoirCount;
		// Byte size of the DI candidate region that precedes the GI
		// data (0 when visibility is off).
		const size_t diCandBytes = (restir.visCandCount > 0u) ?
				(size_t)taskCount * (restir.visCandCount +
				RESTIR_PIXEL_MERGES_MAX) * sizeof(podt::RestirVisCandidate) : 0;
		const u_int diCandSlots = (u_int)((diCandBytes +
				sizeof(podt::RestirReservoir) - 1) /
				sizeof(podt::RestirReservoir));
		const u_int giReservoirSlots = (u_int)(((size_t)reservoirCount *
				sizeof(podt::RestirGIReservoir) +
				sizeof(podt::RestirReservoir) - 1) /
				sizeof(podt::RestirReservoir));

		if (gi.enabled) {
			gi.reservoirCount = reservoirCount;
			gi.giCandCount = Min(4u, gi.candidateCount);

			// Low-resource guard, same pattern as the DI tail: the GI
			// tail costs taskCount * (2K rays+hits) plus taskCount *
			// (K RestirGICandidate + 1 RestirGIResult) bytes of
			// reservoir buffer. Cap K so the extra stays under a fixed
			// budget; all arithmetic in 64-bit (u_int products can
			// overflow at 512K tasks).
			const u_int bytesPerCand =
					(u_int)(2u * (sizeof(Ray) + sizeof(RayHit)) +
					sizeof(podt::RestirGICandidate));
			const size_t giByteBudget = (size_t)256 * 1024 * 1024;
			const u_int maxKByMem = (bytesPerCand > 0 && taskCount > 0) ?
					(u_int)Max<long long>(0ll, (long long)Min<size_t>(
					4u, (giByteBudget - (size_t)taskCount *
					sizeof(podt::RestirGIResult)) /
					((size_t)taskCount * bytesPerCand))) : 0u;
			if (gi.giCandCount > maxKByMem) {
				SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex <<
						"] ReSTIR GI candidates clamped from " <<
						gi.giCandCount << " to " << maxKByMem <<
						" (taskCount=" << taskCount << ", budget=" <<
						giByteBudget / 1024 / 1024 << "MB)");
				gi.giCandCount = maxKByMem;
			}
			if (gi.giCandCount == 0u)
				gi.enabled = false;
		}
		if (gi.enabled) {
			// The GI ray tail starts right after the DI tail
			gi.giCandRayBase = rayTailBase + taskCount *
					((restir.visCandCount > 0u) ?
					(restir.visCandCount + RESTIR_PIXEL_MERGES_MAX) : 0u);
			gi.giReservoirOffset = reservoirCount + diCandSlots;
			gi.giCandDataOffset = gi.giReservoirOffset + giReservoirSlots;
			// Per-task record block: K candidates + 1 result record,
			// rounded up to whole reservoir slots
			gi.giCandStride = (u_int)(((size_t)gi.giCandCount *
					sizeof(podt::RestirGICandidate) +
					sizeof(podt::RestirGIResult) +
					sizeof(podt::RestirReservoir) - 1) /
					sizeof(podt::RestirReservoir));
		} else {
			gi.reservoirCount = 0;
			gi.giCandCount = 0;
			gi.giCandRayBase = rayTailBase + taskCount *
					((restir.visCandCount > 0u) ?
					(restir.visCandCount + RESTIR_PIXEL_MERGES_MAX) : 0u);
			gi.giReservoirOffset = reservoirCount + diCandSlots;
			gi.giCandDataOffset = gi.giReservoirOffset;
			gi.giCandStride = 0;
		}
	}

	intersectionDevice.AllocBufferRO(&taskConfigBuff, &renderEngine->taskConfig, sizeof(slg::ocl::pathoclbase::GPUTaskConfiguration), "GPUTaskConfiguration");

	//--------------------------------------------------------------------------
	// Allocate tasksBuff
	//--------------------------------------------------------------------------

	intersectionDevice.AllocBufferRW(&tasksBuff, nullptr, sizeof(slg::ocl::pathoclbase::GPUTask) * taskCount, "GPUTask");

	//--------------------------------------------------------------------------
	// Allocate tasksDirectLightBuff
	//--------------------------------------------------------------------------

	intersectionDevice.AllocBufferRW(&tasksDirectLightBuff, nullptr, sizeof(slg::ocl::pathoclbase::GPUTaskDirectLight) * taskCount, "GPUTaskDirectLight");

	//--------------------------------------------------------------------------
	// Allocate tasksStateBuff
	//--------------------------------------------------------------------------

	intersectionDevice.AllocBufferRW(&tasksStateBuff, nullptr, sizeof(slg::ocl::pathoclbase::GPUTaskState) * taskCount, "GPUTaskState");

	//--------------------------------------------------------------------------
	// Allocate wavefront per-state task queues (B2/E3)
	//--------------------------------------------------------------------------

	if (wavefrontQueues) {
		intersectionDevice.AllocBufferRW(&taskQueueBuff, nullptr,
				sizeof(u_int) * WAVEFRONT_NUM_STATES * taskCount, "taskQueue");
		// M2 lambda bucketing: per-(state, lambda) histogram counters
		// and segment bases, plus the per-task lambda cache.
		intersectionDevice.AllocBufferRW(&taskQueueCountBuff, nullptr,
				sizeof(u_int) * WAVEFRONT_NUM_STATES * WAVEFRONT_NUM_LAMBDA,
				"taskQueueCount");
		intersectionDevice.AllocBufferRW(&taskQueueBaseBuff, nullptr,
				sizeof(u_int) * WAVEFRONT_NUM_STATES * WAVEFRONT_NUM_LAMBDA,
				"taskQueueBase");
		intersectionDevice.AllocBufferRW(&taskLambdaBuff, nullptr,
				sizeof(u_int) * taskCount, "taskLambda");
		wavefrontQueueCounts.assign(WAVEFRONT_NUM_STATES * WAVEFRONT_NUM_LAMBDA, 0u);
		wavefrontQueueTotals.assign(WAVEFRONT_NUM_STATES, 0u);
	}
}

void PathOCLBaseOCLRenderThread::InitSamplerSharedDataBuffer() {
	const u_int *subRegion = renderEngine->GetFilm().GetSubRegion();
	const u_int filmRegionPixelCount = (subRegion[1] - subRegion[0] + 1) * (subRegion[3] - subRegion[2] + 1);

	size_t size = 0;
	if (renderEngine->oclSampler->type == slg::ocl::RANDOM) {
		size += sizeof(slg::ocl::RandomSamplerSharedData);
	} else if (renderEngine->oclSampler->type == slg::ocl::METROPOLIS) {
		// Nothing
	} else if (renderEngine->oclSampler->type == slg::ocl::SOBOL) {
		size += sizeof(slg::ocl::SobolSamplerSharedData);

		// Plus the a pass field for each pixel
		size += sizeof(u_int) * filmRegionPixelCount;

		// Plus the Sobol directions array (light-path dims are shifted
		// by 2 - IDX_SCREEN_X/Y - so they need lightSampleSize + 2)
		const u_int sobolDimCount = (renderEngine->lightTaskCount > 0) ?
				Max(renderEngine->pathTracer.eyeSampleSize,
				renderEngine->pathTracer.lightSampleSize + 2) :
				renderEngine->pathTracer.eyeSampleSize;
		size += sizeof(u_int) * sobolDimCount * SOBOL_BITS;

		// Plus the Owen blue-noise scramble rank tile
		size += sizeof(u_int) * SOBOL_OWEN_TILE_SIZE * SOBOL_OWEN_TILE_SIZE;

		// Plus the per-pixel luma moments for adaptive sampling
		// (2 floats per pixel: luminance sum and sum of squares)
		size += sizeof(float) * 2 * filmRegionPixelCount;
	} else if (renderEngine->oclSampler->type == slg::ocl::PMJ02SAMPLER) {
		// Same header as Sobol (seedBase/bucketIndex/filmRegionPixelCount)
		size += sizeof(slg::ocl::SobolSamplerSharedData);

		// Plus the a pass field for each pixel
		size += sizeof(u_int) * filmRegionPixelCount;

		// Plus the PMJ02 tables (pairs x samples x xy floats)
		size += sizeof(float) * 2 *
				renderEngine->oclSampler->pmj02.tablePairs *
				renderEngine->oclSampler->pmj02.tableSamples;
	} else if (renderEngine->oclSampler->type == slg::ocl::TILEPATHSAMPLER) {
		size += sizeof(slg::ocl::TilePathSamplerSharedData);

		switch (renderEngine->GetType()) {
			case TILEPATHOCL:
				size += sizeof(u_int) * renderEngine->pathTracer.eyeSampleSize * SOBOL_BITS;
				break;
			case RTPATHOCL:
				break;
			default:
				throw runtime_error("Unknown render engine in PathOCLBaseRenderThread::InitSamplerSharedDataBuffer(): " +
						ToString(renderEngine->GetType()));
		}
	} else
		throw runtime_error("Unknown sampler.type in PathOCLBaseRenderThread::InitSamplerSharedDataBuffer(): " +
				ToString(renderEngine->oclSampler->type));

	if (size == 0)
		intersectionDevice.FreeBuffer(&samplerSharedDataBuff);
	else
		intersectionDevice.AllocBufferRW(&samplerSharedDataBuff, nullptr, size, "SamplerSharedData");

	// Initialize the sampler shared data
	if (renderEngine->oclSampler->type == slg::ocl::RANDOM) {
		slg::ocl::RandomSamplerSharedData rssd;
		rssd.bucketIndex = 0;

		intersectionDevice.EnqueueWriteBuffer(samplerSharedDataBuff, CL_TRUE, size, &rssd);
	} else if (renderEngine->oclSampler->type == slg::ocl::SOBOL) {
		auto _buffer = std::make_unique<char[]>(size);
		auto buffer = _buffer.get();

		// Initialize SobolSamplerSharedData fields
		slg::ocl::SobolSamplerSharedData *sssd = (slg::ocl::SobolSamplerSharedData *)buffer;

		sssd->seedBase = renderEngine->seedBase;
		sssd->bucketIndex = 0;
		sssd->filmRegionPixelCount = filmRegionPixelCount;
		// Light-path dims are shifted by 2 (IDX_SCREEN_X/Y): see the
		// sizing above
		const u_int sobolDimCount = (renderEngine->lightTaskCount > 0) ?
				Max(renderEngine->pathTracer.eyeSampleSize,
				renderEngine->pathTracer.lightSampleSize + 2) :
				renderEngine->pathTracer.eyeSampleSize;
		sssd->sobolDimensions = sobolDimCount;

		// Initialize all pass values. The pass buffer is attached at the
		// end of slg::ocl::SobolSamplerSharedData
		u_int *passBuffer = (u_int *)(buffer + sizeof(slg::ocl::SobolSamplerSharedData));
		fill(passBuffer, passBuffer + filmRegionPixelCount, SOBOL_STARTOFFSET);

		// Initialize the Sobol directions array values. The pass buffer is attached at the
		// end of slg::ocl::SobolSamplerSharedData + all pass values

		u_int *sobolDirections = (u_int *)(buffer + sizeof(slg::ocl::SobolSamplerSharedData) + sizeof(u_int) * filmRegionPixelCount);
		SobolSequence::GenerateDirectionVectors(sobolDirections, sobolDimCount);

		// The Owen blue-noise scramble rank tile is appended after the
		// directions array
		u_int *scrambleTile = sobolDirections + sobolDimCount * SOBOL_BITS;
		SobolSequence::GenerateScrambleTile(scrambleTile, SOBOL_OWEN_TILE_SIZE);

		// The per-pixel luma moments for adaptive sampling start at 0
		float *lumaMoments = (float *)(scrambleTile + SOBOL_OWEN_TILE_SIZE * SOBOL_OWEN_TILE_SIZE);
		fill(lumaMoments, lumaMoments + 2 * filmRegionPixelCount, 0.f);

		// Write the data
		intersectionDevice.EnqueueWriteBuffer(samplerSharedDataBuff, CL_TRUE, size, buffer);
		
	} else if (renderEngine->oclSampler->type == slg::ocl::PMJ02SAMPLER) {
		auto _buffer = std::make_unique<char[]>(size);
		auto buffer = _buffer.get();

		// Same header layout as Sobol (the kernel reuses its offsets)
		slg::ocl::SobolSamplerSharedData *sssd = (slg::ocl::SobolSamplerSharedData *)buffer;

		sssd->seedBase = renderEngine->seedBase;
		sssd->bucketIndex = 0;
		sssd->filmRegionPixelCount = filmRegionPixelCount;
		sssd->sobolDimensions = 0;

		// Pass values start at 0 (PMJ02 has no degenerate early points)
		u_int *passBuffer = (u_int *)(buffer + sizeof(slg::ocl::SobolSamplerSharedData));
		fill(passBuffer, passBuffer + filmRegionPixelCount, 0u);

		// PMJ02 tables, pair after pair
		float *tables = (float *)(buffer + sizeof(slg::ocl::SobolSamplerSharedData) +
				sizeof(u_int) * filmRegionPixelCount);
		PMJ02Sampler::FillDeviceTables(
				renderEngine->oclSampler->pmj02.tablePairs,
				renderEngine->oclSampler->pmj02.tableSamples,
				renderEngine->seedBase, tables);

		// Write the data
		intersectionDevice.EnqueueWriteBuffer(samplerSharedDataBuff, CL_TRUE, size, buffer);
	} else if (renderEngine->oclSampler->type == slg::ocl::TILEPATHSAMPLER) {
		// TilePathSamplerSharedData is updated in PathOCLBaseOCLRenderThread::UpdateSamplerData()
		
		switch (renderEngine->GetType()) {
			case TILEPATHOCL: {
				auto _buffer = std::make_unique<char[]>(size);
				auto buffer = _buffer.get();

				// Initialize the Sobol directions array values
				u_int *sobolDirections = (u_int *)(buffer + sizeof(slg::ocl::TilePathSamplerSharedData));
				SobolSequence::GenerateDirectionVectors(sobolDirections, renderEngine->pathTracer.eyeSampleSize);

				intersectionDevice.EnqueueWriteBuffer(samplerSharedDataBuff, CL_TRUE, size, &buffer[0]);
				break;
			}
			case RTPATHOCL:
				break;
			default:
				throw runtime_error("Unknown render engine in PathOCLBaseRenderThread::InitSamplerSharedDataBuffer(): " +
						ToString(renderEngine->GetType()));
		}
	}
}

void PathOCLBaseOCLRenderThread::InitSamplesBuffer() {
	const u_int taskCount = renderEngine->taskCount;

	//--------------------------------------------------------------------------
	// Sample size
	//--------------------------------------------------------------------------

	size_t sampleSize = 0;

	// Add Sample memory size
	switch (renderEngine->oclSampler->type) {
		case slg::ocl::RANDOM: {
			// pixelIndexBase, pixelIndexOffset and pixelIndexRandomStart fields
			sampleSize += sizeof(slg::ocl::RandomSample);
			break;
		}
		case  slg::ocl::METROPOLIS: {
			const size_t sampleResultSize = sizeof(slg::ocl::SampleResult);
			sampleSize += 2 * sizeof(float) + 5 * sizeof(u_int) + sampleResultSize;
			break;
		}
		case slg::ocl::PMJ02SAMPLER: {
			// Same per-task cursor state as RandomSample
			// (bucketIndex, pixelOffset, passOffset, pass)
			sampleSize += sizeof(slg::ocl::RandomSample);
			break;
		}
		case slg::ocl::SOBOL: {
			sampleSize += sizeof(slg::ocl::SobolSample);
			break;
		}
		case slg::ocl::TILEPATHSAMPLER: {
			sampleSize += sizeof(slg::ocl::TilePathSample);
			break;
		}
		default:
			throw runtime_error("Unknown sampler.type in PathOCLBaseRenderThread::InitSamplesBuffer(): " +
					ToString(renderEngine->oclSampler->type));
	}

	SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex << "] Size of a Sample: " << sampleSize << "bytes");
	intersectionDevice.AllocBufferRW(&samplesBuff, nullptr, sampleSize * taskCount, "Sample");
}

void PathOCLBaseOCLRenderThread::InitSampleResultsBuffer() {
	const u_int taskCount = renderEngine->taskCount;

	const size_t sampleResultSize = sizeof(slg::ocl::SampleResult);

	SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex << "] Size of a SampleResult: " << sampleResultSize << "bytes");
	intersectionDevice.AllocBufferRW(&sampleResultsBuff, nullptr, sampleResultSize * taskCount, "SampleResult");
}

void PathOCLBaseOCLRenderThread::InitSampleDataBuffer() {
	const u_int taskCount = renderEngine->taskCount;

	size_t uDataSize;
	if (renderEngine->oclSampler->type == slg::ocl::RANDOM) {
		// To store IDX_SCREEN_X and IDX_SCREEN_Y
		uDataSize = 2 * sizeof(float);
	} else if (renderEngine->oclSampler->type == slg::ocl::SOBOL) {
		// To store IDX_SCREEN_X and IDX_SCREEN_Y
		uDataSize = 2 * sizeof(float);
	} else if (renderEngine->oclSampler->type == slg::ocl::PMJ02SAMPLER) {
		// To store IDX_SCREEN_X and IDX_SCREEN_Y
		uDataSize = 2 * sizeof(float);
	} else if (renderEngine->oclSampler->type == slg::ocl::METROPOLIS) {
		// Metropolis needs 2 sets of samples, the current and the proposed mutation
		uDataSize = 2 * sizeof(float) * renderEngine->pathTracer.eyeSampleSize;
	} else if (renderEngine->oclSampler->type == slg::ocl::TILEPATHSAMPLER) {
		// To store IDX_SCREEN_X and IDX_SCREEN_Y
		uDataSize = 2 * sizeof(float);
	} else
		throw runtime_error("Unknown sampler.type in PathOCLBaseRenderThread::InitSampleDataBuffer(): " + ToString(renderEngine->oclSampler->type));

	SLG_LOG("[PathOCLBaseRenderThread::" << threadIndex << "] Size of a SampleData: " << uDataSize << "bytes");

	intersectionDevice.AllocBufferRW(&sampleDataBuff, nullptr, uDataSize * taskCount, "SampleData");
}

void PathOCLBaseOCLRenderThread::InitRender() {
	//--------------------------------------------------------------------------
	// Path guiding frozen table first (M2b): small chunk uploads before
	// all other buffers.
	//--------------------------------------------------------------------------

	InitGuide();

	//--------------------------------------------------------------------------
	// Film definition
	//--------------------------------------------------------------------------

	InitFilm();

	//--------------------------------------------------------------------------
	// Camera definition
	//--------------------------------------------------------------------------

	InitCamera();

	//--------------------------------------------------------------------------
	// Scene geometry
	//--------------------------------------------------------------------------

	InitGeometry();

	//--------------------------------------------------------------------------
	// Image maps
	//--------------------------------------------------------------------------

	InitImageMaps();

	//--------------------------------------------------------------------------
	// Texture definitions
	//--------------------------------------------------------------------------

	InitTextures();

	//--------------------------------------------------------------------------
	// Material definitions
	//--------------------------------------------------------------------------

	InitMaterials();

	//--------------------------------------------------------------------------
	// Mesh <=> Material links
	//--------------------------------------------------------------------------

	InitSceneObjects();

	//--------------------------------------------------------------------------
	// Light definitions
	//--------------------------------------------------------------------------

	InitLights();

	//--------------------------------------------------------------------------
	// Light definitions
	//--------------------------------------------------------------------------

	InitPhotonGI();

	//--------------------------------------------------------------------------
	// GPUTaskStats
	//--------------------------------------------------------------------------

	const u_int taskCount = renderEngine->taskCount;

	// In case renderEngine->taskCount has changed
	gpuTaskStats = std::move(
		std::make_unique<slg::ocl::pathoclbase::GPUTaskStats[]>(taskCount)
	);
	for (u_int i = 0; i < taskCount; ++i)
		gpuTaskStats[i].sampleCount = 0;

	//--------------------------------------------------------------------------
	// Allocate GPU task buffers
	//--------------------------------------------------------------------------

	// NOTE: must run before the Ray/RayHit allocation below: it fills
	// taskConfig.pathTracer.restir.visCandCount, which determines the
	// candidate tail size of the ray/hit buffers. Reading visCandCount
	// before this call yields the compile-time 0 and the buffers would
	// be allocated without the tail while the kernels still index
	// taskCount * (1 + K) slots - a device-side out-of-bounds access
	// that wedges the GPU (observed as WindowServer watchdog panics).
	InitGPUTaskBuffer();

	//--------------------------------------------------------------------------
	// Allocate Ray/RayHit buffers
	//--------------------------------------------------------------------------

	// ReSTIR visibility (E2a/E2d): rays/hits hold taskCount regular rays
	// plus, when the visibility target is on, taskCount *
	// (visCandCount + RESTIR_PIXEL_MERGES_MAX) candidate and merge
	// shadow rays; ReSTIR GI (G1) appends taskCount * (2*giCandCount + 1)
	// bounce + NEE + temporal-merge visibility rays behind them
	// (giCandRayBase matches this tail start in InitGPUTaskBuffer()).
	// GPU light tracing adds lightTaskCount camera-visibility ray slots
	// between the per-task rays and the ReSTIR tails (lightVisRayBase).
	const u_int raySlotCount = taskCount +
			renderEngine->lightTaskCount +
			taskCount * (
			((renderEngine->taskConfig.pathTracer.restir.visCandCount > 0u) ?
			(renderEngine->taskConfig.pathTracer.restir.visCandCount +
			RESTIR_PIXEL_MERGES_MAX) : 0u) +
			2u * renderEngine->taskConfig.pathTracer.restirGI.giCandCount +
			((renderEngine->taskConfig.pathTracer.restirGI.giCandCount > 0u) ?
			1u : 0u));
	intersectionDevice.AllocBufferRW(&raysBuff, nullptr, sizeof(Ray) * raySlotCount, "Ray");
	intersectionDevice.AllocBufferRW(&hitsBuff, nullptr, sizeof(RayHit) * raySlotCount, "RayHit");

	//--------------------------------------------------------------------------
	// Allocate GPU task statistic buffers
	//--------------------------------------------------------------------------

	intersectionDevice.AllocBufferRW(&taskStatsBuff, nullptr, sizeof(slg::ocl::pathoclbase::GPUTaskStats) * taskCount, "GPUTask Stats");

	//--------------------------------------------------------------------------
	// Allocate sampler shared data buffer
	//--------------------------------------------------------------------------

	InitSamplerSharedDataBuffer();

	//--------------------------------------------------------------------------
	// Allocate sample buffers
	//--------------------------------------------------------------------------

	InitSamplesBuffer();

	//--------------------------------------------------------------------------
	// Allocate sample data buffers
	//--------------------------------------------------------------------------

	InitSampleDataBuffer();

	//--------------------------------------------------------------------------
	// Allocate sample result buffers
	//--------------------------------------------------------------------------

	InitSampleResultsBuffer();

	//--------------------------------------------------------------------------
	// Allocate volume info buffers if required
	//--------------------------------------------------------------------------

	intersectionDevice.AllocBufferRW(&eyePathInfosBuff, nullptr, sizeof(slg::ocl::EyePathInfo) * taskCount, "PathInfo");

	// GPU light tracing: LightPathInfo per light task, indexed by
	// (gid - eyeTaskCount). Sized 0 when disabled.
	if (renderEngine->lightTaskCount > 0)
		intersectionDevice.AllocBufferRW(&lightPathInfosBuff, nullptr,
				sizeof(slg::ocl::pathoclbase::LightPathInfo) * renderEngine->lightTaskCount,
				"LightPathInfo");
	else
		intersectionDevice.FreeBuffer(&lightPathInfosBuff);

	// Caustic focus cache (guided emission): per-light ring of the last
	// LIGHT_FOCUS_K productive target positions (float4: xyz + aim
	// radius) plus a monotonic fill counter used as the ring cursor.
	// Zero-filled: count 0 means "no hotspot learned yet".
	if ((renderEngine->lightTaskCount > 0) &&
			renderEngine->taskConfig.pathTracer.lightTracing.focusEnable &&
			(renderEngine->compiledScene->lightDefs.size() > 0)) {
		const u_int lightCount = renderEngine->compiledScene->lightDefs.size();
		std::vector<float> zeroFocus(4 * LIGHT_FOCUS_K * lightCount, 0.f);
		// Distant-light casters ride the same buffer, appended after the
		// rings (kernel base offset: lightCount * LIGHT_FOCUS_K)
		zeroFocus.insert(zeroFocus.end(),
				renderEngine->compiledScene->lightFocusCasters.begin(),
				renderEngine->compiledScene->lightFocusCasters.end());
		intersectionDevice.AllocBufferRW(&lightFocusBuff, zeroFocus.data(),
				sizeof(float) * zeroFocus.size(), "LightFocusPoints");
		std::vector<u_int> zeroCount(lightCount, 0u);
		intersectionDevice.AllocBufferRW(&lightFocusCountBuff, zeroCount.data(),
				sizeof(u_int) * zeroCount.size(), "LightFocusCounts");
	} else {
		intersectionDevice.FreeBuffer(&lightFocusBuff);
		intersectionDevice.FreeBuffer(&lightFocusCountBuff);
	}

	// Packed pixel-filter LUTs for the light-path camera splat (CPU
	// FilmSampleSplatter parity). NULL under FILTER_NONE keeps the kernel
	// on the box splat. Layout: [0]=lutsSize, [1]=xWidth, [2]=yWidth,
	// then per-offset {lutW, lutH, dataBase} headers followed by the
	// weight floats (kernel walk: Film_SplatLight in film_funcs.cl).
	FilterRPtr lightSplatFilter = renderEngine->GetPixelFilter();
	if ((renderEngine->lightTaskCount > 0) && lightSplatFilter &&
			(lightSplatFilter->GetType() != FILTER_NONE)) {
		const u_int lutsSize = Max<u_int>(4,
				Max(lightSplatFilter->xWidth, lightSplatFilter->yWidth) + 1);
		FilterLUTs luts(*lightSplatFilter, lutsSize);

		std::vector<float> packed;
		packed.push_back((float)lutsSize);
		packed.push_back(lightSplatFilter->xWidth);
		packed.push_back(lightSplatFilter->yWidth);
		const u_int lutCount = lutsSize * lutsSize;
		const size_t headerBase = packed.size();
		packed.resize(headerBase + 3 * lutCount);
		for (u_int i = 0; i < lutCount; ++i) {
			const FilterLUT *lut = luts.GetLUTAt(i);
			packed[headerBase + 3 * i] = (float)lut->GetWidth();
			packed[headerBase + 3 * i + 1] = (float)lut->GetHeight();
			packed[headerBase + 3 * i + 2] = (float)packed.size();
			const std::span<const float> weights = lut->GetLUT();
			packed.insert(packed.end(), weights.begin(), weights.end());
		}
		intersectionDevice.AllocBufferRO(&lightFilterLUTBuff, packed.data(),
				sizeof(float) * packed.size(), "LightFilterLUTs");
	} else
		intersectionDevice.FreeBuffer(&lightFilterLUTBuff);

	//--------------------------------------------------------------------------
	// Allocate the ReSTIR DI per-pixel temporal reservoirs (zeroed: an
	// all-zero cell means "no reservoir yet")
	//--------------------------------------------------------------------------

	{
		const u_int reservoirCount =
				renderEngine->taskConfig.pathTracer.restir.reservoirCount;
		// The visibility-candidate records are appended after the
		// per-pixel reservoirs: taskCount * (K + RESTIR_PIXEL_MERGES_MAX)
		// RestirVisCandidate records, sized in bytes and rounded up to
		// whole reservoir slots so the layout stays correct if the two
		// structs' sizes diverge again (both are 56B today).
		const u_int candTailCount =
				(renderEngine->taskConfig.pathTracer.restir.visCandCount > 0u) ?
				taskCount * (renderEngine->taskConfig.pathTracer.restir.visCandCount +
				RESTIR_PIXEL_MERGES_MAX) : 0u;
		// ReSTIR GI (G1 GPU): per-pixel GI reservoirs then the per-task
		// candidate/result records follow the DI region. The offsets
		// were computed in InitGPUTaskBuffer() (giReservoirOffset/
		// giCandDataOffset/giCandStride, all in reservoir-slot units).
		const auto &gi = renderEngine->taskConfig.pathTracer.restirGI;
		const u_int giTotalSlots = (gi.giCandCount > 0u) ?
				(gi.giCandDataOffset + taskCount * gi.giCandStride) :
				(gi.giReservoirOffset + (u_int)(
				((size_t)gi.reservoirCount *
				sizeof(slg::ocl::pathoclbase::RestirGIReservoir) +
				sizeof(slg::ocl::pathoclbase::RestirReservoir) - 1) /
				sizeof(slg::ocl::pathoclbase::RestirReservoir)));
		const u_int totalCount = reservoirCount + (u_int)(
				((size_t)candTailCount *
				sizeof(slg::ocl::pathoclbase::RestirVisCandidate) +
				sizeof(slg::ocl::pathoclbase::RestirReservoir) - 1) /
				sizeof(slg::ocl::pathoclbase::RestirReservoir));
		// giReservoirOffset == reservoirCount + DI candidate slots, so
		// the GI total is the end of the buffer whenever GI is enabled.
		const u_int allocCount = (gi.reservoirCount > 0u) ?
				giTotalSlots : totalCount;
		std::vector<slg::ocl::pathoclbase::RestirReservoir> zeroReservoirs(allocCount);
		intersectionDevice.AllocBufferRW(&restirReservoirsBuff, zeroReservoirs.data(),
				sizeof(slg::ocl::pathoclbase::RestirReservoir) * allocCount, "RestirReservoirs");
	}

	//--------------------------------------------------------------------------
	// Allocate the MNEE manifold seed cache (zeroed: an all-zero entry
	// means "no cached seed"); only used when path.mnee.enable is set.
	//--------------------------------------------------------------------------

	{
		std::vector<slg::ocl::pathoclbase::MneeSeedEntry> zeroSeeds(
				MNEE_SEED_CACHE_SIZE);
		intersectionDevice.AllocBufferRW(&mneeSeedsBuff, zeroSeeds.data(),
				sizeof(slg::ocl::pathoclbase::MneeSeedEntry) *
				MNEE_SEED_CACHE_SIZE, "MneeSeeds");
	}

	//--------------------------------------------------------------------------
	// Allocate volume info buffers if required
	//--------------------------------------------------------------------------

	intersectionDevice.AllocBufferRW(&directLightVolInfosBuff, nullptr, sizeof(slg::ocl::PathVolumeInfo) * taskCount, "DirectLightVolumeInfo");

	//--------------------------------------------------------------------------
	// Allocate GPU pixel filter distribution
	//--------------------------------------------------------------------------

	intersectionDevice.AllocBufferRO(&pixelFilterBuff, renderEngine->pixelFilterDistribution.data(),
			renderEngine->pixelFilterDistributionSize, "Pixel Filter Distribution");

	//--------------------------------------------------------------------------
	// Compile kernels
	//--------------------------------------------------------------------------

	InitKernels();

	//--------------------------------------------------------------------------
	// Initialize
	//--------------------------------------------------------------------------

	// Set kernel arguments
	SetKernelArgs();

	// Clear all thread films
	for(ThreadFilmRPtr threadFilm: threadFilms) {
		intersectionDevice.PushThreadCurrentDevice();
		threadFilm->ClearFilm(intersectionDevice, filmClearKernel, filmClearWorkGroupSize);
		intersectionDevice.PopThreadCurrentDevice();
	}

	intersectionDevice.FinishQueue();

	// Reset statistics in order to be more accurate
	intersectionDevice.ResetPerformaceStats();
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
