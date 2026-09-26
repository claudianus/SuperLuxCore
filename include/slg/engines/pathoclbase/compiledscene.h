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

#ifndef _SLG_COMPILEDSESSION_H
#define	_SLG_COMPILEDSESSION_H

#if !defined(LUXRAYS_DISABLE_OPENCL)

#include "slg/slg.h"
#include "slg/editaction.h"

#include "luxrays/utils/spillablearray.h"

#include "slg/core/indexbvh.h"
#include "slg/film/film.h"
#include "slg/scene/scene.h"
#include "slg/scene/sceneobject.h"
#include "slg/lights/strategies/dlscache.h"
#include "slg/lights/visibility/envlightvisibilitycache.h"
#include "slg/engines/pathtracer.h"
#include "slg/cameras/camera.h"

namespace slg {

class CompiledScene {
public:
	CompiledScene(SceneConstRef scn, const PathTracer *pt);
	~CompiledScene();
	
	void SetMaxMemPageSize(const size_t maxSize);
	void EnableCode(const std::string &tags);

	void Compile();
	void Recompile(const EditActionList &editActions);
	void RecompilePhotonGI() { CompilePhotonGI(); }

	// Swaps the host staging arrays (geometry + image map pages) for
	// file-backed mappings once the scene spill feature is enabled.
	// Call only after the device upload queue has been synchronized.
	// Re-uploads (device restarts, additional devices) read through the
	// mappings transparently. Returns the spilled byte count.
	size_t SpillHostStaging();

	static void CompileFilm(const Film &film, slg::ocl::Film &oclFilm);

	static std::tuple<std::vector<float>, size_t>
	CompileDistribution1D(luxrays::Distribution1DConstRef dist);


	static std::tuple<std::vector<float>, size_t>
	CompileDistribution2D(luxrays::Distribution2DConstRef dist);

	static std::string ToOCLString(const slg::ocl::Spectrum &v);

	// Compiled Camera
	slg::ocl::Camera camera;
	luxrays::SpillableArray<float> cameraBokehDistribution;
	u_int cameraBokehDistributionSize;

	// Compiled Scene Meshes. These are host staging arrays for the device
	// uploads: they are SpillableArray so a render thread can swap them
	// for file-backed mappings once the upload is done, without breaking
	// re-uploads on device restarts or additional devices.
	luxrays::SpillableArray<luxrays::Point> verts;
	luxrays::SpillableArray<luxrays::Normal> normals;
	luxrays::SpillableArray<luxrays::Normal> triNormals;
	luxrays::SpillableArray<luxrays::UV> uvs;
	luxrays::SpillableArray<luxrays::Spectrum> cols;
	luxrays::SpillableArray<float> alphas;
	luxrays::SpillableArray<float> vertexAOVs;
	luxrays::SpillableArray<float> triAOVs;
	luxrays::SpillableArray<luxrays::Triangle> tris;
	luxrays::SpillableArray<luxrays::ocl::InterpolatedTransform> interpolatedTransforms;
	luxrays::SpillableArray<luxrays::ocl::ExtMesh> meshDescs;

	// Native curve primitives (Metal HWRT; dev-tools/metal_curve_design.md):
	// global control-point buffer (xyz + radius float4), global per-segment
	// start-cp indices, per-cp shading attributes (2 float4 per cp).
	luxrays::SpillableArray<luxrays::CurveControlPoint> curveCps;
	luxrays::SpillableArray<u_int> curveSegIndices;
	luxrays::SpillableArray<luxrays::CurveCpAttr> curveCpAttrs;
	luxrays::BSphere worldBSphere;

	// Compiled Scene Objects
	luxrays::SpillableArray<slg::ocl::SceneObject> sceneObjs;

	// Compiled Lights
	luxrays::SpillableArray<slg::ocl::LightSource> lightDefs;
	// Additional light related information
	luxrays::SpillableArray<u_int> envLightIndices;
	luxrays::SpillableArray<u_int> lightIndexOffsetByMeshIndex, lightIndexByTriIndex;
	// Env. light Distribution2Ds. The device buffer is
	// [emissionFuncDistributions | envLightDistributions]: material
	// emission distributions come first so their absolute offsets stay
	// valid when lights are recompiled without materials
	luxrays::SpillableArray<float> envLightDistributions;
	// Material directional emission map (SampleableSphericalFunction)
	// Distribution2Ds, indexed by Material::emissionFuncDistOffset
	luxrays::SpillableArray<float> emissionFuncDistributions;
	// Compiled light sampling strategy
	luxrays::SpillableArray<float> lightsDistribution;
	u_int lightsDistributionSize;
	luxrays::SpillableArray<float> infiniteLightSourcesDistribution;
	u_int infiniteLightSourcesDistributionSize;
	// GPU light tracing: Distribution1D over the emit light strategy
	// (same light-index order as lights[]); lights unsupported by the
	// device Emit ports carry zero weight
	luxrays::SpillableArray<float> emitLightsDistribution;
	u_int emitLightsDistributionSize;
	// Distant-light caustic focusing: delta-specular caster bounding
	// spheres (4 floats per caster: center.xyz + radius), appended to
	// the lightFocus device buffer after the per-light hotspot rings
	luxrays::SpillableArray<float> lightFocusCasters;
	// DLSC related data
	luxrays::SpillableArray<slg::ocl::DLSCacheEntry> dlscAllEntries;
	luxrays::SpillableArray<float> dlscDistributions;
	luxrays::SpillableArray<luxrays::ocl::IndexBVHArrayNode> dlscBVHArrayNode;
	float dlscRadius2, dlscNormalCosAngle;
	// EnvLightVisibilityCache related data
	luxrays::SpillableArray<slg::ocl::ELVCacheEntry> elvcAllEntries;
	luxrays::SpillableArray<float> elvcDistributions;
	luxrays::SpillableArray<u_int> elvcTileDistributionOffsets;
	luxrays::SpillableArray<luxrays::ocl::IndexBVHArrayNode> elvcBVHArrayNode;
	float elvcRadius2, elvcNormalCosAngle;
	u_int elvcTilesXCount, elvcTilesYCount;

	// Compiled Materials (and Volumes)
	luxrays::SpillableArray<slg::ocl::Material> mats;
	// (plain vector: CompileMaterialOps() helpers take vector&)
	std::vector<slg::ocl::MaterialEvalOp> matEvalOps;
	// Expressed in float
	u_int maxMaterialEvalStackSize;
	u_int defaultWorldVolumeIndex;
	// Null-collision majorant cells of all heterogeneous volumes
	// (concatenated, indexed by HeterogenousVolumeParam::majorantOffset).
	// Each cell is a float pair (minorant, majorant).
	luxrays::SpillableArray<float> volMajorants;
	// World positions of point-ish lights eligible for equiangular
	// distance sampling, 4 floats (xyz + pad) per light
	luxrays::SpillableArray<float> eqLightPoints;

	// Compiled Textures
	luxrays::SpillableArray<slg::ocl::Texture> texs;
	luxrays::SpillableArray<slg::ocl::TextureEvalOp> texEvalOps;
	// Expressed in float
	u_int maxTextureEvalStackSize;

	// Compiled ImageMaps
	luxrays::SpillableArray<slg::ocl::ImageMap> imageMapDescs;
	std::vector<luxrays::SpillableArray<float> > imageMapMemBlocks;

	// Compiled PhotonGI cache

	// PhotonGI indirect cache
	luxrays::SpillableArray<slg::ocl::RadiancePhoton> pgicRadiancePhotons;
	u_int pgicLightGroupCounts;
	luxrays::SpillableArray<slg::ocl::Spectrum> pgicRadiancePhotonsValues;
	luxrays::SpillableArray<luxrays::ocl::IndexBVHArrayNode> pgicRadiancePhotonsBVHArrayNode;
	// PhotonGI caustic cache
	luxrays::SpillableArray<slg::ocl::Photon> pgicCausticPhotons;
	luxrays::SpillableArray<luxrays::ocl::IndexBVHArrayNode> pgicCausticPhotonsBVHArrayNode;

	// All global settings
	slg::ocl::PathTracer compiledPathTracer;

	// Elements compiled during the last call to Compile()/Recompile()
	bool wasCameraCompiled, wasSceneObjectsCompiled, wasGeometryCompiled, 
		wasMaterialsCompiled, wasLightsCompiled, wasImageMapsCompiled,
		wasPhotonGICompiled, wasEmissionDistsCompiled;

private:
	void AddToImageMapMem(slg::ocl::ImageMap &im, const void *data, const size_t memSize);
	u_int CompileImageMap(ImageMapConstRef im);

	void CompileCamera();
	void CompileSceneObjects();
	void CompileGeometry();
	u_int CompileMaterialConditionalOps(const u_int matIndex,
		const std::vector<slg::ocl::MaterialEvalOp> &evalOpsA, const u_int evalOpStackSizeA,
		const std::vector<slg::ocl::MaterialEvalOp> &evalOpsB, const u_int evalOpStackSizeB,
		std::vector<slg::ocl::MaterialEvalOp> &evalOps) const;
	u_int CompileMaterialConditionalOps(const u_int matIndex,
			const u_int matAIndex, const slg::ocl::MaterialEvalOpType opTypeA,
			const u_int matBIndex, const slg::ocl::MaterialEvalOpType opTypeB,
			std::vector<slg::ocl::MaterialEvalOp> &evalOps) const;
	u_int CompileMaterialOps(const u_int matIndex, const slg::ocl::MaterialEvalOpType opType,
			std::vector<slg::ocl::MaterialEvalOp> &evalOps) const;
	void CompileMaterialOps();
	void CompileMaterials();
	void CompileTextureMapping2D(
		slg::ocl::TextureMapping2D *mapping,
		TextureMapping2DConstRef m
	);
	void CompileTextureMapping3D(
		slg::ocl::TextureMapping3D *mapping,
		TextureMapping3DConstRef m
	);
	u_int CompileTextureOpsGenericBumpMap(const u_int texIndex);
	void PushSpectralPauseStartOp(const u_int texIndex);
	void PushSpectralPauseEndOp(const u_int texIndex);
	u_int CompileTextureOps(const u_int texIndex, const slg::ocl::TextureEvalOpType opType);
	void CompileTextureOps();
	void CompileTextures();
	void CompileImageMaps();
	void CompileLights();

	void CompileDLSC(const LightStrategyDLSCache& dlscLightStrategy);
	void CompileELVC(EnvLightVisibilityCacheRPtr visibilityMapCache);
	void CompileLightStrategy();

	void CompilePhotonGI();
	void CompilePathTracer();

	SceneConstRef scene;
	const PathTracer *pathTracer;

	friend class PathOCLBaseOCLRenderThread;

	size_t maxMemPageSize;
	std::unordered_set<std::string> enabledCode;
};

}

#endif

#endif	/* _SLG_COMPILEDSESSION_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
