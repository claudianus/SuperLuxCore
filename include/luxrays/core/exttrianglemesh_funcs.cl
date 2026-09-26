#line 2 "exttrianglemesh_funcs.cl"

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

//------------------------------------------------------------------------------
// Native curve primitives (Metal HWRT; dev-tools/metal_curve_design.md)
//
// A hit with (triangleIndex & RAYHIT_CURVE_FLAG) is a curve-primitive hit:
// low 31 bits = mesh-local segment index, b1 = curve parameter u. The segment
// indexes curveSegIndices[meshDesc->curveSegsOffset + seg] -> global start
// control point; 4 consecutive control points form a uniform Catmull-Rom
// segment (same spline as strands.cpp's CatmullRomSpline and Metal's
// MTLCurveBasisCatmullRom). Per-cp attributes live in curveCpAttrs (2 float4
// per cp): [2i] = {col.rgb, alpha}, [2i+1] = {uv.u, uv.v, strandU,
// strandIndexBits}.
//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 Curve_EvalPoint(const float3 p0, const float3 p1,
		const float3 p2, const float3 p3, const float u) {
	const float u2 = u * u;
	const float u3 = u2 * u;
	return 0.5f * ((2.f * p1) + (-p0 + p2) * u +
			(2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * u2 +
			(-p0 + 3.f * p1 - 3.f * p2 + p3) * u3);
}

OPENCL_FORCE_INLINE float3 Curve_EvalTangent(const float3 p0, const float3 p1,
		const float3 p2, const float3 p3, const float u) {
	const float u2 = u * u;
	return 0.5f * ((-p0 + p2) +
			2.f * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * u +
			3.f * (-p0 + 3.f * p1 - 3.f * p2 + p3) * u2);
}

// flagged triangleIndex -> global index of the first control point of the
// segment (the 4 consecutive cps forming it)
OPENCL_FORCE_INLINE uint Curve_GetCpStart(const uint meshIndex,
		const uint triangleIndex EXTMESH_PARAM_DECL) {
	const uint segIndex = triangleIndex & ~RAYHIT_CURVE_FLAG;
	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	return curveSegIndices[meshDesc->curveSegsOffset + segIndex];
}

// Round-tube shading normal: hit point minus curve centerline point,
// evaluated in object space and transformed like the mesh normals.
OPENCL_FORCE_INLINE float3 Curve_GetNormal(
		__global const Transform* restrict localToWorld,
		const uint meshIndex, const uint triangleIndex,
		const float3 hitP, const float u
		EXTMESH_PARAM_DECL) {
	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	const uint cpStart = Curve_GetCpStart(meshIndex, triangleIndex EXTMESH_PARAM);
	// OpenCL forbids &float4.x: load the whole float4 and swizzle .xyz.
	const float3 centerObj = Curve_EvalPoint(
			curveCps[cpStart].xyz, curveCps[cpStart + 1].xyz,
			curveCps[cpStart + 2].xyz, curveCps[cpStart + 3].xyz, u);

	float3 n;
	switch (meshDesc->type) {
		case TYPE_EXT_TRIANGLE:
			// Control points are in world coordinates for TYPE_EXT_TRIANGLE
			n = (meshDesc->triangle.appliedTransSwapsHandedness ? -1.f : 1.f) * (hitP - centerObj);
			break;
		case TYPE_EXT_TRIANGLE_INSTANCE: {
			const float3 nObj = Transform_InvApplyPoint(localToWorld, hitP) - centerObj;
			n = (meshDesc->instance.transSwapsHandedness ? -1.f : 1.f) *
					Transform_ApplyNormal(localToWorld, nObj);
			break;
		}
		case TYPE_EXT_TRIANGLE_MOTION: {
			const float3 nObj = Transform_InvApplyPoint(localToWorld, hitP) - centerObj;
			const bool swapsHandedness = Transform_SwapsHandedness(localToWorld);
			n = (swapsHandedness ? -1.f : 1.f) *
					Transform_ApplyNormal(localToWorld, nObj);
			break;
		}
		default:
			n = MAKE_FLOAT3(0.f, 0.f, 1.f);
			break;
	}

	const float l2 = dot(n, n);
	if (l2 > 0.f)
		return n / sqrt(l2);

	// Degenerate (hit on the centerline): fall back to a direction
	// perpendicular to the segment tangent.
	const float3 t = Curve_EvalTangent(
			curveCps[cpStart].xyz, curveCps[cpStart + 1].xyz,
			curveCps[cpStart + 2].xyz, curveCps[cpStart + 3].xyz, u);
	float3 v1, v2;
	CoordinateSystem(t, &v1, &v2);
	return v1;
}

// Strand tangent in object space (analytic Catmull-Rom derivative) — same
// space as the HAIR_TANGENT_* vertex AOV layers on the tessellation.
OPENCL_FORCE_INLINE float3 Curve_GetTangentObj(const uint meshIndex,
		const uint triangleIndex, const float u EXTMESH_PARAM_DECL) {
	const uint cpStart = Curve_GetCpStart(meshIndex, triangleIndex EXTMESH_PARAM);
	return Curve_EvalTangent(
			curveCps[cpStart].xyz, curveCps[cpStart + 1].xyz,
			curveCps[cpStart + 2].xyz, curveCps[cpStart + 3].xyz, u);
}

// Per-strand random: recomputed from the strand index with the same hash
// used by StrendsShape (strands.cpp).
OPENCL_FORCE_INLINE float Curve_StrandRandom(const uint strandIndex) {
	uint h = strandIndex * 2654435761u;
	h ^= h >> 16;
	h *= 2246822519u;
	h ^= h >> 13;
	return (h & 0x00FFFFFFu) / (float)0x01000000u;
}

OPENCL_FORCE_INLINE float2 Curve_GetInterpolateUV(const uint meshIndex,
		const uint triangleIndex, const float u, const uint dataIndex
		EXTMESH_PARAM_DECL) {
	// Curve primitives carry a single UV layer (dataIndex 0)
	if (dataIndex != 0)
		return MAKE_FLOAT2(0.f, 0.f);
	const uint cpStart = Curve_GetCpStart(meshIndex, triangleIndex EXTMESH_PARAM);
	const float2 uv1 = curveCpAttrs[2 * (cpStart + 1) + 1].xy;
	const float2 uv2 = curveCpAttrs[2 * (cpStart + 2) + 1].xy;
	return mix(uv1, uv2, u);
}

OPENCL_FORCE_INLINE float3 Curve_GetInterpolateColor(const uint meshIndex,
		const uint triangleIndex, const float u EXTMESH_PARAM_DECL) {
	const uint cpStart = Curve_GetCpStart(meshIndex, triangleIndex EXTMESH_PARAM);
	const float3 c1 = curveCpAttrs[2 * (cpStart + 1)].xyz;
	const float3 c2 = curveCpAttrs[2 * (cpStart + 2)].xyz;
	return mix(c1, c2, u);
}

OPENCL_FORCE_INLINE float Curve_GetInterpolateAlpha(const uint meshIndex,
		const uint triangleIndex, const float u EXTMESH_PARAM_DECL) {
	const uint cpStart = Curve_GetCpStart(meshIndex, triangleIndex EXTMESH_PARAM);
	const float a1 = curveCpAttrs[2 * (cpStart + 1)].w;
	const float a2 = curveCpAttrs[2 * (cpStart + 2)].w;
	return mix(a1, a2, u);
}

// Hair AOV channels (indices must match HAIR_*_DATA_INDEX in strands.h and
// materialdefs_funcs_hair.cl): 4/5/6 = object-space tangent, 7 = strandU,
// 0 = per-strand random.
OPENCL_FORCE_INLINE float Curve_GetVertexAOV(const uint meshIndex,
		const uint triangleIndex, const float u, const uint dataIndex
		EXTMESH_PARAM_DECL) {
	const uint cpStart = Curve_GetCpStart(meshIndex, triangleIndex EXTMESH_PARAM);
	switch (dataIndex) {
		case 4: case 5: case 6: {
			const float3 t = Curve_EvalTangent(
					curveCps[cpStart].xyz, curveCps[cpStart + 1].xyz,
					curveCps[cpStart + 2].xyz, curveCps[cpStart + 3].xyz, u);
			return (dataIndex == 4) ? t.x : ((dataIndex == 5) ? t.y : t.z);
		}
		case 7: {
			const float u1 = curveCpAttrs[2 * (cpStart + 1) + 1].z;
			const float u2 = curveCpAttrs[2 * (cpStart + 2) + 1].z;
			return mix(u1, u2, u);
		}
		case 0: {
			const uint strandIndex = as_uint(curveCpAttrs[2 * (cpStart + 1) + 1].w);
			return Curve_StrandRandom(strandIndex);
		}
		default:
			return 0.f;
	}
}

//------------------------------------------------------------------------------

OPENCL_FORCE_INLINE float3 ExtMesh_GetGeometryNormal(
		__global const Transform* restrict localToWorld,
		const uint meshIndex, const uint triangleIndex
		EXTMESH_PARAM_DECL) {
	// Curve hits are handled in HitPoint_Init (the tube normal needs the
	// hit point, which this signature does not carry); never index with a
	// flagged index.
	if (triangleIndex & RAYHIT_CURVE_FLAG)
		return MAKE_FLOAT3(0.f, 0.f, 1.f);

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	__global const Normal* restrict tn = &triNormals[meshDesc->triNormalsOffset];

	float3 geometryN = VLOAD3F(&tn[triangleIndex].x);

	switch (meshDesc->type) {
		case TYPE_EXT_TRIANGLE: {
			// geometryN is already in world coordinates for TYPE_EXT_TRIANGLE
			// and
			// pre-computed geometry normals already factor appliedTransSwapsHandedness
			break;
		}
		case TYPE_EXT_TRIANGLE_INSTANCE: {
			// Transform to global coordinates
			geometryN = (meshDesc->instance.transSwapsHandedness ? -1.f : 1.f) * normalize(Transform_ApplyNormal(localToWorld, geometryN));
			break;
		}
		case TYPE_EXT_TRIANGLE_MOTION: {
			const bool swapsHandedness = Transform_SwapsHandedness(localToWorld); 
			// Transform to global coordinates
			geometryN = (swapsHandedness ? -1.f : 1.f) * normalize(Transform_ApplyNormal(localToWorld, geometryN));
			break;
		}
	}

	return geometryN;
}

OPENCL_FORCE_INLINE float3 ExtMesh_GetInterpolateNormal(
		__global const Transform* restrict localToWorld,
		const uint meshIndex, const uint triangleIndex,
		const float b1, const float b2
		EXTMESH_PARAM_DECL) {
	// Curve hits are handled in HitPoint_Init; never index with a flagged index.
	if (triangleIndex & RAYHIT_CURVE_FLAG)
		return MAKE_FLOAT3(0.f, 0.f, 1.f);

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];

	float3 interpolatedN;
	if (meshDesc->normalsOffset != NULL_INDEX) {
		// Shading normal expressed in local coordinates
		__global const Triangle* restrict tri = &triangles[meshDesc->trisOffset + triangleIndex];
		__global const Normal* restrict vns = &vertNormals[meshDesc->normalsOffset];
		const float3 n0 = VLOAD3F(&vns[tri->v[0]].x);
		const float3 n1 = VLOAD3F(&vns[tri->v[1]].x);
		const float3 n2 = VLOAD3F(&vns[tri->v[2]].x);

		const float b0 = 1.f - b1 - b2;
		interpolatedN =  Triangle_InterpolateNormal(n0, n1, n2, b0, b1, b2);

		switch (meshDesc->type) {
			case TYPE_EXT_TRIANGLE: {
				// interpolatedN is already in world coordinates for TYPE_EXT_TRIANGLE
				interpolatedN = (meshDesc->triangle.appliedTransSwapsHandedness ? -1.f : 1.f) * interpolatedN;
				break;
			}
			case TYPE_EXT_TRIANGLE_INSTANCE: {
				// Transform to global coordinates
				interpolatedN = (meshDesc->instance.transSwapsHandedness ? -1.f : 1.f) * normalize(Transform_ApplyNormal(localToWorld, interpolatedN));
				break;
			}
			case TYPE_EXT_TRIANGLE_MOTION: {
				const bool swapsHandedness = Transform_SwapsHandedness(localToWorld); 
				// Transform to global coordinates
				interpolatedN = (swapsHandedness ? -1.f : 1.f) * normalize(Transform_ApplyNormal(localToWorld, interpolatedN));
				break;
			}
		}
	} else
		interpolatedN = ExtMesh_GetGeometryNormal(localToWorld, meshIndex, triangleIndex EXTMESH_PARAM);

	return interpolatedN;
}

OPENCL_FORCE_INLINE float2 ExtMesh_GetInterpolateUV(
		const uint meshIndex, const uint triangleIndex,
		const float b1, const float b2, const uint dataIndex
		EXTMESH_PARAM_DECL) {
	if (triangleIndex & RAYHIT_CURVE_FLAG)
		return Curve_GetInterpolateUV(meshIndex, triangleIndex, b1, dataIndex EXTMESH_PARAM);

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	
	float2 uv = MAKE_FLOAT2(0.f, 0.f);
	if (meshDesc->uvsOffset[dataIndex] != NULL_INDEX) {
		__global const UV* restrict uvs = &vertUVs[meshDesc->uvsOffset[dataIndex]];
		__global const Triangle* restrict tri = &triangles[meshDesc->trisOffset + triangleIndex];

		const float2 uv0 = VLOAD2F(&uvs[tri->v[0]].u);
		const float2 uv1 = VLOAD2F(&uvs[tri->v[1]].u);
		const float2 uv2 = VLOAD2F(&uvs[tri->v[2]].u);

		const float b0 = 1.f - b1 - b2;
		uv = Triangle_InterpolateUV(uv0, uv1, uv2, b0, b1, b2);
	}

	return uv;
}

OPENCL_FORCE_INLINE float3 ExtMesh_GetInterpolateColor(
		const uint meshIndex, const uint triangleIndex,
		const float b1, const float b2, const uint dataIndex
		EXTMESH_PARAM_DECL) {
	if (triangleIndex & RAYHIT_CURVE_FLAG)
		return Curve_GetInterpolateColor(meshIndex, triangleIndex, b1 EXTMESH_PARAM);

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];

	float3 c = WHITE;
	if (meshDesc->colsOffset[dataIndex] != NULL_INDEX) {
		__global const Spectrum* restrict vcs = &vertCols[meshDesc->colsOffset[dataIndex]];
		__global const Triangle* restrict tri = &triangles[meshDesc->trisOffset + triangleIndex];
		const float3 rgb0 = VLOAD3F(vcs[tri->v[0]].c);
		const float3 rgb1 = VLOAD3F(vcs[tri->v[1]].c);
		const float3 rgb2 = VLOAD3F(vcs[tri->v[2]].c);

		const float b0 = 1.f - b1 - b2;
		c = Triangle_InterpolateColor(rgb0, rgb1, rgb2, b0, b1, b2);
	}
	
	return c;
}

OPENCL_FORCE_INLINE float ExtMesh_GetInterpolateAlpha(
		const uint meshIndex, const uint triangleIndex,
		const float b1, const float b2, const uint dataIndex
		EXTMESH_PARAM_DECL) {
	if (triangleIndex & RAYHIT_CURVE_FLAG)
		return Curve_GetInterpolateAlpha(meshIndex, triangleIndex, b1 EXTMESH_PARAM);

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	
	float a = 1.f;
	if (meshDesc->alphasOffset[dataIndex] != NULL_INDEX) {
		__global const float* restrict vas = &vertAlphas[meshDesc->alphasOffset[dataIndex]];
		__global const Triangle* restrict tri = &triangles[meshDesc->trisOffset + triangleIndex];
		const float a0 = vas[tri->v[0]];
		const float a1 = vas[tri->v[1]];
		const float a2 = vas[tri->v[2]];

		const float b0 = 1.f - b1 - b2;
		a =  Triangle_InterpolateAlpha(a0, a1, a2, b0, b1, b2);
	}

	return a;
}

OPENCL_FORCE_INLINE float ExtMesh_GetInterpolateVertexAOV(
		const uint meshIndex, const uint triangleIndex,
		const float b1, const float b2, const uint dataIndex
		EXTMESH_PARAM_DECL) {
	if (triangleIndex & RAYHIT_CURVE_FLAG)
		return Curve_GetVertexAOV(meshIndex, triangleIndex, b1, dataIndex EXTMESH_PARAM);

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	
	float v = 0.f;
	if (meshDesc->vertexAOVOffset[dataIndex] != NULL_INDEX) {
		__global const float* restrict vs = &vertexAOVs[meshDesc->vertexAOVOffset[dataIndex]];
		__global const Triangle* restrict tri = &triangles[meshDesc->trisOffset + triangleIndex];
		const float v0 = vs[tri->v[0]];
		const float v1 = vs[tri->v[1]];
		const float v2 = vs[tri->v[2]];

		const float b0 = 1.f - b1 - b2;
		v =  Triangle_InterpolateVertexAOV(v0, v1, v2, b0, b1, b2);
	}

	return v;
}

OPENCL_FORCE_INLINE float ExtMesh_GetTriAOV(
		const uint meshIndex, const uint triangleIndex,
		const uint dataIndex
		EXTMESH_PARAM_DECL) {
	// Curve primitives have no triangle AOVs
	if (triangleIndex & RAYHIT_CURVE_FLAG)
		return 0.f;

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	
	float t = 0.f;
	if (meshDesc->triAOVOffset[dataIndex] != NULL_INDEX) {
		__global const float* restrict ts = &triAOVs[meshDesc->triAOVOffset[dataIndex]];
		
		t = ts[triangleIndex];
	}

	return t;
}

OPENCL_FORCE_INLINE void ExtMesh_GetDifferentials(
		__global const Transform* restrict localToWorld,
		const uint meshIndex,
		const uint triangleIndex,
		float3 shadeNormal,
		const uint dataIndex,
		float3 *dpdu, float3 *dpdv,
        float3 *dndu, float3 *dndv
		EXTMESH_PARAM_DECL) {
	// Curve hits get their differentials in HitPoint_Init; never index with
	// a flagged index.
	if (triangleIndex & RAYHIT_CURVE_FLAG) {
		*dpdu = MAKE_FLOAT3(0.f, 0.f, 0.f);
		*dpdv = MAKE_FLOAT3(0.f, 0.f, 0.f);
		*dndu = MAKE_FLOAT3(0.f, 0.f, 0.f);
		*dndv = MAKE_FLOAT3(0.f, 0.f, 0.f);
		return;
	}

	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	__global const Point* restrict iVertices = &vertices[meshDesc->vertsOffset];
	__global const Triangle* restrict iTriangles = &triangles[meshDesc->trisOffset];

	// Compute triangle partial derivatives
	__global const Triangle* restrict tri = &iTriangles[triangleIndex];
	const uint vi0 = tri->v[0];
	const uint vi1 = tri->v[1];
	const uint vi2 = tri->v[2];

	float2 uv0, uv1, uv2;
	if (meshDesc->uvsOffset[dataIndex] != NULL_INDEX) {
		// Ok, UV coordinates are available, use them to build the reference
		// system around the shading normal.

		__global const UV* restrict iVertUVs = &vertUVs[meshDesc->uvsOffset[dataIndex]];
		uv0 = VLOAD2F(&iVertUVs[vi0].u);
		uv1 = VLOAD2F(&iVertUVs[vi1].u);
		uv2 = VLOAD2F(&iVertUVs[vi2].u);
	} else {
		uv0 = MAKE_FLOAT2(.5f, .5f);
		uv1 = MAKE_FLOAT2(.5f, .5f);
		uv2 = MAKE_FLOAT2(.5f, .5f);
	}

	// Compute deltas for triangle partial derivatives
	const float du1 = uv0.x - uv2.x;
	const float du2 = uv1.x - uv2.x;
	const float dv1 = uv0.y - uv2.y;
	const float dv2 = uv1.y - uv2.y;
	const float determinant = du1 * dv2 - dv1 * du2;

	if (determinant == 0.f) {
		// Handle 0 determinant for triangle partial derivative matrix
		CoordinateSystem(shadeNormal, dpdu, dpdv);
		*dndu = ZERO;
		*dndv = ZERO;
	} else {
		const float invdet = 1.f / determinant;

		// Vertices expressed in local coordinates
		const float3 p0 = VLOAD3F(&iVertices[vi0].x);
		const float3 p1 = VLOAD3F(&iVertices[vi1].x);
		const float3 p2 = VLOAD3F(&iVertices[vi2].x);
		
		float3 dp1 = p0 - p2;
		float3 dp2 = p1 - p2;

		// dp1 and dp2 are already in world coordinates for TYPE_EXT_TRIANGLE
		if (meshDesc->type != TYPE_EXT_TRIANGLE) {
			// Transform to global coordinates
			dp1 = Transform_ApplyVector(localToWorld, dp1);
			dp2 = Transform_ApplyVector(localToWorld, dp2);
		}

		//------------------------------------------------------------------
		// Compute dpdu and dpdv
		//------------------------------------------------------------------

		const float3 geometryDpDu = ( dv2 * dp1 - dv1 * dp2) * invdet;
		const float3 geometryDpDv = (-du2 * dp1 + du1 * dp2) * invdet;

		*dpdu = cross(shadeNormal, cross(geometryDpDu, shadeNormal));
		*dpdv = cross(shadeNormal, cross(geometryDpDv, shadeNormal));

		//------------------------------------------------------------------
		// Compute dndu and dndv
		//------------------------------------------------------------------

		if (meshDesc->normalsOffset != NULL_INDEX) {
			__global const Normal* restrict iVertNormals = &vertNormals[meshDesc->normalsOffset];
			// Shading normals expressed in local coordinates
			const float3 n0 = VLOAD3F(&iVertNormals[tri->v[0]].x);
			const float3 n1 = VLOAD3F(&iVertNormals[tri->v[1]].x);
			const float3 n2 = VLOAD3F(&iVertNormals[tri->v[2]].x);
			const float3 dn1 = n0 - n2;
			const float3 dn2 = n1 - n2;

			*dndu = ( dv2 * dn1 - dv1 * dn2) * invdet;
			*dndv = (-du2 * dn1 + du1 * dn2) * invdet;
			
			// dndu and dndv are already in world coordinates for TYPE_EXT_TRIANGLE
			if (meshDesc->type != TYPE_EXT_TRIANGLE) {
				// Transform to global coordinates
				*dndu = Transform_ApplyNormal(localToWorld, *dndu);
				*dndv = Transform_ApplyNormal(localToWorld, *dndv);
			}
		} else {
			*dndu = ZERO;
			*dndv = ZERO;
		}
	}
}

OPENCL_FORCE_INLINE void ExtMesh_Sample(
		__global const Transform* restrict localToWorld,
		const uint meshIndex, const uint triangleIndex,
		const float u0, const float u1,
		float3 *samplePoint, float *b0, float *b1, float *b2
		EXTMESH_PARAM_DECL) {
	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];
	__global const Point* restrict triVerts = &vertices[meshDesc->vertsOffset];
	__global const Triangle* restrict tri = &triangles[meshDesc->trisOffset + triangleIndex];

	// Vertices are in local object space
	float3 p0 = VLOAD3F(&triVerts[tri->v[0]].x);
	float3 p1 = VLOAD3F(&triVerts[tri->v[1]].x);
	float3 p2 = VLOAD3F(&triVerts[tri->v[2]].x);
	*samplePoint = Triangle_Sample(
			p0, p1, p2,
			u0, u1,
			b0, b1, b2);

	// samplePoint is already in world coordinates for TYPE_EXT_TRIANGLE
	if (meshDesc->type != TYPE_EXT_TRIANGLE) {
		// Transform to global coordinates
		*samplePoint = Transform_ApplyPoint(localToWorld, *samplePoint);
	}
}

OPENCL_FORCE_INLINE void ExtMesh_GetLocal2World(const float time,
		const uint meshIndex, const uint triangleIndex,
		__global Transform *local2World
		EXTMESH_PARAM_DECL) {
	// Initialized world to local object space transformation
	__global const ExtMesh* restrict meshDesc = &meshDescs[meshIndex];

	switch (meshDesc->type) {
		case TYPE_EXT_TRIANGLE:
			*local2World = meshDesc->triangle.appliedTrans;
			break;
		case TYPE_EXT_TRIANGLE_INSTANCE:
			*local2World = meshDesc->instance.trans;
			break;
		case TYPE_EXT_TRIANGLE_MOTION: {
			Matrix4x4 m;

			MotionSystem_Sample(&meshDesc->motion.motionSystem, time, interpolatedTransforms, &m);
			local2World->mInv = m;

			MotionSystem_SampleInverse(&meshDesc->motion.motionSystem, time, interpolatedTransforms, &m);
			local2World->m = m;

			break;
		}
		default:
			break;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
