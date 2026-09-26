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

#include "luxrays/core/dataset.h"
#include "luxrays/core/intersectiondevice.h"
#include "slg/core/sdl.h"
#include "slg/scene/scene.h"
#include "slg/cameras/camera.h"
#include "slg/lights/pointlight.h"
#include "slg/lights/spotlight.h"
#include "slg/lights/projectionlight.h"
#include "slg/lights/laserlight.h"
#include "slg/volumes/heterogenous.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Scene preprocess
//------------------------------------------------------------------------------

void Scene::PreprocessCamera(const u_int filmWidth, const u_int filmHeight, const u_int *filmSubRegion) {
	camera->Update(filmWidth, filmHeight, filmSubRegion);
}

void Scene::Preprocess(Context& ctx, const u_int filmWidth, const u_int filmHeight,
		const u_int *filmSubRegion, const bool useRTMode) {
	//--------------------------------------------------------------------------
	// Check if I have to update geometry
	//--------------------------------------------------------------------------

	if (!dataSet || editActions.Has(GEOMETRY_EDIT) ||
			(editActions.Has(GEOMETRY_TRANS_EDIT) &&
				!dataSet->DoesAllAcceleratorsSupportUpdate())) {
		if (ctx.IsRunning()) {
			// Stop all intersection devices
			ctx.Stop();
		}

		// Rebuild the data set
		dataSet = std::make_unique<DataSet>(ctx);

		// Add all objects
		for (u_int i = 0; i < objDefs.GetSize(); ++i)
			dataSet->Add(objDefs.GetSceneObject(i).GetExtMesh());

		dataSet->Preprocess();

		// Set the LuxRays DataSet
		ctx.SetDataSet(dataSet);

		// Devices are stopped and the old data set was replaced:
		// objects parked in the trash bin are no longer referenced by
		// anything and can be released for real.
		emptyTrash();

		// Restart all intersection devices
		ctx.Start();
	} else if(editActions.Has(GEOMETRY_TRANS_EDIT)) {
		// I have only to update the DataSet bounding boxes
		dataSet->UpdateBBoxes();
		ctx.UpdateDataSet();
	}
	
	// Only at this point I can safely trace rays

	//--------------------------------------------------------------------------
	// Check if I have to update the camera
	//--------------------------------------------------------------------------
	
	if (editActions.Has(CAMERA_EDIT))
		PreprocessCamera(filmWidth, filmHeight, filmSubRegion);

	// Update auto-focus and auto-volume
	camera->UpdateAuto(*this);

	// At this point, both the data set and the camera are updated
	const BBox sceneBBox = Union(dataSet->GetBBox(), camera->GetBBox());
	sceneBSphere = sceneBBox.BoundingSphere();		

	//--------------------------------------------------------------------------
	// Build the majorant grids used by heterogeneous volume tracking
	//--------------------------------------------------------------------------

	if (editActions.Has(GEOMETRY_EDIT) ||
			editActions.Has(GEOMETRY_TRANS_EDIT) ||
			editActions.Has(MATERIALS_EDIT) ||
			editActions.Has(MATERIAL_TYPES_EDIT) ||
			editActions.Has(IMAGEMAPS_EDIT)) {
		PreprocessVolumes(sceneBBox);
	}

	//--------------------------------------------------------------------------
	// Check if something has changed in light sources
	//--------------------------------------------------------------------------

	if (editActions.Has(GEOMETRY_EDIT) ||
			editActions.Has(GEOMETRY_TRANS_EDIT) ||
			editActions.Has(MATERIALS_EDIT) ||
			editActions.Has(MATERIAL_TYPES_EDIT) ||
			editActions.Has(LIGHTS_EDIT) ||
			editActions.Has(LIGHT_TYPES_EDIT) ||
			editActions.Has(IMAGEMAPS_EDIT)) {
		lightDefs.Preprocess(*this, useRTMode);
	}

	// And for visibility maps
	lightDefs.UpdateVisibilityMaps(*this, useRTMode);

	//--------------------------------------------------------------------------
	// Collect the point-ish lights eligible for equiangular distance
	// sampling (position-defined lights only; directional and area lights
	// are excluded)
	//--------------------------------------------------------------------------

	equiangularLightPoints.clear();
	equiangularLightLuminances.clear();
	for (u_int i = 0; i < lightDefs.GetSize(); ++i) {
		auto& light = lightDefs.GetLightSource(i);
		Point pos;
		switch (light.GetType()) {
			case TYPE_POINT:
			case TYPE_MAPPOINT:
			case TYPE_SPHERE:
			case TYPE_MAPSPHERE:
				pos = static_cast<const PointLight &>(light).GetAbsolutePosition();
				break;
			case TYPE_SPOT: {
				float p[3];
				static_cast<const SpotLight &>(light).GetPreprocessedData(
						NULL, p, NULL, NULL, NULL);
				pos = Point(p[0], p[1], p[2]);
				break;
			}
			case TYPE_PROJECTION: {
				float p[3];
				static_cast<const ProjectionLight &>(light).GetPreprocessedData(
						NULL, p, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
				pos = Point(p[0], p[1], p[2]);
				break;
			}
			case TYPE_LASER: {
				float p[3];
				static_cast<const LaserLight &>(light).GetPreprocessedData(
						NULL, p, NULL, NULL, NULL);
				pos = Point(p[0], p[1], p[2]);
				break;
			}
			default:
				continue;
		}
		equiangularLightPoints.push_back(pos);
		// Light power weights the contribution-aware selection
		equiangularLightLuminances.push_back(Max(0.f, light.GetPower(*this)));
	}

	//--------------------------------------------------------------------------
	// Preprocess image maps according resize policy
	//--------------------------------------------------------------------------

	imgMapCache.Preprocess(*this, useRTMode);

	//--------------------------------------------------------------------------
	// Reset the edit actions
	//--------------------------------------------------------------------------

	editActions.Reset();
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

//------------------------------------------------------------------------------
// Volume preprocess
//
// Builds the majorant grid of each heterogeneous volume using delta tracking.
// The grid domain is the union of the bounding boxes of the objects using the
// volume as interior; a volume used as exterior, camera or default world
// volume can be crossed by rays anywhere so it gets the scene bbox as domain.
//------------------------------------------------------------------------------

void Scene::PreprocessVolumes(const BBox &sceneBBox) {
	// Check if there is anything to do
	bool hasTrackingVolume = false;
	for (u_int i = 0; i < matDefs.GetSize(); ++i) {
		auto& m = matDefs.GetMaterial(i);
		if ((m.GetType() == HETEROGENEOUS_VOL) &&
				static_cast<const HeterogeneousVolume &>(m).IsDeltaTracking()) {
			hasTrackingVolume = true;
			break;
		}
	}
	if (!hasTrackingVolume)
		return;

	std::unordered_map<const Volume *, BBox> interiorDomains;
	std::unordered_set<const Volume *> unboundedVolumes;

	for (u_int i = 0; i < objDefs.GetSize(); ++i) {
		auto& obj = objDefs.GetSceneObject(i);
		auto& mat = obj.GetMaterial();

		if (auto v = mat.GetInteriorVolume())
			interiorDomains[v] = Union(interiorDomains[v], obj.GetExtMesh().GetBBox());
		if (auto v = mat.GetExteriorVolume())
			unboundedVolumes.insert(v);
	}
	if (camera && camera->HasVolume())
		unboundedVolumes.insert(&camera->GetVolume());
	if (defaultWorldVolume)
		unboundedVolumes.insert(&*defaultWorldVolume);

	for (u_int i = 0; i < matDefs.GetSize(); ++i) {
		auto& m = matDefs.GetMaterial(i);
		if (m.GetType() != HETEROGENEOUS_VOL)
			continue;
		auto& hv = static_cast<HeterogeneousVolume &>(m);
		if (!hv.IsDeltaTracking())
			continue;

		if (unboundedVolumes.count(&hv)) {
			hv.BuildMajorantGrid(sceneBBox);
		} else {
			const auto it = interiorDomains.find(&hv);
			if (it != interiorDomains.end())
				hv.BuildMajorantGrid(it->second);
			// else: the volume is not referenced by any object (e.g. only
			// reachable through per-hitPoint material ops): the grid is not
			// built and Scatter() falls back to ray marching.
		}
	}
}
