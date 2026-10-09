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

#include <boost/format.hpp>
#include <unordered_map>

#include "luxrays/core/exttrianglemesh.h"
#include "slg/shapes/displacement.h"
#include "luxrays/core/trianglemesh.h"
#include "luxrays/core/color/spectral.h"
#include "slg/scene/scene.h"
#include "slg/textures/texture.h"

using namespace std;
using namespace luxrays;
using namespace slg;

DisplacementShape::DisplacementShape(luxrays::ExtTriangleMeshRef srcMesh, const Texture &dispMap,
		const Params &params) {
	SDL_LOG("Displacement shape " << srcMesh.GetName() << " with texture " << dispMap.GetName());

	// I need vertex normals
	if (!srcMesh.HasNormals())
		srcMesh.ComputeNormals();

	// I need vertex UVs for vector displacement
	if ((params.mapType == VECTOR_DISPLACEMENT) &&
			(params.mapSpace == NATIVE_SPACE) && !srcMesh.HasUVs(params.uvIndex))
		throw runtime_error("Displacement shape for vector displacement can be used only with mesh having UVs defined");
	const auto validIndex = [](const u_int index) {
		return (index == NULL_INDEX) || (index < EXTMESH_MAX_DATA_COUNT);
	};
	if (!validIndex(params.normalIndex) || !validIndex(params.tangentIndex) ||
			!validIndex(params.signIndex) || !validIndex(params.vertexIDLowIndex) ||
			!validIndex(params.vertexIDHighIndex))
		throw runtime_error("Invalid displacement data index on mesh: " + srcMesh.GetName());
	if (((params.normalIndex != NULL_INDEX) && !srcMesh.HasColors(params.normalIndex)) ||
			((params.tangentIndex != NULL_INDEX) && !srcMesh.HasColors(params.tangentIndex)) ||
			((params.signIndex != NULL_INDEX) && !srcMesh.HasAlphas(params.signIndex)) ||
			((params.vertexIDLowIndex != NULL_INDEX) && !srcMesh.HasVertexAOV(params.vertexIDLowIndex)) ||
			((params.vertexIDHighIndex != NULL_INDEX) && !srcMesh.HasVertexAOV(params.vertexIDHighIndex)))
		throw runtime_error("Missing displacement normal/tangent/sign data on mesh: " + srcMesh.GetName());
	if ((params.vertexIDLowIndex == NULL_INDEX) != (params.vertexIDHighIndex == NULL_INDEX))
		throw runtime_error("Displacement vertex identity requires both data channels: " + srcMesh.GetName());

	const double startTime = WallClockTime();

	const u_int vertCount = srcMesh.GetTotalVertexCount();
	const auto vertices = srcMesh.GetVertices();
	VertexBuffer newVertices(vertCount);

	// I need to build the dpdu, dpdv, dndu, dndv for each vertex. They are mostly
	// used for vector displacement but they may be used by the texture too.
	vector<Vector> dpdu(vertCount);
	vector<Vector> dpdv(vertCount);
	vector<Normal> dndu(vertCount);
	vector<Normal> dndv(vertCount);

	vector<u_int> triangleIndex(vertCount);
	vector<u_int> representative;
	vector<bool> smoothCorner;
	unordered_map<u_int, u_int> firstVertex;
	if (params.vertexIDLowIndex != NULL_INDEX) {
		representative.resize(vertCount);
		if (params.vertexIDSmoothFlag)
			smoothCorner.resize(vertCount);
	}

	// Go trough the faces and save the information
	vector<bool> doneVerts(vertCount, false);
	const auto triCount = srcMesh.GetTotalTriangleCount();
	const auto tris(srcMesh.GetTriangles());
	for (u_int i = 0; i < triCount; ++i) {
		const Triangle &tri = tris[i];

		for (u_int j = 0; j < 3; ++j) {
			const u_int vertIndex = tri.v[j];

			if (!doneVerts[vertIndex]) {
				const Normal shadeN = srcMesh.GetShadeNormal(Transform::TRANS_IDENTITY, vertIndex);

				// Compute geometry differentials
				srcMesh.GetDifferentials(Transform::TRANS_IDENTITY, i, shadeN, params.uvIndex,
						&dpdu[vertIndex], &dpdv[vertIndex],
						&dndu[vertIndex], &dndv[vertIndex]);

				triangleIndex[vertIndex] = i;
				if (!representative.empty()) {
					// Cycles mesh_displace.cpp evaluates the first triangle corner
					// of each original vertex, then moves all shading/UV copies
					// together. The low word can also carry the corner smooth flag.
					const float lowValue = srcMesh.GetVertexAOV(vertIndex, params.vertexIDLowIndex);
					const float highValue = srcMesh.GetVertexAOV(vertIndex, params.vertexIDHighIndex);
					if (!isfinite(lowValue) || !isfinite(highValue) ||
							(lowValue < 0.f) || (lowValue > (params.vertexIDSmoothFlag ? 131071.f : 65535.f)) ||
							(highValue < 0.f) || (highValue > 65535.f) ||
							(floorf(lowValue) != lowValue) || (floorf(highValue) != highValue))
						throw runtime_error("Invalid displacement vertex identity on mesh: " + srcMesh.GetName());
					const u_int low = static_cast<u_int>(lowValue);
					const u_int high = static_cast<u_int>(highValue);
					const u_int id = (high << 16) | (low & 0xffffu);
					if (!smoothCorner.empty())
						smoothCorner[vertIndex] = (low & 0x10000u) != 0;
					representative[vertIndex] = firstVertex.emplace(id, vertIndex).first->second;
				}

				doneVerts[vertIndex] = true;
			}
		}
	}

	const u_int binormalIndex = params.mapChannels[0];
	const u_int tangentIndex = params.mapChannels[1];
	const u_int normalIndex = params.mapChannels[2];

	#pragma omp parallel for
	for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int i = 0; i < vertCount; ++i) {
		if (!representative.empty() && (representative[i] != i))
			continue;
		HitPoint hitPoint;
		hitPoint.Init();
		// Coordinates are RGB/vector data even when a caller has spectral TLS.
		Spectral::ScopePause dataEvaluation;
		
		hitPoint.fixedDir = Vector(0.f, 0.f, 1.f);
		hitPoint.p = srcMesh.GetVertex(Transform::TRANS_IDENTITY, i);

		hitPoint.geometryN = srcMesh.GetShadeNormal(Transform::TRANS_IDENTITY, i);
		hitPoint.interpolatedN = hitPoint.geometryN;
		hitPoint.shadeN = hitPoint.interpolatedN;

		hitPoint.defaultUV = srcMesh.HasUVs(params.uvIndex) ? srcMesh.GetUV(i, params.uvIndex) : UV(0.f, 0.f);
		hitPoint.mesh = &srcMesh;
		hitPoint.triangleIndex = triangleIndex[i];
		if (i == tris[hitPoint.triangleIndex].v[0]) {
			// First vertex of the triangle
			hitPoint.triangleBariCoord1 = 0.f;
			hitPoint.triangleBariCoord2 = 0.f;
		} else if (i == tris[hitPoint.triangleIndex].v[1]) {
			// Second vertex of the triangle
			hitPoint.triangleBariCoord1 = 1.f;
			hitPoint.triangleBariCoord2 = 0.f;
		} else {
			// Last vertex of the triangle
			hitPoint.triangleBariCoord1 = 0.f;
			hitPoint.triangleBariCoord2 = 1.f;
		}

		hitPoint.dpdu = dpdu[i];
		hitPoint.dpdv = dpdv[i];
		hitPoint.dndu = dndu[i];
		hitPoint.dndv = dndv[i];

		hitPoint.passThroughEvent = 0.f;
		srcMesh.GetLocal2World(0.f, hitPoint.localToWorld);
		hitPoint.interiorVolume = nullptr;
		hitPoint.exteriorVolume = nullptr;
		hitPoint.objectID = 0;
		hitPoint.fromLight = false;
		hitPoint.intoObject = true;
		hitPoint.throughShadowTransparency = false;
		const Normal objectNormal = hitPoint.shadeN;
		if (params.mapSpace != NATIVE_SPACE) {
			// Displacement textures see the original world position, while the
			// generated geometry remains local to this particular object instance.
			hitPoint.localToWorld = params.objectToWorld;
			hitPoint.p = params.objectToWorld * hitPoint.p;
			hitPoint.geometryN = Normalize(params.objectToWorld *
					srcMesh.GetGeometryNormal(Transform::TRANS_IDENTITY, hitPoint.triangleIndex));
			hitPoint.interpolatedN = Normalize(params.objectToWorld * objectNormal);
			hitPoint.shadeN = hitPoint.interpolatedN;
			hitPoint.dpdu = params.objectToWorld * hitPoint.dpdu;
			hitPoint.dpdv = params.objectToWorld * hitPoint.dpdv;
			hitPoint.dndu = params.objectToWorld * hitPoint.dndu;
			hitPoint.dndv = params.objectToWorld * hitPoint.dndv;
		}
		hitPoint.coatN = hitPoint.shadeN;

		Vector disp;
		if (params.mapType == HIGHT_DISPLACEMENT)
			disp = (dispMap.GetFloatValue(hitPoint) * params.scale + params.offset) *
					Vector(hitPoint.shadeN);
		else if (params.mapSpace != NATIVE_SPACE) {
			const Spectrum value = dispMap.GetSpectrumValue(hitPoint) * params.scale;
			disp = Vector(value.c[0], value.c[1], value.c[2]);
			if (params.mapSpace == WORLD_SPACE)
				disp = Inverse(params.objectToWorld) * disp;
			else if (params.mapSpace == TANGENT_SPACE) {
				Vector n = Vector(objectNormal);
				if (params.normalIndex != NULL_INDEX) {
					const Spectrum raw = srcMesh.GetColor(i, params.normalIndex);
					n = Vector(raw.c[0], raw.c[1], raw.c[2]);
				}
				// Match Cycles' MikkTSpace corner attribute and signed bitangent.
				// Its no-attribute fallback uses the shading dPdu direction.
				const Triangle &tri = tris[hitPoint.triangleIndex];
				const Vector edge = params.objectToWorld * (vertices[tri.v[1]] - vertices[tri.v[0]]);
				const float tangentLength2 = Dot(edge, edge);
				Vector tangent = (tangentLength2 > 0.f) ? edge / sqrtf(tangentLength2) : Vector();
				if (params.tangentIndex != NULL_INDEX) {
					const Spectrum raw = srcMesh.GetColor(i, params.tangentIndex);
					tangent = Vector(raw.c[0], raw.c[1], raw.c[2]);
				}
				Vector bitangent = Cross(n, tangent);
				const float length2 = Dot(bitangent, bitangent);
				bitangent = (length2 > 0.f) ? bitangent / sqrtf(length2) : Vector();
				if (params.signIndex != NULL_INDEX)
					bitangent *= srcMesh.GetAlpha(i, params.signIndex);
				disp = tangent * disp.x + n * disp.y + bitangent * disp.z;
			}
		} else {
			// Not using dispOffset parameter because it doesn't make very much sense
			// for vector displacement
			const Spectrum dispValue = dispMap.GetSpectrumValue(hitPoint) * params.scale;

			// Build the local reference system, uses shadeN, dpdu and dpdv
			const Frame frame = hitPoint.GetFrame();

			disp = frame.ToWorld(Vector(dispValue.c[binormalIndex], dispValue.c[tangentIndex], dispValue.c[normalIndex]));


			// I work on tangent space and in this case: R is an offset along
			// the tangent, G along the normal and B along the bitangent.
			// This is the Blender standard.
			//disp = frame.ToWorld(Vector(dispValue.c[2], dispValue.c[0], dispValue.c[1]));

			// This is the Mudbox standard.
			//disp = frame.ToWorld(Vector(dispValue.c[0], dispValue.c[2], dispValue.c[1]));
		}

		newVertices[i] = vertices[i] + disp;
	}
	if (!representative.empty()) {
		// The parallel evaluation above has completed before copies read it.
		#pragma omp parallel for
		for (int i = 0; i < vertCount; ++i) {
			const u_int first = representative[i];
			if (first != i)
				newVertices[i] = vertices[i] + (newVertices[first] - vertices[first]);
		}
	}

	optional<NormalBuffer> displacedNormals;
	if (params.normalSmooth && !smoothCorner.empty()) {
		vector<Vector> before(vertCount), after(vertCount);
		vector<Normal> faceNormals(triCount);
		const auto unit = [](const Vector &v) {
			const float length2 = Dot(v, v);
			return length2 > 0.f ? v / sqrtf(length2) : Vector();
		};
		for (u_int i = 0; i < triCount; ++i) {
			const Triangle &tri = tris[i];
			const Vector pre = unit(Cross(vertices[tri.v[1]] - vertices[tri.v[0]],
					vertices[tri.v[2]] - vertices[tri.v[0]]));
			const Vector post = unit(Cross(newVertices[tri.v[1]] - newVertices[tri.v[0]],
					newVertices[tri.v[2]] - newVertices[tri.v[0]]));
			faceNormals[i] = Normal(post);
			for (u_int j = 0; j < 3; ++j) {
				const u_int first = representative[tri.v[j]];
				before[first] += pre;
				after[first] += post;
			}
		}
		displacedNormals.emplace(vertCount);
		for (u_int i = 0; i < vertCount; ++i) {
			if (smoothCorner[i]) {
				const u_int first = representative[i];
				if (params.normalDelta) {
					const Vector delta = unit(after[first]) - unit(before[first]);
					(*displacedNormals)[i] = Normal(unit(Vector(srcMesh.GetShadeNormal(Transform::TRANS_IDENTITY, i)) + delta));
				} else
					(*displacedNormals)[i] = Normal(unit(after[first]));
			} else
				(*displacedNormals)[i] = faceNormals[triangleIndex[i]];
		}
	}
	// Make a copy of the original mesh and overwrite vertex information
	mesh = srcMesh.Copy(
		std::move(newVertices),
		std::nullopt,
		std::move(displacedNormals),
		std::nullopt,
		std::nullopt,
		std::nullopt
	);
	if (params.normalSmooth && smoothCorner.empty())
		mesh->ComputeNormals();

	// For some debugging
	//mesh->Save("debug.ply");

	const double endTime = WallClockTime();
	SDL_LOG("Displacement time: " << (boost::format("%.3f") % (endTime - startTime)) << "secs");
}

DisplacementShape::~DisplacementShape() {
}

ExtTriangleMeshUPtr DisplacementShape::RefineImpl(SceneConstRef scene) {
	return std::move(mesh);
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
