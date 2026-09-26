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

#include <string>
#include <limits>

#if defined (_MSC_VER) || defined (__INTEL_COMPILER)
#include <intrin.h>
#endif

#include "luxrays/core/context.h"
#include "luxrays/accelerators/embreeaccel.h"
#include "luxrays/core/exttrianglemesh.h"
#include "luxrays/utils/strutils.h"
#include "luxrays/core/hardwareintersectiondevice.h"

namespace luxrays {

EmbreeAccel::EmbreeAccel(const Context & context) :
	ctx(context),
	uniqueRTCSceneByMesh(MeshPtrCompare),
	uniqueGeomByMesh(MeshPtrCompare),
	uniqueInstMatrixByMesh(MeshPtrCompare)
{
	embreeDevice = rtcNewDevice(NULL);
	embreeScene = NULL;
}

EmbreeAccel::~EmbreeAccel() {
	if (embreeScene) {
		rtcReleaseScene(embreeScene);

		// I have to free all Embree scenes used for instances
		std::pair<const Mesh * , RTCScene> elem;
		for(auto& elem: uniqueRTCSceneByMesh)
			rtcReleaseScene(elem.second);
	}

	rtcReleaseDevice(embreeDevice);
}

//--- Clustered (.lxm v2) mesh support ----------------------------------------
// A cluster-indexed mesh is exported as an Embree USER geometry whose
// primitives are the clusters: Embree builds its BVH over the stored
// cluster bounds and calls back into the mesh only for the clusters a
// ray actually reaches. Vertex/triangle pages stay unmapped until first
// traversal contact (ray-driven residency).

static void ClusterBoundsFunc(const RTCBoundsFunctionArguments *args) {
	const ExtTriangleMesh *mesh =
			static_cast<const ExtTriangleMesh *>(args->geometryUserPtr);
	const LxmCluster &cl = mesh->GetCluster(args->primID);
	RTCBounds *b = args->bounds_o;
	b->lower_x = cl.bboxMin[0]; b->lower_y = cl.bboxMin[1]; b->lower_z = cl.bboxMin[2];
	b->upper_x = cl.bboxMax[0]; b->upper_y = cl.bboxMax[1]; b->upper_z = cl.bboxMax[2];
}

static void ClusterIntersectFunc(const RTCIntersectFunctionNArguments *args) {
	const ExtTriangleMesh *mesh =
			static_cast<const ExtTriangleMesh *>(args->geometryUserPtr);
	const LxmCluster &cl = mesh->GetCluster(args->primID);
	const Triangle *tris = mesh->GetTriangles().data();

	// Ray packets are SoA (see RTCRayHitNt); rtcIntersect1 uses N == 1.
	RTCRayHitNt<1> *rh = reinterpret_cast<RTCRayHitNt<1> *>(args->rayhit);
	for (u_int n = 0; n < args->N; ++n) {
		if (!args->valid[n])
			continue;

		Ray ray(Point(rh->ray.org_x[n], rh->ray.org_y[n], rh->ray.org_z[n]),
				Vector(rh->ray.dir_x[n], rh->ray.dir_y[n], rh->ray.dir_z[n]),
				rh->ray.tnear[n], rh->ray.tfar[n]);
		for (u_int j = cl.firstTri, e = cl.firstTri + cl.triCount; j < e; ++j) {
			const Triangle &t3 = tris[j];
			float t, b1, b2;
			if (Triangle::Intersect(ray,
					mesh->GetVertex(Transform::TRANS_IDENTITY, t3.v[0]),
					mesh->GetVertex(Transform::TRANS_IDENTITY, t3.v[1]),
					mesh->GetVertex(Transform::TRANS_IDENTITY, t3.v[2]),
					&t, &b1, &b2)) {
				rh->ray.tfar[n] = t;
				ray.maxt = t;
				rh->hit.u[n] = b1;
				rh->hit.v[n] = b2;
				// Report the real triangle index (not the cluster id) so
				// shading lookups stay identical to the flat build.
				rh->hit.primID[n] = j;
				rh->hit.geomID[n] = args->geomID;
			}
		}
	}
}

static void ClusterOccludedFunc(const RTCOccludedFunctionNArguments *args) {
	const ExtTriangleMesh *mesh =
			static_cast<const ExtTriangleMesh *>(args->geometryUserPtr);
	const LxmCluster &cl = mesh->GetCluster(args->primID);
	const Triangle *tris = mesh->GetTriangles().data();

	RTCRayNt<1> *r = reinterpret_cast<RTCRayNt<1> *>(args->ray);
	for (u_int n = 0; n < args->N; ++n) {
		if (!args->valid[n])
			continue;

		Ray ray(Point(r->org_x[n], r->org_y[n], r->org_z[n]),
				Vector(r->dir_x[n], r->dir_y[n], r->dir_z[n]),
				r->tnear[n], r->tfar[n]);
		for (u_int j = cl.firstTri, e = cl.firstTri + cl.triCount; j < e; ++j) {
			const Triangle &t3 = tris[j];
			float t, b1, b2;
			if (Triangle::Intersect(ray,
					mesh->GetVertex(Transform::TRANS_IDENTITY, t3.v[0]),
					mesh->GetVertex(Transform::TRANS_IDENTITY, t3.v[1]),
					mesh->GetVertex(Transform::TRANS_IDENTITY, t3.v[2]),
					&t, &b1, &b2)) {
				// Embree occluded convention: signal the hit by setting
				// tfar to -inf.
				r->tfar[n] = -std::numeric_limits<float>::infinity();
				break;
			}
		}
	}
}

void EmbreeAccel::ExportClusteredTriangleMesh(const RTCScene embreeScene,
		const ExtTriangleMesh &mesh) const {
	const RTCGeometry geom = rtcNewGeometry(embreeDevice, RTC_GEOMETRY_TYPE_USER);

	rtcSetGeometryUserPrimitiveCount(geom, mesh.GetClusterIndexCount());
	rtcSetGeometryUserData(geom,
			const_cast<ExtTriangleMesh *>(&mesh));
	rtcSetGeometryBoundsFunction(geom, ClusterBoundsFunc, nullptr);
	rtcSetGeometryIntersectFunction(geom, ClusterIntersectFunc);
	rtcSetGeometryOccludedFunction(geom, ClusterOccludedFunc);

	rtcCommitGeometry(geom);
	rtcAttachGeometry(embreeScene, geom);
	rtcReleaseGeometry(geom);
}

void EmbreeAccel::ExportTriangleMesh(const RTCScene embreeScene, MeshConstRef mesh) const {
	const ExtTriangleMesh *extMesh = ExtTriangleMesh::FromMesh(&mesh);
	if (extMesh && extMesh->HasClusterIndex() && !extMesh->HasVertexMotion()) {
		ExportClusteredTriangleMesh(embreeScene, *extMesh);
		return;
	}

	const RTCGeometry geom = rtcNewGeometry(embreeDevice, RTC_GEOMETRY_TYPE_TRIANGLE);

	// Per-vertex deformation motion: hand every step buffer to Embree as
	// an additional vertex timestep. Embree distributes timesteps
	// uniformly over the ray-time interval, so non-uniform step times are
	// approximated (same convention as the Metal HWRT path).
	if (extMesh && extMesh->HasVertexMotion()) {
		const u_int stepCount = extMesh->GetVertexMotionStepCount();
		if (stepCount > RTC_MAX_TIME_STEP_COUNT)
			throw std::runtime_error("Embree accelerator supports up to " +
					ToString(RTC_MAX_TIME_STEP_COUNT) +
					" motion blur steps, unable to use " + ToString(stepCount));

		rtcSetGeometryTimeStepCount(geom, stepCount);
		for (u_int step = 0; step < stepCount; ++step) {
			const auto &stepVerts = extMesh->GetVertexMotionStep(step);
			rtcSetSharedGeometryBuffer(
				geom,
				RTC_BUFFER_TYPE_VERTEX,
				step,
				RTC_FORMAT_FLOAT3,
				stepVerts.GetObjects().data(),
				0,
				sizeof(Point),
				extMesh->GetTotalVertexCount());
		}
	} else {
		// Share with Embree the mesh vertices
		auto meshVerts = mesh.GetVertices();
		rtcSetSharedGeometryBuffer(
			geom,
			RTC_BUFFER_TYPE_VERTEX,
			0,
			RTC_FORMAT_FLOAT3,
			meshVerts.data(),
			0,
			sizeof(Point),
			mesh.GetTotalVertexCount());
	}


	// Share with Embree the mesh triangles
	auto meshTris = mesh.GetTriangles();
	rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, meshTris.data(),
			0, sizeof(Triangle), mesh.GetTotalTriangleCount());

	rtcCommitGeometry(geom);

	rtcAttachGeometry(embreeScene, geom);

	rtcReleaseGeometry(geom);

}

void EmbreeAccel::ExportMotionTriangleMesh(const RTCScene embreeScene, const MotionTriangleMesh & mtm) const {
	auto &ms = mtm.GetMotionSystem();

	// Check if I would need more than the max. number of steps (i.e. 129) supported by Embree
	if (ms.times.size() > RTC_MAX_TIME_STEP_COUNT)
		throw std::runtime_error("Embree accelerator supports up to " + ToString(RTC_MAX_TIME_STEP_COUNT) +
				" motion blur steps, unable to use " + ToString(ms.times.size()));

	const RTCGeometry geom = rtcNewGeometry(embreeDevice, RTC_GEOMETRY_TYPE_TRIANGLE);
	rtcSetGeometryTimeStepCount(geom, ms.times.size());

	// The base mesh may itself carry a vertex-motion series: sample it at
	// the same step times so transform and deformation compose.
	const ExtTriangleMesh *extMesh = ExtTriangleMesh::FromMesh(&mtm);
	const bool hasVertMotion = extMesh && extMesh->HasVertexMotion();

	for (u_int step = 0; step < ms.times.size(); ++step) {
		// Copy the mesh start position vertices
		Point *vertices = (Point *)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, step, RTC_FORMAT_FLOAT3,
				sizeof(Point), mtm.GetTotalVertexCount());

		Transform local2World;
		mtm.GetLocal2World(ms.times[step], local2World);
		if (hasVertMotion) {
			for (u_int i = 0; i < mtm.GetTotalVertexCount(); ++i)
				vertices[i] = local2World * extMesh->GetVertexAtTime(i, ms.times[step]);
		} else {
			for (u_int i = 0; i < mtm.GetTotalVertexCount(); ++i)
				vertices[i] = mtm.GetVertex(local2World, i);
		}
	}

	// Share the mesh triangles
	auto meshTris = mtm.GetTriangles();
	rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, (RTCBuffer)meshTris.data(),
			0, sizeof(Triangle), mtm.GetTotalTriangleCount());

	rtcCommitGeometry(geom);

	rtcAttachGeometry(embreeScene, geom);

	rtcReleaseGeometry(geom);

}

void EmbreeAccel::Init(
	const std::deque<const Mesh *> &meshes,
	const u_longlong totalVertexCount,
	const u_longlong totalTriangleCount
) {
	const double t0 = WallClockTime();

	//--------------------------------------------------------------------------
	// Extract the meshes min. and max. time. To normalize between 0.f and 1.f.
	//--------------------------------------------------------------------------

	minTime = std::numeric_limits<float>::max();
	maxTime = std::numeric_limits<float>::min();
	for(auto* mesh: meshes) {
		auto* mtm = dynamic_cast<const MotionTriangleMesh *>(mesh);

		if (mtm) {
			minTime = Min(minTime, mtm->GetMotionSystem().StartTime());
			maxTime = Max(maxTime, mtm->GetMotionSystem().EndTime());
		}

		// Per-vertex deformation series have their own shutter times
		const ExtTriangleMesh *extMesh = ExtTriangleMesh::FromMesh(mesh);
		if (extMesh && extMesh->HasVertexMotion()) {
			const auto &times = extMesh->GetVertexMotionTimes();
			minTime = Min(minTime, times.front());
			maxTime = Max(maxTime, times.back());
		}
	}

	if ((minTime == std::numeric_limits<float>::max()) ||
			(maxTime == std::numeric_limits<float>::min())) {
		minTime = 0.f;
		maxTime = 1.f;
		timeScale = 1.f;
	} else
		timeScale = 1.f / (maxTime - minTime);

	//--------------------------------------------------------------------------
	// Convert the meshes to an Embree Scene
	//--------------------------------------------------------------------------

	embreeScene = rtcNewScene(embreeDevice);
	rtcSetSceneBuildQuality(embreeScene, RTC_BUILD_QUALITY_HIGH);
	rtcSetSceneFlags(embreeScene, RTC_SCENE_FLAG_DYNAMIC);

	for(auto* ptr: meshes) {
		auto& mesh = *ptr;
		switch (mesh.GetType()) {
			case TYPE_TRIANGLE:
			case TYPE_EXT_TRIANGLE:
				ExportTriangleMesh(embreeScene, mesh);
				break;
			case TYPE_TRIANGLE_INSTANCE:
			case TYPE_EXT_TRIANGLE_INSTANCE: {
				auto& itm = dynamic_cast<const InstanceTriangleMesh&>(mesh);

				// Check if a RTCScene has already been created
				auto it = uniqueRTCSceneByMesh.find(&itm.GetTriangleMesh());

				RTCScene instScene;
				if (it == uniqueRTCSceneByMesh.end()) {
					auto& instancedMesh = itm.GetTriangleMesh();

					// Create a new RTCScene
					instScene = rtcNewScene(embreeDevice);
					ExportTriangleMesh(instScene, instancedMesh);
					rtcCommitScene(instScene);

					uniqueRTCSceneByMesh[&instancedMesh] = instScene;
				} else
					instScene = it->second;

				RTCGeometry geom = rtcNewGeometry(embreeDevice, RTC_GEOMETRY_TYPE_INSTANCE);
				rtcSetGeometryInstancedScene(geom, instScene);
				rtcSetGeometryTransform(geom, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, &(itm.GetTransformation().m.m[0][0]));
				rtcCommitGeometry(geom);

				rtcAttachGeometry(embreeScene, geom);

				rtcReleaseGeometry(geom);

				// Save the instance ID
				uniqueGeomByMesh[&mesh] = geom;
				// Save the matrix
				uniqueInstMatrixByMesh[&mesh] = itm.GetTransformation().m;
				break;
			}
			case TYPE_TRIANGLE_MOTION:
			case TYPE_EXT_TRIANGLE_MOTION: {
				auto& mtm = dynamic_cast<const MotionTriangleMesh &>(mesh);
				ExportMotionTriangleMesh(embreeScene, mtm);
				break;
			}
			default:
				throw std::runtime_error("Unknown Mesh type in EmbreeAccel::Init(): " + ToString(mesh.GetType()));
		}
	}

	rtcCommitScene(embreeScene);

	LR_LOG(ctx, "EmbreeAccel build time: " << int((WallClockTime() - t0) * 1000) << "ms");
}

void EmbreeAccel::Update() {
	// Update all Embree scenes used for instances
	bool updated = false;
	std::pair<const Mesh *, RTCGeometry> elem;
	for(auto& elem: uniqueGeomByMesh) {
		auto& itm = dynamic_cast<const InstanceTriangleMesh &>(*elem.first);

		// Check if the transformation has changed
		if (uniqueInstMatrixByMesh[elem.first] != itm.GetTransformation().m) {
			rtcSetGeometryTransform(elem.second, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, &(itm.GetTransformation().m.m[0][0]));
			rtcCommitGeometry(elem.second);
			updated = true;
		}
	}

	if (updated)
		rtcCommitScene(embreeScene);
}

bool EmbreeAccel::MeshPtrCompare(const Mesh * p0, const Mesh * p1) {
	return p0 < p1;
}

bool EmbreeAccel::Intersect(const Ray *ray, RayHit *hit) const {

	RTCRayHit embreeRayHit;

	if (isnan(ray->o.x) || isnan(ray->o.y) || isnan(ray->o.z) || isnan(ray->d.x) || isnan(ray->d.y) || isnan(ray->d.z)) {
		return false;
	}

	embreeRayHit.ray.org_x = ray->o.x;
	embreeRayHit.ray.org_y = ray->o.y;
	embreeRayHit.ray.org_z = ray->o.z;

	embreeRayHit.ray.dir_x = ray->d.x;
	embreeRayHit.ray.dir_y = ray->d.y;
	embreeRayHit.ray.dir_z = ray->d.z;

	embreeRayHit.ray.tnear = ray->mint;
	embreeRayHit.ray.tfar = ray->maxt;

	embreeRayHit.ray.mask = 0xFFFFFFFF;
	// Clamp so out-of-range shutter times sample the boundary poses, like
	// the BVH/MBVH and OpenCL motion paths do
	embreeRayHit.ray.time = Clamp((ray->time - minTime) * timeScale, 0.f, 1.f);

	embreeRayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
	embreeRayHit.hit.primID = RTC_INVALID_GEOMETRY_ID;
	embreeRayHit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
	
	rtcIntersect1(embreeScene, &embreeRayHit);

	if ((embreeRayHit.hit.geomID != RTC_INVALID_GEOMETRY_ID) &&
			// A safety check in case of not enough numerical precision. Embree
			// can return some intersection out of [mint, maxt] range for
			// some extremely large floating point number.
			(embreeRayHit.ray.tfar >= ray->mint) && (embreeRayHit.ray.tfar <= ray->maxt)) {
		hit->meshIndex = (embreeRayHit.hit.instID[0] == RTC_INVALID_GEOMETRY_ID) ? embreeRayHit.hit.geomID : embreeRayHit.hit.instID[0];
		hit->triangleIndex = embreeRayHit.hit.primID;

		hit->t = embreeRayHit.ray.tfar;

		hit->b1 = embreeRayHit.hit.u;
		hit->b2 = embreeRayHit.hit.v;

		return true;
	} else {
		return false;
	}
}

HardwareIntersectionKernelUPtr EmbreeAccel::NewHardwareIntersectionKernel(
	HardwareIntersectionDevice &device
) const {
	return HardwareIntersectionKernelUPtr(nullptr);
}

}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
