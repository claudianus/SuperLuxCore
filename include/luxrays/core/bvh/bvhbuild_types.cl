#line 2 "bvhbuild_types.cl"

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

typedef struct {
	union {
		struct {
			// I can not use BBox here because objects with a constructor are not
			// allowed inside an union.
			float bboxMin[3];
			float bboxMax[3];
		} bvhNode;
		struct {
			unsigned int v[3];
			unsigned int meshIndex, triangleIndex;
		} triangleLeaf;
		struct {
			unsigned int leafIndex;
			unsigned int transformIndex, motionIndex; // transformIndex or motionIndex have to be NULL_INDEX (i.e. only one can be used)
			unsigned int meshOffsetIndex;
		} bvhLeaf; // Used by MBVH
	};
	// Most significant bit is used to mark leafs
	unsigned int nodeData;
	int pad0; // To align to float4
} BVHArrayNode;

typedef struct {
	union {
		// I can not use BBox/Point/Normal here because objects with a constructor are not
		// allowed inside an union.
		struct {
			float bboxMin[3];
			float bboxMax[3];
		} bvhNode;
		struct {
			unsigned int entryIndex;
		} entryLeaf;
	};
	// Most significant bit is used to mark leafs
	unsigned int nodeData;
	int pad; // To align to float4
} IndexBVHArrayNode;

#define IndexBVHNodeData_IsLeaf(nodeData) ((nodeData) & 0x80000000u)
#define IndexBVHNodeData_GetSkipIndex(nodeData) ((nodeData) & 0x7fffffffu)

// Per-vertex deformation motion blur (dev-tools/deformation-motion-blur-design.md):
// one entry per MBVH leaf reference (indexed by meshOffsetIndex). Leaves without
// vertex motion have vertCount == 0 and take the static vertex fetch path.
typedef struct {
	unsigned int vertCount;        // 0 => leaf carries no vertex motion
	unsigned int stepCount;        // number of motion steps (>= 2)
	unsigned int vertsOffset;      // first vertex of step 0 in the motion vertex buffer
	unsigned int timesOffset;      // first step time in the motion times buffer
	unsigned int staticVertOffset; // first vertex of the leaf mesh in the global (page-decoded) vertex index space
	unsigned int pad0, pad1, pad2; // pad to a multiple of float4
} VertMotionDesc;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
