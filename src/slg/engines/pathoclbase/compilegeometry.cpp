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

#include "luxrays/utils/buffer.h"
#include <functional>
#if !defined(LUXRAYS_DISABLE_OPENCL)

#include <iosfwd>
#include <limits>
#include <set>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include "slg/engines/pathoclbase/compiledscene.h"
#include "luxrays/core/geometry/bbox.h"
#include "slg/kernels/kernels.h"
#include "slg/scene/sceneobjectdefs.h"

using namespace std;
using namespace luxrays;
using namespace slg;

static bool MeshPtrCompare(const Mesh * p0, const Mesh * p1) {
	return p0 < p1;
}

void CompiledScene::CompileGeometry() {
	SLG_LOG("Compile Geometry");
	wasGeometryCompiled = true;

	const u_int objCount = scene.GetObjects().GetSize();

	const double tStart = WallClockTime();

	// Clear arrays (Reset() also drops any file mapping from a previous
	// spilled staging state)
	verts.Reset();
	normals.Reset();
	triNormals.Reset();
	uvs.Reset();
	cols.Reset();
	alphas.Reset();
	vertexAOVs.Reset();
	triAOVs.Reset();
	tris.Reset();
	interpolatedTransforms.Reset();
	meshDescs.Reset();
	curveCps.Reset();
	curveSegIndices.Reset();
	curveCpAttrs.Reset();

	// Reserve the exact unique payload before appending it. Geometric vector
	// growth otherwise temporarily keeps the old and new multi-GB allocations
	// alive while the original scene and acceleration structure are resident.
	// Count by base-mesh identity, matching the sharing rules below; two meshes
	// with equal sizes or values are deliberately not treated as the same mesh.
	std::set<const ExtTriangleMesh *> uniqueBaseMeshes;
	size_t vertexCount = 0, triangleCount = 0, normalCount = 0;
	size_t uvCount = 0, colorCount = 0, alphaCount = 0;
	size_t vertexAOVCount = 0, triangleAOVCount = 0;
	size_t curveCpCount = 0, curveAttrCount = 0, curveIndexCount = 0;
	size_t interpolatedTransformCount = 0;
	for (u_int i = 0; i < objCount; ++i) {
		const ExtMesh &mesh = scene.GetObjects().GetSceneObject(i).GetExtMesh();
		const ExtTriangleMesh *base;
		switch (mesh.GetType()) {
			case TYPE_EXT_TRIANGLE_INSTANCE:
				base = &static_cast<const ExtInstanceTriangleMesh &>(mesh).GetExtTriangleMesh();
				break;
			case TYPE_EXT_TRIANGLE_MOTION: {
				const auto &motion = static_cast<const ExtMotionTriangleMesh &>(mesh);
				base = &motion.GetExtTriangleMesh();
				const auto &system = motion.GetMotionSystem();
				interpolatedTransformCount += system.interpolatedTransforms.size() +
						system.interpolatedInverseTransforms.size();
				break;
			}
			case TYPE_EXT_TRIANGLE:
				base = &static_cast<const ExtTriangleMesh &>(mesh);
				break;
			default:
				throw runtime_error("Unsupported mesh type in CompiledScene::CompileGeometry(): " + ToString(mesh.GetType()));
		}
		if (!uniqueBaseMeshes.insert(base).second)
			continue;
		const size_t vertices = base->GetTotalVertexCount();
		const size_t triangles = base->GetTotalTriangleCount();
		vertexCount += vertices;
		triangleCount += triangles;
		if (base->HasNormals()) normalCount += vertices;
		for (u_int layer = 0; layer < EXTMESH_MAX_DATA_COUNT; ++layer) {
			if (base->HasUVs(layer)) uvCount += vertices;
			if (base->HasColors(layer)) colorCount += vertices;
			if (base->HasAlphas(layer)) alphaCount += vertices;
			if (base->HasVertexAOV(layer)) vertexAOVCount += vertices;
			if (base->HasTriAOV(layer)) triangleAOVCount += triangles;
		}
		if (base->HasCurveData()) {
			curveCpCount += base->GetCurveCps().size();
			curveAttrCount += base->GetCurveCpAttrs().size();
			curveIndexCount += base->GetCurveSegIndices().size();
		}
	}
	verts.reserve(vertexCount);
	normals.reserve(normalCount);
	triNormals.reserve(triangleCount);
	uvs.reserve(uvCount);
	cols.reserve(colorCount);
	alphas.reserve(alphaCount);
	vertexAOVs.reserve(vertexAOVCount);
	triAOVs.reserve(triangleAOVCount);
	tris.reserve(triangleCount);
	curveCps.reserve(curveCpCount);
	curveCpAttrs.reserve(curveAttrCount);
	curveSegIndices.reserve(curveIndexCount);
	meshDescs.reserve(objCount);
	interpolatedTransforms.reserve(interpolatedTransformCount);
	SLG_LOG("Geometry staging unique meshes: " << uniqueBaseMeshes.size() <<
			", vertices: " << vertexCount << ", triangles: " << triangleCount);
	SLG_LOG("Geometry staging payload bytes: " <<
			vertexCount * sizeof(verts[0]) + normalCount * sizeof(normals[0]) +
			triangleCount * (sizeof(triNormals[0]) + sizeof(tris[0])) +
			uvCount * sizeof(uvs[0]) + colorCount * sizeof(cols[0]) +
			alphaCount * sizeof(alphas[0]) + vertexAOVCount * sizeof(vertexAOVs[0]) +
			triangleAOVCount * sizeof(triAOVs[0]) + curveCpCount * sizeof(curveCps[0]) +
			curveAttrCount * sizeof(curveCpAttrs[0]) + curveIndexCount * sizeof(curveSegIndices[0]) +
			objCount * sizeof(meshDescs[0]) + interpolatedTransformCount * sizeof(interpolatedTransforms[0]));

	//--------------------------------------------------------------------------
	// Translate geometry
	//--------------------------------------------------------------------------

	// Not using std::unordered_map because the key is an ExtMesh pointer
	std::map<const ExtMesh *, u_int, bool (*)(const Mesh * , const Mesh * )> definedMeshs(MeshPtrCompare);

	u_int vertsOffset = 0;
	u_int trisOffset = 0;
	u_int normalsOffset = 0;
	u_int triNormalsOffset = 0;
	u_int uvsOffset = 0;
	u_int colsOffset = 0;
	u_int alphasOffset = 0;
	u_int vertexAOVOffset = 0;
	u_int triAOVOffset = 0;
	u_int curveSegsOffset = 0;

	auto InitMeshDesc = [&](slg::ocl::ExtMesh &dstMeshDesc, const ExtMesh &srcMesh,
			const ExtTriangleMesh &srcBaseMesh) { 
        dstMeshDesc.vertsOffset = vertsOffset;
		// Prepared before worker startup; unused Generated coordinates must
		// not trigger a vertex scan of an unrelated proxy mesh.
		if (srcBaseMesh.HasGeneratedTransform())
			memcpy(dstMeshDesc.generatedTransform,
					srcBaseMesh.GetGeneratedTransform().data(), sizeof(float[12]));
		else
			memset(dstMeshDesc.generatedTransform, 0, sizeof(float[12]));
		vertsOffset += srcMesh.GetTotalVertexCount();

		dstMeshDesc.trisOffset = trisOffset;
		trisOffset += srcMesh.GetTotalTriangleCount();

		if (srcMesh.HasNormals()) {
			dstMeshDesc.normalsOffset = normalsOffset;
			normalsOffset += srcMesh.GetTotalVertexCount();
		} else
			dstMeshDesc.normalsOffset = NULL_INDEX;

		dstMeshDesc.triNormalsOffset = triNormalsOffset;
		triNormalsOffset += srcMesh.GetTotalTriangleCount();

		for (u_int dataIndex = 0; dataIndex < EXTMESH_MAX_DATA_COUNT; ++dataIndex) {
			if (srcMesh.HasUVs(dataIndex)) {
				dstMeshDesc.uvsOffset[dataIndex] = uvsOffset;
				uvsOffset += srcMesh.GetTotalVertexCount();
			} else
				dstMeshDesc.uvsOffset[dataIndex] = NULL_INDEX;

			if (srcMesh.HasColors(dataIndex)) {
				dstMeshDesc.colsOffset[dataIndex] = colsOffset;
				colsOffset += srcMesh.GetTotalVertexCount();
			} else
				dstMeshDesc.colsOffset[dataIndex] = NULL_INDEX;

			if (srcMesh.HasAlphas(dataIndex)) {
				dstMeshDesc.alphasOffset[dataIndex] = alphasOffset;
				alphasOffset += srcMesh.GetTotalVertexCount();
			} else
				dstMeshDesc.alphasOffset[dataIndex] = NULL_INDEX;

			if (srcMesh.HasVertexAOV(dataIndex)) {
				dstMeshDesc.vertexAOVOffset[dataIndex] = vertexAOVOffset;
				vertexAOVOffset += srcMesh.GetTotalVertexCount();
			} else
				dstMeshDesc.vertexAOVOffset[dataIndex] = NULL_INDEX;

			if (srcMesh.HasTriAOV(dataIndex)) {
				dstMeshDesc.triAOVOffset[dataIndex] = triAOVOffset;
				triAOVOffset += srcMesh.GetTotalTriangleCount();
			} else
				dstMeshDesc.triAOVOffset[dataIndex] = NULL_INDEX;
		}

		// Native curve primitives (Metal HWRT): the curve data lives on the
		// base triangle mesh; curveSegIndices[] entries are globalized below
		// by adding the mesh's control-point base.
		if (srcBaseMesh.HasCurveData()) {
			dstMeshDesc.curveSegsOffset = curveSegsOffset;
			dstMeshDesc.curveSegsCount = srcBaseMesh.GetCurveSegCount();
			curveSegsOffset += dstMeshDesc.curveSegsCount;
		} else {
			dstMeshDesc.curveSegsOffset = NULL_INDEX;
			dstMeshDesc.curveSegsCount = 0;
		}
    };

	slg::ocl::ExtMesh currentMeshDesc;
	ExtTriangleMesh initBaseMesh;
	std::reference_wrapper<const ExtTriangleMesh> baseMesh = initBaseMesh;
	for (u_int i = 0; i < objCount; ++i) {
		ExtMeshConstRef mesh = scene.GetObjects().GetSceneObject(i).GetExtMesh();

		bool isExistingInstance;
		switch (mesh.GetType()) {
			case TYPE_EXT_TRIANGLE_INSTANCE: {
				// It is an instanced mesh
				auto& imesh = static_cast<const ExtInstanceTriangleMesh &>(mesh);
				baseMesh = imesh.GetExtTriangleMesh();

				// Check if is one of the already defined meshes
				ExtTriangleMeshConstRef trimesh = imesh.GetExtTriangleMesh();
				auto it = definedMeshs.find(&trimesh);
				// auto it = definedMeshs.find(&imesh.GetExtTriangleMesh());
				if (it == definedMeshs.end()) {
					// It is a new one
					InitMeshDesc(currentMeshDesc, imesh, baseMesh.get());

					isExistingInstance = false;

					const u_int index = meshDescs.size();
					definedMeshs[&imesh.GetExtTriangleMesh()] = index;
				} else {
					currentMeshDesc = meshDescs[it->second];

					isExistingInstance = true;
				}

				currentMeshDesc.type = slg::ocl::TYPE_EXT_TRIANGLE_INSTANCE;
				memcpy(&currentMeshDesc.instance.trans.m, &imesh.GetTransformation().m, sizeof(float[4][4]));
				memcpy(&currentMeshDesc.instance.trans.mInv, &imesh.GetTransformation().mInv, sizeof(float[4][4]));
				currentMeshDesc.instance.transSwapsHandedness = imesh.GetTransformation().SwapsHandedness();
				break;
			}
			case TYPE_EXT_TRIANGLE_MOTION: {
				// It is an instanced mesh
				auto& mmesh = static_cast<const ExtMotionTriangleMesh&>(mesh);
				baseMesh = mmesh.GetExtTriangleMesh();

				// Check if is one of the already defined meshes
				auto it = definedMeshs.find(&mmesh.GetExtTriangleMesh());
				if (it == definedMeshs.end()) {
					// It is a new one
					InitMeshDesc(currentMeshDesc, mmesh, baseMesh.get());

					isExistingInstance = false;

					const u_int index = meshDescs.size();
					definedMeshs[&mmesh.GetExtTriangleMesh()] = index;
				} else {
					currentMeshDesc = meshDescs[it->second];

					isExistingInstance = true;
				}

				currentMeshDesc.type = slg::ocl::TYPE_EXT_TRIANGLE_MOTION;
				
				const MotionSystem &ms = mmesh.GetMotionSystem();

				// Copy the motion system information

				// Forward transformations
				currentMeshDesc.motion.motionSystem.interpolatedTransformFirstIndex = interpolatedTransforms.size();
				for (auto const &it : ms.interpolatedTransforms) {
					// Here, I assume that luxrays::ocl::InterpolatedTransform and
					// luxrays::InterpolatedTransform are the same
					interpolatedTransforms.push_back(*((const luxrays::ocl::InterpolatedTransform *)&it));
				}
				currentMeshDesc.motion.motionSystem.interpolatedTransformLastIndex = interpolatedTransforms.size() - 1;

				// Backward transformations
				currentMeshDesc.motion.motionSystem.interpolatedInverseTransformFirstIndex = interpolatedTransforms.size();
				for (auto const &it : ms.interpolatedInverseTransforms) {
					// Here, I assume that luxrays::ocl::InterpolatedTransform and
					// luxrays::InterpolatedTransform are the same
					interpolatedTransforms.push_back(*((const luxrays::ocl::InterpolatedTransform *)&it));
				}
				currentMeshDesc.motion.motionSystem.interpolatedInverseTransformLastIndex = interpolatedTransforms.size() - 1;
				break;
			}
			case TYPE_EXT_TRIANGLE: {
				baseMesh = static_cast<const ExtTriangleMesh &>(mesh);

				// Direct objects may reference the same base mesh as another
				// direct object or an instance. Only the immutable payload is
				// shared; every object still gets its own type/transform descriptor.
				const auto it = definedMeshs.find(&baseMesh.get());
				isExistingInstance = it != definedMeshs.end();
				if (isExistingInstance)
					currentMeshDesc = meshDescs[it->second];
				else {
					InitMeshDesc(currentMeshDesc, baseMesh.get(), baseMesh.get());
					definedMeshs[&baseMesh.get()] = meshDescs.size();
				}

				currentMeshDesc.type = slg::ocl::TYPE_EXT_TRIANGLE;

				Transform t;
				// Time doesn't matter for ExtTriangleMesh
				baseMesh.get().GetLocal2World(0.f, t);
				memcpy(&currentMeshDesc.triangle.appliedTrans.m, &t.m, sizeof(float[4][4]));
				memcpy(&currentMeshDesc.triangle.appliedTrans.mInv, &t.mInv, sizeof(float[4][4]));
				currentMeshDesc.triangle.appliedTransSwapsHandedness = t.SwapsHandedness();

				break;
			}
			default:
				throw runtime_error("Unsupported mesh type in CompiledScene::CompileGeometry(): " + ToString(mesh.GetType()));
		}
		Transform baseTransform;
		baseMesh.get().GetLocal2World(0.f, baseTransform);
		currentMeshDesc.baseAppliedTransSwapsHandedness = baseTransform.SwapsHandedness();

		if (!isExistingInstance) {		
			//------------------------------------------------------------------
			// Compile mesh normals (expressed in local coordinates)
			//------------------------------------------------------------------

			if (baseMesh.get().HasNormals()) {
				const auto& n = baseMesh.get().GetNormals().GetObjects();
				normals.Append(n.begin(), n.begin() + baseMesh.get().GetTotalVertexCount());
			}

			//------------------------------------------------------------------
			// Compile mesh triangle normals (expressed in local coordinates)
			//------------------------------------------------------------------

			const auto& tn = baseMesh.get().GetTriNormals().GetObjects();
			triNormals.Append(
				tn.begin(),
				tn.begin() + baseMesh.get().GetTotalTriangleCount()
			);

			for (u_int dataIndex = 0; dataIndex < EXTMESH_MAX_DATA_COUNT; ++dataIndex) {
				//--------------------------------------------------------------
				// Compile vertex uvs
				//--------------------------------------------------------------

				if (baseMesh.get().HasUVs(dataIndex)) {
					const UV *u = baseMesh.get().GetUVs(dataIndex).get();
					uvs.Append(u, u + baseMesh.get().GetTotalVertexCount());
				}

				//--------------------------------------------------------------
				// Compile vertex colors
				//--------------------------------------------------------------

				if (baseMesh.get().HasColors(dataIndex)) {
					const Spectrum *c = baseMesh.get().GetColors(dataIndex).get();
					cols.Append(c, c + baseMesh.get().GetTotalVertexCount());
				}

				//--------------------------------------------------------------
				// Compile vertex alphas
				//--------------------------------------------------------------

				if (baseMesh.get().HasAlphas(dataIndex)) {
					const float *a = baseMesh.get().GetAlphas(dataIndex).get();
					alphas.Append(a, a + baseMesh.get().GetTotalVertexCount());
				}

				//--------------------------------------------------------------
				// Compile vertex AOVs
				//--------------------------------------------------------------

				if (baseMesh.get().HasVertexAOV(dataIndex)) {
					const float *v = baseMesh.get().GetVertexAOVs(dataIndex).get();
					vertexAOVs.Append(v, v + baseMesh.get().GetTotalVertexCount());
				}

				//--------------------------------------------------------------
				// Compile triangle AOVs
				//--------------------------------------------------------------

				if (baseMesh.get().HasTriAOV(dataIndex)) {
					const float *t = baseMesh.get().GetTriAOVs(dataIndex).get();
					triAOVs.Append(t, t + baseMesh.get().GetTotalTriangleCount());
				}
			}

			//------------------------------------------------------------------
			// Compile baseMesh vertices (expressed in local coordinates)
			//------------------------------------------------------------------

			const Points v = baseMesh.get().GetVertices();
			verts.Append(
				v.begin(),
				v.begin() + baseMesh.get().GetTotalVertexCount()
			);

			//------------------------------------------------------------------
			// Compile baseMesh triangle indices
			//------------------------------------------------------------------

			const auto t = baseMesh.get().GetTriangles();
			tris.Append(t.begin(), t.begin() + baseMesh.get().GetTotalTriangleCount());

			//------------------------------------------------------------------
			// Compile native curve primitives (Metal HWRT). Segment indices
			// are globalized by adding the mesh's control-point base.
			//------------------------------------------------------------------

			if (baseMesh.get().HasCurveData()) {
				const u_int cpBase = (u_int)curveCps.size();

				const auto &cps = baseMesh.get().GetCurveCps();
				curveCps.Append(cps.begin(), cps.end());

				const auto &attrs = baseMesh.get().GetCurveCpAttrs();
				curveCpAttrs.Append(attrs.begin(), attrs.end());

				const auto &segs = baseMesh.get().GetCurveSegIndices();
				for (const u_int s : segs)
					curveSegIndices.push_back(s + cpBase);
			}
		}

		meshDescs.push_back(currentMeshDesc);
	}

	worldBSphere = scene.GetDataSet().GetBSphere();

	const double tEnd = WallClockTime();
	SLG_LOG("Scene geometry compilation time: " << int((tEnd - tStart) * 1000.0) << "ms");
}

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
