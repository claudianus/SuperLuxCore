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

#include "luxrays/core/exttrianglemesh.h"
#include "slg/shapes/strands.h"
#include "luxrays/utils/buffer.h"
#include "slg/scene/scene.h"
#include "slg/cameras/perspective.h"
#include <memory>
#include <cstring>

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// CatmullRomCurve class definition
//------------------------------------------------------------------------------

class CatmullRomCurve {
public:
	CatmullRomCurve() {
	}
	~CatmullRomCurve() {
	}

	void AddPoint(const Point &p, const float size, const Spectrum &col,
			const float transp, const UV &uv) {
		points.push_back(p);
		sizes.push_back(size);
		cols.push_back(col);
		transps.push_back(transp);
		uvs.push_back(uv);
	}

	void AdaptiveTessellate(const u_int maxDepth, const float error, vector<float> &values) {
		values.push_back(0.f);
		AdaptiveTessellate(0, maxDepth, error, values, 0.f, 1.f);
		values.push_back(1.f);

		sort(values.begin(), values.end());
	}

	Point EvaluatePoint(const float t) {
		const int count = (int)points.size();
		if (count > 2) {
			int segment = Floor2Int((count - 1) * t);
			segment = max(segment, 0);
			segment = min(segment, count - 2);

			const float ct = t * (count - 1) - segment;

			if (segment == 0)
				return CatmullRomSpline(points[0], points[0], points[1], points[2], ct);
			if (segment == count - 2)
				return CatmullRomSpline(points[count - 3], points[count - 2], points[count - 1], points[count - 1], ct);

			return CatmullRomSpline(points[segment - 1], points[segment], points[segment + 1], points[segment + 2], ct);
		} else if (count == 2)
			return (1.f - t) * points[0] + t * points[1];
		else if (count == 1)
			return points[0];
		else
			throw runtime_error("Internal error in CatmullRomCurve::EvaluatePoint()");
	}

	float EvaluateSize(const float t) {
		int count = (int)sizes.size();
		if (count > 2) {
			int segment = Floor2Int((count - 1) * t);
			segment = max(segment, 0);
			segment = min(segment, count - 2);

			const float ct = t * (count - 1) - segment;

			if (segment == 0)
				return CatmullRomSpline(sizes[0], sizes[0], sizes[1], sizes[2], ct);
			if (segment == count - 2)
				return CatmullRomSpline(sizes[count - 3], sizes[count - 2], sizes[count - 1], sizes[count - 1], ct);

			return CatmullRomSpline(sizes[segment - 1], sizes[segment], sizes[segment + 1], sizes[segment + 2], ct);
		} else if (count == 2)
			return (1.f - t) * sizes[0] + t * sizes[1];
		else if (count == 1)
			return sizes[0];
		else
			throw runtime_error("Internal error in CatmullRomCurve::EvaluateSize()");
	}

	Spectrum EvaluateColor(const float t) {
		int count = (int)cols.size();
		if (count > 2) {
			int segment = Floor2Int((count - 1) * t);
			segment = max(segment, 0);
			segment = min(segment, count - 2);

			const float ct = t * (count - 1) - segment;

			if (segment == 0)
				return CatmullRomSpline(cols[0], cols[0], cols[1], cols[2], ct);
			if (segment == count - 2)
				return CatmullRomSpline(cols[count - 3], cols[count - 2], cols[count - 1], cols[count - 1], ct);

			return CatmullRomSpline(cols[segment - 1], cols[segment], cols[segment + 1], cols[segment + 2], ct);
		} else if (count == 2)
			return (1.f - t) * cols[0] + t * cols[1];
		else if (count == 1)
			return cols[0];
		else
			throw runtime_error("Internal error in CatmullRomCurve::EvaluateColor()");
	}

	float EvaluateTransparency(const float t) {
		int count = (int)transps.size();
		if (count > 2) {
			int segment = Floor2Int((count - 1) * t);
			segment = max(segment, 0);
			segment = min(segment, count - 2);

			const float ct = t * (count - 1) - segment;

			if (segment == 0)
				return CatmullRomSpline(transps[0], transps[0], transps[1], transps[2], ct);
			if (segment == count - 2)
				return CatmullRomSpline(transps[count - 3], transps[count - 2], transps[count - 1], transps[count - 1], ct);

			return CatmullRomSpline(transps[segment - 1], transps[segment], transps[segment + 1], transps[segment + 2], ct);
		} else if (count == 2)
			return (1.f - t) * transps[0] + t * transps[1];
		else if (count == 1)
			return transps[0];
		else
			throw runtime_error("Internal error in CatmullRomCurve::EvaluateTransparency()");
	}

	UV EvaluateUV(const float t) {
		int count = (int)uvs.size();
		if (count > 2) {
			int segment = Floor2Int((count - 1) * t);
			segment = max(segment, 0);
			segment = min(segment, count - 2);

			const float ct = t * (count - 1) - segment;

			if (segment == 0)
				return CatmullRomSpline(uvs[0], uvs[0], uvs[1], uvs[2], ct);
			if (segment == count - 2)
				return CatmullRomSpline(uvs[count - 3], uvs[count - 2], uvs[count - 1], uvs[count - 1], ct);

			return CatmullRomSpline(uvs[segment - 1], uvs[segment], uvs[segment + 1], uvs[segment + 2], ct);
		} else if (count == 2)
			return (1.f - t) * uvs[0] + t * uvs[1];
		else if (count == 1)
			return uvs[0];
		else
			throw runtime_error("Internal error in CatmullRomCurve::EvaluateUV()");
	}

private:
	bool AdaptiveTessellate(const u_int depth, const u_int maxDepth, const float error,
			vector<float> &values, const float t0, const float t1) {
		if (depth >= maxDepth)
			return false;

		const float tmid = (t0 + t1) * .5f;

		const Point p0 = EvaluatePoint(t0);
		const Point pmid = EvaluatePoint(tmid);
		const Point p1 = EvaluatePoint(t1);
		
		const Vector vmid = pmid - p0;
		const Vector v = p1 - p0;

		// Check if the vectors are nearly parallel
		if (AbsDot(Normalize(vmid), Normalize(v)) < 1.f - .05f) {
			// Tessellate left side too
			const bool leftSide = AdaptiveTessellate(depth + 1, maxDepth, error, values, t0, tmid);
			const bool rightSide = AdaptiveTessellate(depth + 1, maxDepth, error, values, tmid, t1);

			if (leftSide || rightSide)
				values.push_back(tmid);

			return false;
		}

		//----------------------------------------------------------------------
		// Curve flatness check
		//----------------------------------------------------------------------

		// Calculate the distance between vmid and the segment
		const float distance = Cross(v, vmid).Length() / vmid.Length();

		// Check if the distance normalized with the segment length is
		// over the required error
		const float segmentLength = v.Length();
		if (distance / segmentLength > error) {
			// Tessellate left side too
			AdaptiveTessellate(depth + 1, maxDepth, error, values, t0, tmid);
			
			values.push_back(tmid);

			// Tessellate right side too
			AdaptiveTessellate(depth + 1, maxDepth, error, values, tmid, t1);

			return true;
		}
		
		//----------------------------------------------------------------------
		// Curve size check
		//----------------------------------------------------------------------

		const float s0 = EvaluateSize(t0);
		const float smid = EvaluateSize(tmid);
		const float s1 = EvaluateSize(t1);

		const float expectedSize = (s0 + s1) * .5f;
		if (fabsf(expectedSize - smid) > error) {
			// Tessellate left side too
			AdaptiveTessellate(depth + 1, maxDepth, error, values, t0, tmid);
			
			values.push_back(tmid);

			// Tessellate right side too
			AdaptiveTessellate(depth + 1, maxDepth, error, values, tmid, t1);

			return true;
		}

		return false;
	}

	float CatmullRomSpline(const float a, const float b, const float c, const float d, const float t) {
		const float t1 = (c - a) * .5f;
		const float t2 = (d - b) * .5f;

		const float h1 = +2 * t * t * t - 3 * t * t + 1;
		const float h2 = -2 * t * t * t + 3 * t * t;
		const float h3 = t * t * t - 2 * t * t + t;
		const float h4 = t * t * t - t * t;

		return b * h1 + c * h2 + t1 * h3 + t2 * h4;
	}

	Point CatmullRomSpline(const Point a, const Point b, const Point c, const Point d, const float t) {
		return Point(
				CatmullRomSpline(a.x, b.x, c.x, d.x, t),
				CatmullRomSpline(a.y, b.y, c.y, d.y, t),
				CatmullRomSpline(a.z, b.z, c.z, d.z, t));
	}

	Spectrum CatmullRomSpline(const Spectrum a, const Spectrum b, const Spectrum c, const Spectrum d, const float t) {
		return Spectrum(
				Clamp(CatmullRomSpline(a.c[0], b.c[0], c.c[0], d.c[0], t), 0.f, 1.f),
				Clamp(CatmullRomSpline(a.c[1], b.c[1], c.c[1], d.c[1], t), 0.f, 1.f),
				Clamp(CatmullRomSpline(a.c[2], b.c[2], c.c[2], d.c[2], t), 0.f, 1.f));
	}

	UV CatmullRomSpline(const UV a, const UV b, const UV c, const UV d, const float t) {
		return UV(
				Clamp(CatmullRomSpline(a.u, b.u, c.u, d.u, t), 0.f, 1.f),
				Clamp(CatmullRomSpline(a.v, b.v, c.v, d.v, t), 0.f, 1.f));
	}

	vector<Point> points;
	vector<float> sizes;
	vector<Spectrum> cols;
	vector<float> transps;
	vector<UV> uvs;
};

//------------------------------------------------------------------------------
// StrendsShape methods
//------------------------------------------------------------------------------

StrendsShape::StrendsShape(SceneConstRef scene,
		const cyHairFile *hairFile, const TessellationType tesselType,
		const u_int aMaxDepth, const float aError, const u_int sSideCount,
		const bool sCapBottom, const bool sCapTop, const bool useCamPos) {
	adaptiveMaxDepth = aMaxDepth;
	adaptiveError = aError;
	solidSideCount = sSideCount;
	solidCapBottom = sCapBottom;
	solidCapTop = sCapTop;
	useCameraPosition = useCamPos;

	const cyHairFileHeader &header = hairFile->GetHeader();
	if (header.hair_count == 0)
		throw runtime_error("Empty strands shape are not supported");
	if (useCameraPosition && !scene.HasCamera())
		throw runtime_error(
			"The scene.GetCamera() must be defined "
			"in order to enable strands useCameraPosition flag"
		);

	SLG_LOG("Refining " << header.hair_count << " strands");
	const double start = WallClockTime();

	const auto points = hairFile->GetPointsArray();
	const auto thickness = hairFile->GetThicknessArray();
	const auto segments = hairFile->GetSegmentsArray();
	const auto colors = hairFile->GetColorsArray();
	const auto transparency = hairFile->GetTransparencyArray();
	const auto uvs = hairFile->GetUVsArray();

	if (not segments.empty() || (header.d_segments > 0)) {
		u_int pointIndex = 0;

		vector<Point> hairPoints;
		vector<float> hairSizes;
		vector<Spectrum> hairCols;
		vector<float> hairTransps;
		vector<UV> hairUVs;

		vector<Point> meshVerts;
		vector<Normal> meshNorms;
		vector<Triangle> meshTris;
		vector<UV> meshUVs;
		vector<Spectrum> meshCols;
		vector<Vector> meshTangents;
		vector<float> meshStrandUs;
		vector<float> meshStrandRands;
		vector<float> meshTransps;

		// Native curve primitive data (Metal HWRT; see
		// dev-tools/metal_curve_design.md): object-space Catmull-Rom control
		// points (xyz + radius), mesh-local per-segment start indices and
		// per-control-point shading attributes. Emitted alongside the
		// tessellation; software paths keep using the triangles.
		vector<CurveControlPoint> curveCps;
		vector<u_int> curveSegIndices;
		vector<CurveCpAttr> curveCpAttrs;

		// E9 phase 5b: record the per-strand control-point layout so a
		// motion step can re-run this exact tessellation on new point
		// positions (see StrendsShape::TessellateMotionStep).
		auto strandRecipe = std::make_shared<ExtTriangleMesh::StrandMotionRecipe>();
		strandRecipe->tesselType = tesselType;
		strandRecipe->adaptiveMaxDepth = adaptiveMaxDepth;
		strandRecipe->adaptiveError = adaptiveError;
		strandRecipe->solidSideCount = solidSideCount;
		strandRecipe->solidCapBottom = solidCapBottom;
		strandRecipe->solidCapTop = solidCapTop;
		strandRecipe->useCameraPosition = useCameraPosition;

		for (u_int i = 0; i < header.hair_count; ++i) {
			// segmentSize must be signed
			const auto segmentSize = not segments.empty() ? segments[i] : header.d_segments;
			if ((segmentSize <= 0) || (pointIndex >= header.point_count))
				continue;

			// Collect the segment points and size
			hairPoints.clear();
			hairSizes.clear();
			hairCols.clear();
			hairTransps.clear();
			hairUVs.clear();
			for (int j = 0; j <= segmentSize; ++j) {
				// A corrupted .hair file may declare more segment points
				// than header.point_count: never read past the arrays
				if (pointIndex >= header.point_count)
					break;
				hairPoints.push_back(Point(points[pointIndex * 3], points[pointIndex * 3 + 1], points[pointIndex * 3 + 2]));
				hairSizes.push_back(((not thickness.empty()) ? thickness[pointIndex] : header.d_thickness) * .5f);
				if (not colors.empty())
					hairCols.push_back(Spectrum(colors[pointIndex * 3], colors[pointIndex * 3 + 1], colors[pointIndex * 3 + 2]));
				else
					hairCols.push_back(Spectrum(header.d_color[0], header.d_color[1], header.d_color[2]));
				if (not transparency.empty())
					hairTransps.push_back(1.f - transparency[pointIndex]);
				else
					hairTransps.push_back(1.f - header.d_transparency);
				if (not uvs.empty())
					hairUVs.push_back(UV(uvs[pointIndex * 2], uvs[pointIndex * 2 + 1]));
				else
					hairUVs.push_back(UV(0.f, j / (float)segmentSize));

				++pointIndex;
			}

			if (hairPoints.empty())
				continue;

			// Per-strand random (Cycles Hair Info > Random): a deterministic
			// value in [0,1) identical across every vertex of this strand,
			// derived from the strand index so it is stable frame to frame.
			u_int randHash = i * 2654435761u;
			randHash ^= randHash >> 16;
			randHash *= 2246822519u;
			randHash ^= randHash >> 13;
			const float strandRand = (randHash & 0x00FFFFFFu) / float(0x01000000u);
			const u_int vertsBefore = meshVerts.size();

			// Only strands that actually emit geometry join the recipe.
			strandRecipe->strandPointCounts.push_back((u_int)hairPoints.size());
			strandRecipe->pointSizes.insert(strandRecipe->pointSizes.end(),
					hairSizes.begin(), hairSizes.end());

			switch (tesselType) {
				case TESSEL_RIBBON:
					TessellateRibbon(scene, hairPoints, hairSizes, hairCols, hairUVs,
							hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
							meshCols, meshTransps, meshTangents, meshStrandUs);
					break;
				case TESSEL_RIBBON_ADAPTIVE:
					TessellateAdaptive(scene, false, hairPoints, hairSizes, hairCols, hairUVs,
							hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
							meshCols, meshTransps, meshTangents, meshStrandUs);
					break;
				case TESSEL_SOLID:
					TessellateSolid(scene, hairPoints, hairSizes, hairCols, hairUVs,
							hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
							meshCols, meshTransps, meshTangents, meshStrandUs);
					break;
				case TESSEL_SOLID_ADAPTIVE:
					TessellateAdaptive(scene, true, hairPoints, hairSizes, hairCols, hairUVs,
							hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
							meshCols, meshTransps, meshTangents, meshStrandUs);
					break;
				default:
					SLG_LOG("Unknown tessellation  type in an Strands Shape: " + ToString(tesselType));
			}

			// Emit this strand as uniform Catmull-Rom curve primitives for
			// the Metal HWRT path. Padded endpoints ([p0,p0,...,p_{n-1},
			// p_{n-1}]) give every segment its 4 control points contiguously;
			// this is the same spline model TessellateAdaptive() evaluates.
			const u_int strandPointCount = (u_int)hairPoints.size();
			if (strandPointCount >= 2) {
				const u_int cpBase = (u_int)curveCps.size();
				const float strandUSpan = 1.f / (float)(strandPointCount - 1);
				float strandIndexBits;
				memcpy(&strandIndexBits, &i, sizeof(float));
				for (u_int k = 0; k < strandPointCount + 2; ++k) {
					const u_int p = Min(Max((int)k - 1, 0), (int)strandPointCount - 1);
					curveCps.push_back({ hairPoints[p].x, hairPoints[p].y,
							hairPoints[p].z, hairSizes[p] });

					CurveCpAttr attr;
					attr.colR = hairCols[p].c[0];
					attr.colG = hairCols[p].c[1];
					attr.colB = hairCols[p].c[2];
					attr.alpha = hairTransps[p];
					attr.u = hairUVs[p].u;
					attr.v = hairUVs[p].v;
					attr.strandU = p * strandUSpan;
					attr.strandIndexBits = strandIndexBits;
					curveCpAttrs.push_back(attr);
				}
				for (u_int j = 0; j + 1 < strandPointCount; ++j)
					curveSegIndices.push_back(cpBase + j);
			}

			// Stamp the per-strand random onto every vertex this strand emitted.
			for (u_int v = vertsBefore; v < meshVerts.size(); ++v)
				meshStrandRands.push_back(strandRand);
		}

		// Normalize normals
		for (u_int i = 0; i < meshNorms.size(); ++i)
			meshNorms[i] = Normalize(meshNorms[i]);

		SLG_LOG("Strands mesh: " << meshTris.size() << " triangles");

		// Create the mesh
		VertexBuffer newMeshVerts(meshVerts);

		TriangleBuffer newMeshTris(meshTris);

		NormalBuffer newMeshNorms(meshNorms.size());
		newMeshNorms.Set(std::span<const luxrays::Normal>(meshNorms));

		auto newMeshUVs = std::make_shared<UV[]>(meshUVs.size());
		std::copy(meshUVs.begin(), meshUVs.end(), newMeshUVs.get());

		// Check if I have to include vertex colors too
		ExtMeshProp<Spectrum>::Layer newMeshCols = nullptr;
		const Spectrum spectrum1(1.f);
		for(const Spectrum &c: meshCols) {
			if (c == spectrum1) continue;

			// The mesh uses vertex colors
			SLG_LOG("Strands shape uses colors");

			newMeshCols = std::make_shared<Spectrum[]>(meshUVs.size());
			std::copy(
#if !defined(__clang__)
				std::execution::par,
#endif
				meshCols.begin(),
				meshCols.end(),
				newMeshCols.get()
			);
			break;
		}

		// Check if I have to include vertex alpha too
		ExtMeshProp<float>::Layer newMeshTransps = nullptr;
		for(const float &a: meshTransps) {
			if (a == 1.f) continue;

			// The mesh uses vertex alphas
			SLG_LOG("Strands shape uses alphas");

			newMeshTransps = std::make_shared<float[]>(meshTransps.size());
			std::copy(meshTransps.begin(), meshTransps.end(), newMeshTransps.get());
			break;
		}

		mesh = std::make_unique<ExtTriangleMesh>(
			std::move(newMeshVerts),
			std::move(newMeshTris),
			std::move(newMeshNorms),
			newMeshUVs,
			newMeshCols,
			newMeshTransps
		);

		if (!curveSegIndices.empty())
			mesh->SetCurveData(std::move(curveCps), std::move(curveSegIndices),
					std::move(curveCpAttrs));
		mesh->SetStrandMotionRecipe(strandRecipe);
		SLG_LOG("Strands curve data: " << mesh->GetCurveSegCount() << " segments, "
				<< mesh->GetCurveCpCount() << " control points");

		// Store the per-vertex strand tangent (object space) in the reserved
		// vertex AOV layers for the hair material
		if (meshTangents.size() == meshVerts.size()) {
			auto tangentX = std::make_shared<float[]>(meshTangents.size());
			auto tangentY = std::make_shared<float[]>(meshTangents.size());
			auto tangentZ = std::make_shared<float[]>(meshTangents.size());
			for (u_int i = 0; i < meshTangents.size(); ++i) {
				tangentX[i] = meshTangents[i].x;
				tangentY[i] = meshTangents[i].y;
				tangentZ[i] = meshTangents[i].z;
			}
			mesh->SetVertexAOV(HAIR_TANGENT_X_DATA_INDEX, tangentX, meshTangents.size());
			mesh->SetVertexAOV(HAIR_TANGENT_Y_DATA_INDEX, tangentY, meshTangents.size());
			mesh->SetVertexAOV(HAIR_TANGENT_Z_DATA_INDEX, tangentZ, meshTangents.size());
		}

		// Store the per-vertex normalized strand position (0 = root, 1 = tip)
		// in its reserved vertex AOV layer for the Cycles Hair Info > Intercept
		// output.
		if (meshStrandUs.size() == meshVerts.size()) {
			auto strandU = std::make_shared<float[]>(meshStrandUs.size());
			std::copy(meshStrandUs.begin(), meshStrandUs.end(), strandU.get());
			mesh->SetVertexAOV(HAIR_STRAND_U_DATA_INDEX, strandU, meshStrandUs.size());
		}

		// Store the per-strand random (Cycles Hair Info > Random) in its
		// reserved vertex AOV layer.
		if (meshStrandRands.size() == meshVerts.size()) {
			auto strandRand = std::make_shared<float[]>(meshStrandRands.size());
			std::copy(meshStrandRands.begin(), meshStrandRands.end(), strandRand.get());
			mesh->SetVertexAOV(HAIR_STRAND_RANDOM_DATA_INDEX, strandRand, meshStrandRands.size());
		}
	} else
		throw runtime_error("Strands shape without segments are not supported");

	const float dt = WallClockTime() - start;
	SLG_LOG("Refining time: " << std::setprecision(3) << dt << " secs");
}

StrendsShape::StrendsShape(
		const ExtTriangleMesh::StrandMotionRecipe &recipe) {
	adaptiveMaxDepth = recipe.adaptiveMaxDepth;
	adaptiveError = recipe.adaptiveError;
	solidSideCount = recipe.solidSideCount;
	solidCapBottom = recipe.solidCapBottom;
	solidCapTop = recipe.solidCapTop;
	useCameraPosition = recipe.useCameraPosition;
}

bool StrendsShape::TessellateMotionStep(SceneConstRef scene,
		const ExtTriangleMesh::StrandMotionRecipe &recipe,
		const float *points, vector<Point> &meshVerts,
		vector<CurveControlPoint> *stepCurveCps) {
	StrendsShape shape(recipe);

	// Attribute inputs are required by the Tessellate*() signatures but
	// only positions feed `meshVerts` — shading attributes stay the base
	// pose's, recorded once at construction.
	const Spectrum white(1.f);
	u_int pointIndex = 0;
	const u_int totalPoints = recipe.GetTotalPointCount();

	// These accumulate across strands exactly like the constructor's —
	// Tessellate*() index them globally (baseOffset + vertex), so they
	// must live outside the per-strand loop.
	vector<Normal> meshNorms;
	vector<Triangle> meshTris;
	vector<UV> meshUVs;
	vector<Spectrum> meshCols;
	vector<float> meshTransps;
	vector<Vector> meshTangents;
	vector<float> meshStrandUs;

	for (const u_int pointCount : recipe.strandPointCounts) {
		if (pointIndex + pointCount > totalPoints)
			return false;

		vector<Point> hairPoints;
		hairPoints.reserve(pointCount);
		for (u_int j = 0; j < pointCount; ++j) {
			const u_int p = pointIndex + j;
			hairPoints.push_back(Point(points[p * 3], points[p * 3 + 1], points[p * 3 + 2]));
		}
		const vector<float> hairSizes(recipe.pointSizes.begin() + pointIndex,
				recipe.pointSizes.begin() + pointIndex + pointCount);
		vector<Spectrum> hairCols(pointCount, white);
		vector<float> hairTransps(pointCount, 1.f);
		vector<UV> hairUVs;
		hairUVs.reserve(pointCount);
		for (u_int j = 0; j < pointCount; ++j)
			hairUVs.push_back(UV(0.f, (pointCount > 1) ? j / float(pointCount - 1) : 0.f));

		switch ((TessellationType)recipe.tesselType) {
			case TESSEL_RIBBON:
				shape.TessellateRibbon(scene, hairPoints, hairSizes, hairCols, hairUVs,
						hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
						meshCols, meshTransps, meshTangents, meshStrandUs);
				break;
			case TESSEL_RIBBON_ADAPTIVE:
				shape.TessellateAdaptive(scene, false, hairPoints, hairSizes, hairCols, hairUVs,
						hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
						meshCols, meshTransps, meshTangents, meshStrandUs);
				break;
			case TESSEL_SOLID:
				shape.TessellateSolid(scene, hairPoints, hairSizes, hairCols, hairUVs,
						hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
						meshCols, meshTransps, meshTangents, meshStrandUs);
				break;
			case TESSEL_SOLID_ADAPTIVE:
				shape.TessellateAdaptive(scene, true, hairPoints, hairSizes, hairCols, hairUVs,
						hairTransps, meshVerts, meshNorms, meshTris, meshUVs,
						meshCols, meshTransps, meshTangents, meshStrandUs);
				break;
			default:
				return false;
		}

		// Same padded Catmull-Rom layout the constructor emits for the
		// native curve path: per strand, [p0, p0, p1, ..., pn-1, pn-1].
		if (stepCurveCps && pointCount >= 2) {
			for (u_int k = 0; k < pointCount + 2; ++k) {
				const u_int p = Min(Max((int)k - 1, 0), (int)pointCount - 1);
				stepCurveCps->push_back({ hairPoints[p].x, hairPoints[p].y,
						hairPoints[p].z, hairSizes[p] });
			}
		}
		pointIndex += pointCount;
	}
	return pointIndex == totalPoints;
}

void StrendsShape::TessellateRibbon(SceneConstRef scene,
		const vector<Point> &hairPoints,
		const vector<float> &hairSizes, const vector<Spectrum> &hairCols,
		const vector<UV> &hairUVs, const vector<float> &hairTransps,
		vector<Point> &meshVerts, vector<Normal> &meshNorms,
		vector<Triangle> &meshTris, vector<UV> &meshUVs, vector<Spectrum> &meshCols,
		vector<float> &meshTransps, vector<Vector> &meshTangents,
		vector<float> &meshStrandUs) const {
	// Create the mesh vertices
	const u_int baseOffset = meshVerts.size();
	// Normalized position along the strand (0 = root, 1 = tip)
	const float strandUStep = 1.f / Max(1, (int)hairPoints.size() - 1);

	const Point cameraPosition =
		(useCameraPosition && (scene.GetCamera().GetType() == Camera::PERSPECTIVE)) ?
		(dynamic_cast<const PerspectiveCamera&>(scene.GetCamera())).orig :
		Point();

	Vector previousDir;
	Vector previousX;
	// I'm using quaternion here in order to avoid Gimbal lock problem
	Quaternion trans;
	for (int i = 0; i < (int)hairPoints.size(); ++i) {
		Vector dir;
		// I need a special case for the very last point. Guard against
		// degenerate strands (single or coincident points): they used to
		// read hairPoints[-1] and produce NaN directions.
		if (hairPoints.size() < 2) {
			dir = Vector(0.f, 0.f, 1.f);
		} else if (i == (int)hairPoints.size() - 1) {
			const Vector d = hairPoints[i] - hairPoints[i - 1];
			dir = (d.LengthSquared() > 0.f) ? Normalize(d) : previousDir;
		} else {
			const Vector d = hairPoints[i + 1] - hairPoints[i];
			dir = (d.LengthSquared() > 0.f) ? Normalize(d) : previousDir;
		}

		if (i == 0) {
			// Build the initial quaternion by establishing an initial (arbitrary)
			// frame

			// Check if I have to face the ribbon in a specific direction
			Vector up;
			if (useCameraPosition)
				up = Normalize(cameraPosition - hairPoints[i]);
			else
				up = Vector(1.f, 0.f, 0.f);

			if (AbsDot(dir, up) > 1.f - .05f) {
				up = Vector(0.f, 1.f, 0.f);
				if (AbsDot(dir, up) > 1.f - .05f)
					up = Vector(0.f, 0.f, 1.f);
			}

			if (hairPoints.size() > 1) {
				const Transform dirTrans = LookAt(hairPoints[0], hairPoints[1], up);
				trans = Quaternion(dirTrans.m);
			}
		} else {
			// Compose the new delta transformation with all old one
			trans = GetRotationBetween(previousDir, dir) * trans;
		}
		previousDir = dir;

		const Vector newPreviousX = trans.RotateVector(Vector(1.f, 0.f, 0.f));

		// Using this trick to have a section half way between previous and new one
		const Vector x = (i == 0) ? newPreviousX : (previousX + newPreviousX) * .5;

		previousX = newPreviousX;
		
		const Point p0 = hairPoints[i] + hairSizes[i] * x;
		const Point p1 = hairPoints[i] - hairSizes[i] * x;
		const float strandU = i * strandUStep;
		meshVerts.push_back(p0);
		meshNorms.push_back(Normal());
		meshTangents.push_back(dir);
		meshStrandUs.push_back(strandU);
		meshVerts.push_back(p1);
		meshNorms.push_back(Normal());
		meshTangents.push_back(dir);
		meshStrandUs.push_back(strandU);

		meshUVs.push_back(hairUVs[i]);
		meshUVs.push_back(hairUVs[i]);

		meshCols.push_back(hairCols[i]);
		meshCols.push_back(hairCols[i]);

		meshTransps.push_back(hairTransps[i]);
		meshTransps.push_back(hairTransps[i]);
	}

	// Triangulate the vertex mesh
	for (int i = 0; i < (int)hairPoints.size() - 1; ++i) {
		const u_int index = baseOffset + i * 2;

		const u_int i0 = index;
		const u_int i1 = index + 1;
		const u_int i2 = index + 2;
		const u_int i3 = index + 3;

		// First triangle
		meshTris.push_back(Triangle(i0, i1, i2));
		// First triangle normal
		const Normal n0 = Normal(Cross(meshVerts[i2] - meshVerts[i0], meshVerts[i1] - meshVerts[i0]));
		meshNorms[i0] += n0;
		meshNorms[i1] += n0;
		meshNorms[i2] += n0;

		// Second triangle
		meshTris.push_back(Triangle(i1, i3, i2));
		// Second triangle normal
		const Normal n1 = Normal(Cross(meshVerts[i2] - meshVerts[i1], meshVerts[i3] - meshVerts[i1]));
		meshNorms[i1] += n1;
		meshNorms[i2] += n1;
		meshNorms[i3] += n1;
	}
}

void StrendsShape::TessellateAdaptive(SceneConstRef scene,
		const bool solid, const vector<Point> &hairPoints,
		const vector<float> &hairSizes, const vector<Spectrum> &hairCols,
		const vector<UV> &hairUVs, const vector<float> &hairTransps,
		vector<Point> &meshVerts, vector<Normal> &meshNorms,
		vector<Triangle> &meshTris, vector<UV> &meshUVs, vector<Spectrum> &meshCols,
		vector<float> &meshTransps, vector<Vector> &meshTangents,
		vector<float> &meshStrandUs) const {
	// Interpolate the hair segments
	CatmullRomCurve curve;
	for (int i = 0; i < (int)hairPoints.size(); ++i)
		curve.AddPoint(hairPoints[i], hairSizes[i], hairCols[i],
				hairTransps[i], hairUVs[i]);

	// Tessellate the curve
	vector<float> values;
	curve.AdaptiveTessellate(adaptiveMaxDepth, adaptiveError, values);

	// Create the ribbon
	vector<Point> tesselPoints;
	vector<float> tesselSizes;
	vector<Spectrum> tesselCols;
	vector<float> tesselTransps;
	vector<UV> tesselUVs;
	for (u_int i = 0; i < values.size(); ++i) {
		tesselPoints.push_back(curve.EvaluatePoint(values[i]));
		tesselSizes.push_back(curve.EvaluateSize(values[i]));
		tesselCols.push_back(curve.EvaluateColor(values[i]));
		tesselTransps.push_back(curve.EvaluateTransparency(values[i]));
		tesselUVs.push_back(curve.EvaluateUV(values[i]));
	}

	if (solid)
		TessellateSolid(scene, tesselPoints, tesselSizes, tesselCols, tesselUVs, tesselTransps,
			meshVerts, meshNorms, meshTris, meshUVs, meshCols, meshTransps, meshTangents, meshStrandUs);
	else
		TessellateRibbon(scene, tesselPoints, tesselSizes, tesselCols, tesselUVs, tesselTransps,
			meshVerts, meshNorms, meshTris, meshUVs, meshCols, meshTransps, meshTangents, meshStrandUs);
}

void StrendsShape::TessellateSolid(SceneConstRef scene,
		const vector<Point> &hairPoints,
		const vector<float> &hairSizes, const vector<Spectrum> &hairCols,
		const vector<UV> &hairUVs, const vector<float> &hairTransps,
		vector<Point> &meshVerts, vector<Normal> &meshNorms,
		vector<Triangle> &meshTris, vector<UV> &meshUVs, vector<Spectrum> &meshCols,
		vector<float> &meshTransps, vector<Vector> &meshTangents,
		vector<float> &meshStrandUs) const {
	// Create the mesh vertices
	const u_int baseOffset = meshVerts.size();
	const float angleStep = Radians(360.f / solidSideCount);
	// Normalized position along the strand (0 = root, 1 = tip)
	const float strandUStep = 1.f / Max(1, (int)hairPoints.size() - 1);

	Vector previousDir;
	Vector previousX, previousY, previousZ;
	// I'm using quaternion here in order to avoid Gimbal lock problem
	Quaternion trans;
	for (int i = 0; i < (int)hairPoints.size(); ++i) {
		Vector dir;
		// I need a special case for the very last point. Guard against
		// degenerate strands (single or coincident points): they used to
		// read hairPoints[-1] and produce NaN directions.
		if (hairPoints.size() < 2) {
			dir = Vector(0.f, 0.f, 1.f);
		} else if (i == (int)hairPoints.size() - 1) {
			const Vector d = hairPoints[i] - hairPoints[i - 1];
			dir = (d.LengthSquared() > 0.f) ? Normalize(d) : previousDir;
		} else {
			const Vector d = hairPoints[i + 1] - hairPoints[i];
			dir = (d.LengthSquared() > 0.f) ? Normalize(d) : previousDir;
		}

		if (i == 0) {
			// Build the initial quaternion by establishing an initial (arbitrary)
			// frame

			Vector up(0.f, 0.f, 1.f);
			if (AbsDot(dir, up) > 1.f - .05f)
				up = Vector(1.f, 0.f, 0.f);

			if (hairPoints.size() > 1) {
				const Transform dirTrans = LookAt(hairPoints[0], hairPoints[1], up);
				trans = Quaternion(dirTrans.m);
			}
		} else {
			// Compose the new delta transformation with all old one
			trans = GetRotationBetween(previousDir, dir) * trans;
		}
		previousDir = dir;

		const Vector newPreviousX = trans.RotateVector(Vector(1.f, 0.f, 0.f));
		const Vector newPreviousY = trans.RotateVector(Vector(0.f, 1.f, 0.f));
		const Vector newPreviousZ = trans.RotateVector(Vector(0.f, 0.f, 1.f));

		// Using this trick to have a section half way between previous and new one
		const Vector x = (i == 0) ? newPreviousX : (previousX + newPreviousX) * .5;
		const Vector y = (i == 0) ? newPreviousY : (previousY + newPreviousY) * .5;
		const Vector z = (i == 0) ? newPreviousZ : (previousZ + newPreviousZ) * .5;

		previousX = newPreviousX;
		previousY = newPreviousY;
		previousZ = newPreviousZ;

		float angle = 0.f;
		for (u_int j = 0; j < solidSideCount; ++j) {
			const Point lp(hairSizes[i] * cosf(angle), hairSizes[i] * sinf(angle), 0.f);
			const Point p(
				x.x * lp.x + y.x * lp.y + z.x * lp.z + hairPoints[i].x,
				x.y * lp.x + y.y * lp.y + z.y * lp.z + hairPoints[i].y,
				x.z * lp.x + y.z * lp.y + z.z * lp.z + hairPoints[i].z);
			
			meshVerts.push_back(p);
			meshNorms.push_back(Normal());
			meshTangents.push_back(dir);
			meshStrandUs.push_back(i * strandUStep);
			meshUVs.push_back(hairUVs[i]);
			meshCols.push_back(hairCols[i]);
			meshTransps.push_back(hairTransps[i]);

			angle += angleStep;
		}
	}

	// Triangulate the vertex mesh
	for (int i = 0; i < (int)hairPoints.size() - 1; ++i) {
		const u_int index = baseOffset + i * solidSideCount;

		for (u_int j = 0; j < solidSideCount; ++j) {
			// Side face
			const u_int i0 = index + j;
			const u_int i1 = (j == solidSideCount - 1) ? index : (index + j + 1);
			const u_int i2 = index + j + solidSideCount;
			const u_int i3 = (j == solidSideCount - 1) ? (index + solidSideCount) : (index + j + solidSideCount  + 1);

			// First triangle
			meshTris.push_back(Triangle(i0, i2, i1));
			const Normal n0 = Normal(Cross(meshVerts[i2] - meshVerts[i0], meshVerts[i1] - meshVerts[i0]));
			meshNorms[i0] += n0;
			meshNorms[i1] += n0;
			meshNorms[i2] += n0;

			// Second triangle
			meshTris.push_back(Triangle(i1, i2, i3));
			const Normal n1 = Normal(Cross(meshVerts[i2] - meshVerts[i0], meshVerts[i3] - meshVerts[i0]));
			meshNorms[i1] += n1;
			meshNorms[i3] += n1;
			meshNorms[i2] += n1;
		}
	}

	if (solidCapTop && hairPoints.size() > 1) {
		// Add a top fan cap
		const u_int offset = meshVerts.size();
		const Normal n = Normal(Normalize(hairPoints[hairPoints.size() - 1] - hairPoints[hairPoints.size() - 2]));
		for (u_int j = 0; j < solidSideCount; ++j) {
			meshVerts.push_back(meshVerts[offset - solidSideCount + j]);
			meshNorms.push_back(n);
			meshTangents.push_back(Vector(n));
			meshStrandUs.push_back(1.f);
			meshUVs.push_back(hairUVs.back());
			meshCols.push_back(hairCols.back());
			meshTransps.push_back(hairTransps.back());
		}

		// Add the fan center
		meshVerts.push_back(hairPoints.back());
		meshNorms.push_back(n);
		meshTangents.push_back(Vector(n));
		meshStrandUs.push_back(1.f);
		meshUVs.push_back(hairUVs.back());
		meshCols.push_back(hairCols.back());
		meshTransps.push_back(hairTransps.back());

		const u_int i3 = meshVerts.size() - 1;
		for (u_int j = 0; j < solidSideCount; ++j) {
			const u_int i0 = offset + j;
			const u_int i1 = (j == solidSideCount - 1) ? offset : (offset + j + 1);

			meshTris.push_back(Triangle(i1, i0, i3));
		}
	}

	if (solidCapBottom && hairPoints.size() > 1) {
		// Add a bottom fan cap
		const u_int offset = meshVerts.size();
		const Normal n = Normal(Normalize(hairPoints[0] - hairPoints[1]));
		for (u_int j = 0; j < solidSideCount; ++j) {
			meshVerts.push_back(meshVerts[baseOffset + j]);
			meshNorms.push_back(n);
			meshTangents.push_back(-Vector(n));
			meshStrandUs.push_back(0.f);
			meshUVs.push_back(hairUVs[0]);
			meshCols.push_back(hairCols[0]);
			meshTransps.push_back(hairTransps[0]);
		}

		// Add the fan center
		meshVerts.push_back(hairPoints[0]);
		meshNorms.push_back(n);
		meshTangents.push_back(-Vector(n));
		meshStrandUs.push_back(0.f);
		meshUVs.push_back(hairUVs[0]);
		meshCols.push_back(hairCols[0]);
		meshTransps.push_back(hairTransps[0]);

		const u_int i3 = meshVerts.size() - 1;
		for (u_int j = 0; j < solidSideCount; ++j) {
			const u_int i0 = offset + j;
			const u_int i1 = (j == solidSideCount - 1) ? offset : (offset + j + 1);

			meshTris.push_back(Triangle(i0, i1, i3));
		}
	}
}

StrendsShape::~StrendsShape() {
}

ExtTriangleMeshUPtr StrendsShape::RefineImpl(SceneConstRef scene) {
	return std::move(mesh);
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
