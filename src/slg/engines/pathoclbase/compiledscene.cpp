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

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <limits>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include "slg/engines/pathoclbase/compiledscene.h"
#include "slg/kernels/kernels.h"

using namespace std;
using namespace luxrays;
using namespace slg;

CompiledScene::CompiledScene(SceneConstRef scn, const PathTracer *pt) : scene(scn) {
	pathTracer = pt;
	maxMemPageSize = numeric_limits<size_t>::max();

	lightsDistribution.clear();
	
	EditActionList editActions;
	editActions.AddAllAction();
	Recompile(editActions);
}

CompiledScene::~CompiledScene() {
}

void CompiledScene::SetMaxMemPageSize(const size_t maxSize) {
	maxMemPageSize = maxSize;
}

void CompiledScene::EnableCode(const std::string &tags) {
	SLG_LOG("Always enabled OpenCL code: " + tags);
	boost::split(enabledCode, tags, boost::is_any_of(" \t"));
}

void CompiledScene::Compile() {
	EditActionList editActions;
	editActions.AddAllAction();
	Recompile(editActions);
}

void CompiledScene::Recompile(const EditActionList &editActions) {
	wasCameraCompiled = false;
	wasGeometryCompiled = false;
	wasMaterialsCompiled = false;
	wasSceneObjectsCompiled = false;
	wasLightsCompiled = false;
	wasImageMapsCompiled = false;
	wasPhotonGICompiled = false;
	wasEmissionDistsCompiled = false;

	if (editActions.Has(CAMERA_EDIT))
		CompileCamera();
	// GEOMETRY_TRANS_EDIT is also handled in RenderEngine::EndSceneEdit() if
	// accelerators support updates but still need to update transformations
	// inside mesh description here.
	if (editActions.Has(GEOMETRY_EDIT) || editActions.Has(GEOMETRY_TRANS_EDIT))
		CompileGeometry();
	if (editActions.Has(MATERIALS_EDIT) || editActions.Has(MATERIAL_TYPES_EDIT))
		CompileMaterials();
	if (editActions.Has(GEOMETRY_EDIT) || editActions.Has(MATERIALS_EDIT) || editActions.Has(MATERIAL_TYPES_EDIT))
		CompileSceneObjects();
	// GEOMETRY_EDIT and GEOMETRY_TRANS_EDIT are included here because a triangle
	// area light may have been edited
	// wasEmissionDistsCompiled: a changed material emission map rebuilds
	// emissionFuncDistributions, shifting the absolute offsets stored in
	// lightDefs, so lights must be recompiled too
	if (editActions.Has(GEOMETRY_EDIT) || editActions.Has(GEOMETRY_TRANS_EDIT) ||
			editActions.Has(LIGHTS_EDIT) || editActions.Has(LIGHT_TYPES_EDIT) ||
			wasEmissionDistsCompiled)
		CompileLights();
	if (editActions.Has(IMAGEMAPS_EDIT))
		CompileImageMaps();

	if (wasGeometryCompiled || wasMaterialsCompiled || wasSceneObjectsCompiled ||
			wasLightsCompiled || wasImageMapsCompiled)
		CompilePathTracer();
	
	// For some debugging
//	cout << "=========================================================\n";
//	cout << GetTexturesEvaluationSourceCode();
//	cout << "=========================================================\n";
//	cout << GetMaterialsEvaluationSourceCode();
//	cout << "=========================================================\n";
}

string CompiledScene::ToOCLString(const slg::ocl::Spectrum &v) {
	return "(float3)(" + ToString(v.c[0]) + ", " + ToString(v.c[1]) + ", " + ToString(v.c[2]) + ")";
}

// Swaps the host staging arrays (geometry + image map pages) for
// copy-on-write file mappings under the scene spill dir. Safe to call
// only after the device upload queue has been synchronized; arrays
// already spilled are skipped, so additional render threads calling
// this just no-op. The mappings keep the data readable for device
// restarts and later-started devices while letting the kernel evict
// the pages under memory pressure.
size_t CompiledScene::SpillHostStaging() {
	if (!scene.GeoSpillEnabled() || scene.GeoSpillDir().empty())
		return 0;

	const std::string pfx = scene.GeoSpillDir() + "/stg-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
			"-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "-";
	std::filesystem::create_directories(scene.GeoSpillDir());

	const size_t minBytes = scene.GeoSpillMinBytes();
	size_t spilled = 0;
	auto spill = [&](auto &arr, const std::string &name) {
		if (arr.size() * sizeof(arr[0]) >= minBytes)
			spilled += arr.Spill(pfx + name + ".bin");
	};
	spill(verts, "verts");
	spill(normals, "normals");
	spill(triNormals, "trinormals");
	spill(uvs, "uvs");
	spill(cols, "cols");
	spill(alphas, "alphas");
	spill(vertexAOVs, "vertaovs");
	spill(triAOVs, "triaovs");
	spill(tris, "tris");
	spill(interpolatedTransforms, "itran");
	spill(curveCps, "curvecps");
	spill(curveSegIndices, "curvesegs");
	spill(curveCpAttrs, "curveattrs");
	spill(meshDescs, "meshdescs");
	spill(cameraBokehDistribution, "bokehdist");
	spill(sceneObjs, "sceneobjs");
	spill(lightDefs, "lightdefs");
	spill(envLightIndices, "envlightidx");
	spill(lightIndexOffsetByMeshIndex, "lightidxmesh");
	spill(lightIndexByTriIndex, "lightidxtri");
	spill(envLightDistributions, "envlightdist");
	spill(emissionFuncDistributions, "emitdist");
	spill(lightsDistribution, "lightdist");
	spill(infiniteLightSourcesDistribution, "infdist");
	spill(emitLightsDistribution, "emitlightdist");
	spill(lightFocusCasters, "focuscasters");
	spill(dlscAllEntries, "dlscentries");
	spill(dlscDistributions, "dlscdist");
	spill(dlscBVHArrayNode, "dlscbvh");
	spill(elvcAllEntries, "elvcentries");
	spill(elvcDistributions, "elvcdist");
	spill(elvcTileDistributionOffsets, "elvctileoff");
	spill(elvcBVHArrayNode, "elvcbvh");
	spill(mats, "mats");
	spill(volMajorants, "volmaj");
	spill(eqLightPoints, "eqlightpts");
	spill(texs, "texs");
	spill(texEvalOps, "texevalops");
	spill(imageMapDescs, "imapdescs");
	spill(pgicRadiancePhotons, "pgicradiance");
	spill(pgicRadiancePhotonsValues, "pgicvalues");
	spill(pgicRadiancePhotonsBVHArrayNode, "pgicbvh");
	spill(pgicCausticPhotons, "pgiccaustic");
	spill(pgicCausticPhotonsBVHArrayNode, "pgiccbvh");
	for (u_int i = 0; i < imageMapMemBlocks.size(); ++i)
		spill(imageMapMemBlocks[i], "imaps-" + std::to_string(i));

	return spilled;
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
