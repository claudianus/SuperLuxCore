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

#ifndef _SLG_STRANDSSHAPE_H
#define	_SLG_STRANDSSHAPE_H

#include <string>
#include <vector>

#include "luxrays/usings.h"
#include "luxrays/core/exttrianglemesh.h"
#include "luxrays/utils/cyhair/cyHairFile.h"

#include "slg/shapes/shape.h"

namespace slg {

// Vertex AOV layers reserved by the strands tessellation for hair shading:
// the per-vertex strand tangent in object space (used by HairMaterial).
constexpr u_int HAIR_TANGENT_X_DATA_INDEX = 4;
constexpr u_int HAIR_TANGENT_Y_DATA_INDEX = 5;
constexpr u_int HAIR_TANGENT_Z_DATA_INDEX = 6;
// Normalized position along the strand, 0 at the root and 1 at the tip
// (the Cycles "Hair Info > Intercept" shading parameter).
constexpr u_int HAIR_STRAND_U_DATA_INDEX = 7;
// Deterministic per-strand random in [0,1), constant across every vertex of a
// strand (the Cycles "Hair Info > Random" shading parameter). Stored in a
// low-numbered AOV layer that strands shapes do not otherwise use.
constexpr u_int HAIR_STRAND_RANDOM_DATA_INDEX = 0;

class StrendsShape : public Shape {
public:
	typedef enum {
		TESSEL_RIBBON, TESSEL_RIBBON_ADAPTIVE,
		TESSEL_SOLID, TESSEL_SOLID_ADAPTIVE
	} TessellationType;

	StrendsShape(SceneConstRef scene,
			const luxrays::cyHairFile *hairFile, const TessellationType tesselType,
			const u_int adaptiveMaxDepth, const float adaptiveError, 
			const u_int solidSideCount, const bool solidCapBottom, const bool solidCapTop,
			const bool useCameraPosition);
	virtual ~StrendsShape();

	virtual ShapeType GetType() const override { return STRANDS; }

	// Per-strand motion blur (E9 phase 5b): re-run the same tessellation
	// on a shutter-step's control points, filling `meshVerts` with the
	// tessellated vertex stream. `points` is a flat xyz array holding
	// exactly the recipe's total control-point count. When `stepCurveCps`
	// is non-null it also receives the step's padded Catmull-Rom control
	// points (same layout as the base mesh's GetCurveCps()) so native
	// curve backends can keyframe them. Returns false when the step
	// produces a different vertex count than the base mesh (adaptive
	// tessellation can subdivide differently per pose) — the caller then
	// keeps the mesh static.
	static bool TessellateMotionStep(SceneConstRef scene,
			const luxrays::ExtTriangleMesh::StrandMotionRecipe &recipe,
			const float *points, std::vector<luxrays::Point> &meshVerts,
			std::vector<luxrays::CurveControlPoint> *stepCurveCps = nullptr);

	// Params-only construction used by TessellateMotionStep — no
	// tessellation work is done.
	explicit StrendsShape(
			const luxrays::ExtTriangleMesh::StrandMotionRecipe &recipe);

protected:
	virtual luxrays::ExtTriangleMeshUPtr RefineImpl(SceneConstRef scene) override;

	void TessellateRibbon(SceneConstRef scene,
		const std::vector<luxrays::Point> &hairPoints,
		const std::vector<float> &hairSizes, const std::vector<luxrays::Spectrum> &hairCols,
		const std::vector<luxrays::UV> &hairUVs, const std::vector<float> &hairTransps,
		std::vector<luxrays::Point> &meshVerts, std::vector<luxrays::Normal> &meshNorms,
		std::vector<luxrays::Triangle> &meshTris, std::vector<luxrays::UV> &meshUVs, std::vector<luxrays::Spectrum> &meshCols,
		std::vector<float> &meshTransps, std::vector<luxrays::Vector> &meshTangents,
		std::vector<float> &meshStrandUs) const;
	void TessellateAdaptive(SceneConstRef scene,
		const bool solid, const std::vector<luxrays::Point> &hairPoints,
		const std::vector<float> &hairSizes, const std::vector<luxrays::Spectrum> &hairCols,
		const std::vector<luxrays::UV> &hairUVs, const std::vector<float> &hairTransps,
		std::vector<luxrays::Point> &meshVerts, std::vector<luxrays::Normal> &meshNorms,
		std::vector<luxrays::Triangle> &meshTris, std::vector<luxrays::UV> &meshUVs, std::vector<luxrays::Spectrum> &meshCols,
		std::vector<float> &meshTransps, std::vector<luxrays::Vector> &meshTangents,
		std::vector<float> &meshStrandUs) const;
	void TessellateSolid(SceneConstRef scene,
		const std::vector<luxrays::Point> &hairPoints,
		const std::vector<float> &hairSizes, const std::vector<luxrays::Spectrum> &hairCols,
		const std::vector<luxrays::UV> &hairUVs, const std::vector<float> &hairTransps,
		std::vector<luxrays::Point> &meshVerts, std::vector<luxrays::Normal> &meshNorms,
		std::vector<luxrays::Triangle> &meshTris, std::vector<luxrays::UV> &meshUVs, std::vector<luxrays::Spectrum> &meshCols,
		std::vector<float> &meshTransps, std::vector<luxrays::Vector> &meshTangents,
		std::vector<float> &meshStrandUs) const;

	// Tessellation options
	u_int adaptiveMaxDepth;
	float adaptiveError;
	u_int solidSideCount;
	bool solidCapBottom, solidCapTop;
	bool useCameraPosition;

};

}

#endif	/* _SLG_STRANDSSHAPE_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
