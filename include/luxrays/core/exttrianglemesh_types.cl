#line 2 "exttrianglemesh_types.cl"

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

#define EXTMESH_MAX_DATA_COUNT 8u

typedef struct {
	Transform appliedTrans;
	int appliedTransSwapsHandedness;
} TriangleMeshParam;

typedef struct {
	Transform trans;
	int transSwapsHandedness;
} TriangleInstanceMeshParam;

typedef struct {
	MotionSystem motionSystem;
} TriangleMotionMeshParam;

typedef struct {
	MeshType type;

	// Vertex information
	unsigned int vertsOffset;
	unsigned int normalsOffset;
	unsigned int triNormalsOffset;
	unsigned int uvsOffset[EXTMESH_MAX_DATA_COUNT];
	unsigned int colsOffset[EXTMESH_MAX_DATA_COUNT];
	unsigned int alphasOffset[EXTMESH_MAX_DATA_COUNT];

	 // Vertex and Triangle AOV
	unsigned int vertexAOVOffset[EXTMESH_MAX_DATA_COUNT];
	unsigned int triAOVOffset[EXTMESH_MAX_DATA_COUNT];

	// Triangle information
	unsigned int trisOffset;

	// Native curve primitives (Metal HWRT; dev-tools/metal_curve_design.md):
	// offset of this mesh's segments inside the global curveSegIndices
	// buffer (NULL_INDEX when the mesh carries no curve data) and the
	// segment count. curveSegIndices[] entries are global control-point
	// indices into the curveCps/curveCpAttrs buffers.
	unsigned int curveSegsOffset;
	unsigned int curveSegsCount;

	// Object space transformation
	union {
		TriangleMeshParam triangle;
		TriangleInstanceMeshParam instance;
		TriangleMotionMeshParam motion;
	};
} ExtMesh;

#if defined(SLG_OPENCL_KERNEL)

// High bit of RayHit.triangleIndex: marks a native curve-primitive hit
// (Metal HWRT path only). The low 31 bits are the mesh-local segment index
// and RayHit.b1 carries the curve parameter u. Software accelerators never
// set the flag.
#define RAYHIT_CURVE_FLAG 0x80000000u

#define EXTMESH_PARAM_DECL , \
		__global const ExtMesh* restrict meshDescs, \
		__global const Point* restrict vertices, \
		__global const Normal* restrict vertNormals, \
		__global const Normal* restrict triNormals, \
		__global const UV* restrict vertUVs, \
		__global const Spectrum* restrict vertCols, \
		__global const float* restrict vertAlphas, \
		__global const float* restrict vertexAOVs, \
		__global const float* restrict triAOVs, \
		__global const Triangle* restrict triangles, \
		__global const InterpolatedTransform* restrict interpolatedTransforms, \
		__global const float4* restrict curveCps, \
		__global const uint* restrict curveSegIndices, \
		__global const float4* restrict curveCpAttrs
#define EXTMESH_PARAM , \
		meshDescs, \
		vertices, \
		vertNormals, \
		triNormals, \
		vertUVs, \
		vertCols, \
		vertAlphas, \
		vertexAOVs, \
		triAOVs, \
		triangles, \
		interpolatedTransforms, \
		curveCps, \
		curveSegIndices, \
		curveCpAttrs

#endif
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
