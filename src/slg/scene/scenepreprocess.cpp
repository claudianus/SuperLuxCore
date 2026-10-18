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
#include "slg/lights/lightsourcedefs.h"
#include "slg/materials/materialdefs.h"
#include "slg/scene/scene.h"
#include "slg/scene/sceneobjectdefs.h"
#include "slg/cameras/camera.h"
#include "slg/lights/pointlight.h"
#include "slg/lights/spotlight.h"
#include "slg/lights/projectionlight.h"
#include "slg/lights/laserlight.h"
#include "slg/volumes/heterogenous.h"
#include "slg/volumes/homogenous.h"
#include <array>
#include <map>
#include <numeric>
#include "slg/textures/texturedefs.h"
#include "slg/textures/hitpointfield.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Scene preprocess
//------------------------------------------------------------------------------

namespace {
// Prove one closed, consistently oriented connected shell. Exact-position
// welding joins UV/normal seams but never bridges a geometric crack.
// Bounded temporary storage; large/animated/uncertain meshes fall back.
bool IsClosedSSSShell(const ExtMesh &mesh) {
	const size_t count = mesh.GetTotalTriangleCount();
	if (!count || count > 50000 || mesh.GetTotalVertexCount() > 150000 || mesh.GetType() == TYPE_EXT_TRIANGLE_MOTION) return false;
	Transform transform;
	mesh.GetLocal2World(0.f, transform);
	std::map<std::array<float, 3>, u_int> unique;
	std::vector<u_int> vertices(mesh.GetTotalVertexCount());
	std::vector<Point> points;
	for (u_int i = 0; i < vertices.size(); ++i) {
		const Point p = mesh.GetVertex(transform, i);
		if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) return false;
		const auto [it, added] = unique.emplace(std::array<float, 3>{p.x, p.y, p.z}, unique.size());
		vertices[i] = it->second;
		if (added) points.push_back(p);
	}
	std::vector<std::array<u_int, 3>> edges;
	edges.reserve(3 * count);
	std::vector<u_int> parent(count);
	std::iota(parent.begin(), parent.end(), 0u);
	auto root = [&](u_int v) { while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; } return v; };
	const Point center = mesh.GetBBox().Center();
	double signedVolume = 0.;
	u_int face = 0;
	for (const auto &triangle : mesh.GetTriangles()) {
		u_int ids[3];
		for (unsigned i = 0; i < 3; ++i) ids[i] = vertices[triangle.v[i]];
		if (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0]) return false;
		const Vector normal = Cross(points[ids[1]] - points[ids[0]], points[ids[2]] - points[ids[0]]);
		if (!(Dot(normal, normal) > 0.f) || !isfinite(Dot(normal, normal))) return false;
		const Vector a = points[ids[0]] - center, b = points[ids[1]] - center, c = points[ids[2]] - center;
		signedVolume += double(a.x) * (double(b.y)*c.z - double(b.z)*c.y) +
				double(a.y) * (double(b.z)*c.x - double(b.x)*c.z) +
				double(a.z) * (double(b.x)*c.y - double(b.y)*c.x);
		for (unsigned i = 0; i < 3; ++i) {
			const u_int a = ids[i], b = ids[(i + 1) % 3];
			edges.push_back({Min(a,b), (Max(a,b) << 1) | (a > b), face});
		}
		++face;
	}
	if (!(signedVolume > 0.) || !isfinite(signedVolume)) return false;
	std::sort(edges.begin(), edges.end());
	for (size_t i = 0; i < edges.size(); i += 2) {
		if (i + 1 >= edges.size() || edges[i][0] != edges[i+1][0] ||
				(edges[i][1] ^ edges[i+1][1]) != 1u) return false;
		if (i + 2 < edges.size() && edges[i][0] == edges[i+2][0] &&
				(edges[i][1] >> 1) == (edges[i+2][1] >> 1)) return false;
		parent[root(edges[i+1][2])] = root(edges[i][2]);
	}
	for (u_int i = 0; i < count; ++i) if (root(i) != root(0)) return false;
	return true;
}
}

void Scene::PreprocessSSSOcclusion() {
	sssOccludedDirectVolumes.clear();
	std::map<const Volume *, BBox> candidates;
	std::unordered_set<const Volume *> rejected;
	size_t topologyBudget = 500000;
	for (u_int i = 0; i < objDefs->GetSize(); ++i) {
		const auto &object = objDefs->GetSceneObject(i);
		const auto &material = object.GetMaterial();
		// A hit-dependent volume can refer to any candidate: fail closed.
		if (material.HasVolumeOverrides()) return;
		if (material.GetExteriorVolume()) rejected.insert(material.GetExteriorVolume().get());
		const auto volume = material.GetInteriorVolume();
		if (!volume || volume->GetType() != HOMOGENEOUS_VOL ||
				!static_cast<const HomogeneousVolume *>(&*volume)->IsSSSParametrized()) continue;
		const auto &mesh = object.GetExtMesh();
		if (material.GetType() != OPENPBR || material.GetFrontTransparencyTexture() ||
				material.GetBackTransparencyTexture() || !material.GetPassThroughShadowTransparency().Black() ||
				mesh.GetBevelRadius() != 0.f || mesh.GetTotalTriangleCount() > topologyBudget || !IsClosedSSSShell(mesh)) rejected.insert(&*volume);
		topologyBudget -= Min(topologyBudget, mesh.GetTotalTriangleCount());
		candidates[&*volume] = Union(candidates[&*volume], mesh.GetBBox());
	}
	if (defaultWorldVolume) rejected.insert(&*defaultWorldVolume);
	for (const auto &[volume, box] : candidates) {
		if (rejected.count(volume)) continue;
		bool outside = true;
		for (u_int i = 0; i < lightDefs->GetSize() && outside; ++i) {
			const auto &light = lightDefs->GetLightSource(i);
			if (!light.IsDirectLightSamplingEnabled() || light.IsInfinite()) continue;
			if (light.GetType() == TYPE_TRIANGLE)
				outside = !box.Overlaps(static_cast<const TriangleLight &>(light).sceneObject->GetExtMesh().GetBBox());
			else if (light.GetType() == TYPE_POINT || light.GetType() == TYPE_MAPPOINT)
				outside = !box.Inside(static_cast<const PointLight &>(light).GetAbsolutePosition());
			else outside = false;
		}
		if (outside) sssOccludedDirectVolumes.emplace(volume, box);
	}
	SLG_LOG("SSS closed opaque NEE domains: " << sssOccludedDirectVolumes.size());
}

void Scene::PreprocessCamera(const u_int filmWidth, const u_int filmHeight, const u_int *filmSubRegion) {
	camera->Update(filmWidth, filmHeight, filmSubRegion);
}

void Scene::Preprocess(Context& ctx, const u_int filmWidth, const u_int filmHeight,
		const u_int *filmSubRegion, const bool useRTMode) {
	// Warm only meshes whose material reads Generated coordinates. Unrelated
	// proxy meshes must not fault their vertex pages in for this cache.
	bool needsGenerated = false;
	for (u_int i = 0; i < texDefs->GetSize(); ++i) {
		const Texture &tex = texDefs->GetTexture(i);
		if (tex.GetType() == HITPOINT_TEX &&
				static_cast<const HitPointFieldTexture &>(tex).GetChannel() == HITPOINT_GENERATED) {
			needsGenerated = true;
			break;
		}
	}
	if (needsGenerated) {
		std::unordered_set<const Material *> generatedMaterials;
		std::unordered_set<const Texture *> referencedTextures;
		for (u_int i = 0; i < matDefs->GetSize(); ++i) {
			const Material &mat = matDefs->GetMaterial(i);
			referencedTextures.clear();
			mat.AddReferencedTextures(referencedTextures);
			for (const Texture *tex : referencedTextures) {
				if (tex->GetType() == HITPOINT_TEX &&
						static_cast<const HitPointFieldTexture *>(tex)->GetChannel() == HITPOINT_GENERATED) {
					generatedMaterials.insert(&mat);
					break;
				}
			}
		}
		for (u_int i = 0; i < objDefs->GetSize(); ++i) {
			const SceneObject &obj = objDefs->GetSceneObject(i);
			if (generatedMaterials.count(&obj.GetMaterial())) {
				const ExtTriangleMesh *base = obj.GetExtMesh().GetAsExtTriangleMesh();
				if (base)
					base->GetGeneratedTransform();
			}
		}
	}

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

		// Out-of-core geometry spilling: swap large mesh buffers for
		// file-backed copy-on-write mappings BEFORE the data set is
		// built, so accelerators sharing the vertex buffers (Embree)
		// bind to the mapped addresses.
		if (geoSpillEnable) {
			const size_t spilled = SpillGeometryBuffers();
			if (spilled)
				SLG_LOG("Geometry spilled to disk: " <<
						spilled / (1024 * 1024) << " MB in " <<
						geoSpillLastDir);
		}

		// Rebuild the data set
		dataSet = std::make_unique<DataSet>(ctx);

		// Native curve primitives (Metal HWRT) must be disabled on meshes
		// used as light sources: light sampling draws points on the
		// triangle tessellation, so the intersected surface has to be the
		// same geometry or the larger round tube self-occludes shadow
		// rays toward the sampled points (see ExtTriangleMesh::
		// SetCurvePrimitivesEnabled). Recomputed on every data set build:
		// reset all, then disable for any object that emits light (a mesh
		// shared by multiple objects stays disabled if any is a source).
		for (u_int i = 0; i < objDefs->GetSize(); ++i) {
			if (ExtTriangleMesh *mesh = const_cast<ExtTriangleMesh *>(
					ExtTriangleMesh::FromMesh(&objDefs->GetSceneObject(i).GetExtMesh())))
				mesh->SetCurvePrimitivesEnabled(true);
		}
		for (u_int i = 0; i < objDefs->GetSize(); ++i) {
			const SceneObject &so = objDefs->GetSceneObject(i);
			if (so.GetMaterial().IsLightSource()) {
				if (ExtTriangleMesh *mesh = const_cast<ExtTriangleMesh *>(
						ExtTriangleMesh::FromMesh(&so.GetExtMesh())))
					mesh->SetCurvePrimitivesEnabled(false);
			}
		}

		// Add all objects
		for (u_int i = 0; i < objDefs->GetSize(); ++i)
			dataSet->Add(objDefs->GetSceneObject(i).GetExtMesh());

		dataSet->Preprocess();
		cachedEmbreeAccel = dataSet->GetAccelerator(ACCEL_EMBREE);

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
		lightDefs->Preprocess(*this, useRTMode);
	}

	// Rebuild on every preprocess, including edits; retain only certificates.
	PreprocessSSSOcclusion();

	// And for visibility maps
	lightDefs->UpdateVisibilityMaps(*this, useRTMode);

	//--------------------------------------------------------------------------
	// Collect the point-ish lights eligible for equiangular distance
	// sampling (position-defined lights only; directional and area lights
	// are excluded)
	//--------------------------------------------------------------------------

	equiangularLightPoints.clear();
	equiangularLightLuminances.clear();
	for (u_int i = 0; i < lightDefs->GetSize(); ++i) {
		auto& light = lightDefs->GetLightSource(i);
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

	// Out-of-core image map spilling: after resize policies and color
	// conversions are done, swap large pixel storages for file-backed
	// copy-on-write mappings. Pages become demand-paged and reclaimable.
	if (geoSpillEnable && imgSpillEnable) {
		const size_t spilled = SpillImageMaps();
		if (spilled)
			SLG_LOG("Image maps spilled to disk: " <<
					spilled / (1024 * 1024) << " MB in " <<
					geoSpillLastDir);
	}

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
	for (u_int i = 0; i < matDefs->GetSize(); ++i) {
		auto& m = matDefs->GetMaterial(i);
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

	for (u_int i = 0; i < objDefs->GetSize(); ++i) {
		auto& obj = objDefs->GetSceneObject(i);
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

	for (u_int i = 0; i < matDefs->GetSize(); ++i) {
		auto& m = matDefs->GetMaterial(i);
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
