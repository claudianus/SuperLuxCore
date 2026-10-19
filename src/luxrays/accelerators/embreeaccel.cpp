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
#include <cmath>
#include <cstdlib>

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

// Diagnostic precision path. Keep the native path as default until the full
// renderer/performance gates cover this callback, motion and clustered meshes.
static bool PreciseTriangleHit(const Mesh &mesh, const u_int primitive,
		const double *origin, const double *direction, const double minT,
		const double maxT, float *t, float *b1, float *b2) {
	const Triangle &tri = mesh.GetTriangles()[primitive];
	const auto vertices = mesh.GetVertices();
	double p[3][3];
	for (int i = 0; i < 3; ++i) {
		const Point &v = vertices[tri.v[i]];
		p[i][0] = double(v.x) - origin[0];
		p[i][1] = double(v.y) - origin[1];
		p[i][2] = double(v.z) - origin[2];
	}
	int kz = 0;
	if (std::abs(direction[1]) > std::abs(direction[kz])) kz = 1;
	if (std::abs(direction[2]) > std::abs(direction[kz])) kz = 2;
	if (direction[kz] == 0.) return false;
	int kx = (kz + 1) % 3, ky = (kx + 1) % 3;
	if (direction[kz] < 0.) std::swap(kx, ky);
	const double sx = direction[kx] / direction[kz], sy = direction[ky] / direction[kz];
	double x[3], y[3], z[3];
	for (int i = 0; i < 3; ++i) {
		x[i] = p[i][kx] - sx * p[i][kz];
		y[i] = p[i][ky] - sy * p[i][kz];
		z[i] = p[i][kz] / direction[kz];
	}
	const double e0 = x[1]*y[2] - y[1]*x[2];
	const double e1 = x[2]*y[0] - y[2]*x[0];
	const double e2 = x[0]*y[1] - y[0]*x[1];
	if (((e0 < 0.) || (e1 < 0.) || (e2 < 0.)) &&
			((e0 > 0.) || (e1 > 0.) || (e2 > 0.))) return false;
	const double det = e0 + e1 + e2;
	if (det == 0. || !std::isfinite(det)) return false;
	const double distance = (e0*z[0] + e1*z[1] + e2*z[2]) / det;
	if (!(distance >= minT && distance <= maxT) || !std::isfinite(distance)) return false;
	*t = float(distance); *b1 = float(e1 / det); *b2 = float(e2 / det);
	return true;
}

static void PreciseTriangleBounds(const RTCBoundsFunctionArguments *args) {
	const Mesh &mesh = *static_cast<const Mesh *>(args->geometryUserPtr);
	const Triangle &tri = mesh.GetTriangles()[args->primID];
	const auto vertices = mesh.GetVertices();
	const Point &a = vertices[tri.v[0]], &b = vertices[tri.v[1]], &c = vertices[tri.v[2]];
	const float inf = std::numeric_limits<float>::infinity();
	args->bounds_o->lower_x = std::nextafter(std::min(a.x, std::min(b.x,c.x)), -inf);
	args->bounds_o->lower_y = std::nextafter(std::min(a.y, std::min(b.y,c.y)), -inf);
	args->bounds_o->lower_z = std::nextafter(std::min(a.z, std::min(b.z,c.z)), -inf);
	args->bounds_o->upper_x = std::nextafter(std::max(a.x, std::max(b.x,c.x)), inf);
	args->bounds_o->upper_y = std::nextafter(std::max(a.y, std::max(b.y,c.y)), inf);
	args->bounds_o->upper_z = std::nextafter(std::max(a.z, std::max(b.z,c.z)), inf);
}

static void PreciseTriangleIntersect(const RTCIntersectFunctionNArguments *args) {
	const Mesh &mesh = *static_cast<const Mesh *>(args->geometryUserPtr);
	RTCRayN *ray = RTCRayHitN_RayN(args->rayhit, args->N);
	RTCHitN *hit = RTCRayHitN_HitN(args->rayhit, args->N);
	for (u_int i = 0; i < args->N; ++i) {
		if (!args->valid[i]) continue;
		const double origin[] = {RTCRayN_org_x(ray,args->N,i), RTCRayN_org_y(ray,args->N,i), RTCRayN_org_z(ray,args->N,i)};
		const double direction[] = {RTCRayN_dir_x(ray,args->N,i), RTCRayN_dir_y(ray,args->N,i), RTCRayN_dir_z(ray,args->N,i)};
		float t, b1, b2;
		if (!PreciseTriangleHit(mesh, args->primID, origin, direction,
				RTCRayN_tnear(ray,args->N,i), RTCRayN_tfar(ray,args->N,i), &t, &b1, &b2)) continue;
		RTCRayN_tfar(ray,args->N,i) = t;
		RTCHitN_u(hit,args->N,i) = b1; RTCHitN_v(hit,args->N,i) = b2;
		RTCHitN_primID(hit,args->N,i) = args->primID;
		RTCHitN_geomID(hit,args->N,i) = args->geomID;
		const auto verts = mesh.GetVertices();
		const Triangle &tri = mesh.GetTriangles()[args->primID];
		const Point &a = verts[tri.v[0]], &b = verts[tri.v[1]], &c = verts[tri.v[2]];
		const double ax = double(b.x)-a.x, ay = double(b.y)-a.y, az = double(b.z)-a.z;
		const double bx = double(c.x)-a.x, by = double(c.y)-a.y, bz = double(c.z)-a.z;
		const double nx = ay*bz-az*by, ny = az*bx-ax*bz, nz = ax*by-ay*bx;
		const double length = std::hypot(nx,ny,nz);
		RTCHitN_Ng_x(hit,args->N,i) = float(nx/length);
		RTCHitN_Ng_y(hit,args->N,i) = float(ny/length);
		RTCHitN_Ng_z(hit,args->N,i) = float(nz/length);
		for (u_int l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l)
			RTCHitN_instID(hit,args->N,i,l) = args->context->instID[l];
	}
}

static void PreciseTriangleOccluded(const RTCOccludedFunctionNArguments *args) {
	const Mesh &mesh = *static_cast<const Mesh *>(args->geometryUserPtr);
	RTCRayN *ray = args->ray;
	for (u_int i = 0; i < args->N; ++i) {
		if (!args->valid[i]) continue;
		const double origin[] = {RTCRayN_org_x(ray,args->N,i), RTCRayN_org_y(ray,args->N,i), RTCRayN_org_z(ray,args->N,i)};
		const double direction[] = {RTCRayN_dir_x(ray,args->N,i), RTCRayN_dir_y(ray,args->N,i), RTCRayN_dir_z(ray,args->N,i)};
		float t, b1, b2;
		if (PreciseTriangleHit(mesh, args->primID, origin, direction,
				RTCRayN_tnear(ray,args->N,i), RTCRayN_tfar(ray,args->N,i), &t, &b1, &b2))
			RTCRayN_tfar(ray,args->N,i) = -std::numeric_limits<float>::infinity();
	}
}

void EmbreeAccel::ExportTriangleMesh(const RTCScene embreeScene, MeshConstRef mesh) const {
	const ExtTriangleMesh *extMesh = ExtTriangleMesh::FromMesh(&mesh);
	if (extMesh && extMesh->HasClusterIndex() && !extMesh->HasVertexMotion()) {
		ExportClusteredTriangleMesh(embreeScene, *extMesh);
		return;
	}
	const char *precise = std::getenv("LUXRAYS_EMBREE_PRECISE");
	if (precise && std::string(precise) == "1" && !(extMesh && extMesh->HasVertexMotion())) {
		const RTCGeometry geom = rtcNewGeometry(embreeDevice, RTC_GEOMETRY_TYPE_USER);
		rtcSetGeometryUserPrimitiveCount(geom, mesh.GetTotalTriangleCount());
		rtcSetGeometryUserData(geom, const_cast<Mesh *>(&mesh));
		rtcSetGeometryBoundsFunction(geom, PreciseTriangleBounds, nullptr);
		rtcSetGeometryIntersectFunction(geom, PreciseTriangleIntersect);
		rtcSetGeometryOccludedFunction(geom, PreciseTriangleOccluded);
		rtcCommitGeometry(geom); rtcAttachGeometry(embreeScene, geom); rtcReleaseGeometry(geom);
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
