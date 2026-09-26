/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#ifndef _SLG_LIGHTSTRATEGY_LIGHTBVH_H
#define	_SLG_LIGHTSTRATEGY_LIGHTBVH_H

#include "slg/lights/strategies/logpower.h"

namespace slg {

// OpenCL data types
namespace ocl {
using luxrays::ocl::Point;
using luxrays::ocl::Vector;
#include "slg/lights/strategies/lightbvh_types.cl"
}

//------------------------------------------------------------------------------
// LightStrategyLightBVH
//
// Position/normal-aware light hierarchy (Estevez & Kulla 2018,
// "Importance Sampling of Many Lights with Adaptive Tree Splitting",
// the technique behind the many-light samplers of Cycles/Arnold/
// RenderMan). TASK_ILLUMINATE builds a binary tree over the
// direct-sampling-enabled lights where each node carries a tight
// bound of the cluster's emitted importance at the receiver:
// geometric attenuation through the node bbox, the surface cosine
// through the bbox bounding cone, and emission orientation through a
// bounding cone over the child emission direction cones. Sampling
// descends the tree choosing each branch with probability
// proportional to its bound - O(log N) per pick, vs. the flat O(1)
// table that samples near/backfacing clusters as eagerly as the
// front-facing bright ones.
//
// Infinite/directional lights have no position to cluster: they are
// stored as flat leaves whose energy bypasses the spatial bound at
// every node (energyFlat vs. energyLocal fields), so they keep their
// power-proportional share of the picks.
//
// TASK_EMIT and TASK_INFINITE_ONLY keep the inherited log-power flat
// distribution - the hierarchy pays off only at receivers.
//------------------------------------------------------------------------------

class LightStrategyLightBVH : public LightStrategyLogPower {
public:
	LightStrategyLightBVH() : LightStrategyLogPower(TYPE_LIGHT_BVH),
		taskType(TASK_EMIT), minDist2(0.f) { }
	virtual ~LightStrategyLightBVH() { }

	virtual void Preprocess(SceneConstRef scene, const LightStrategyTask taskType,
			const bool useRTMode);

	// Used for direct light sampling
	virtual LightSourcePtr SampleLights(
			SceneConstRef scene,
			const float u,
			const luxrays::Point &p, const luxrays::Normal &n,
			const bool isVolume,
			float *pdf) const;
	virtual float SampleLightPdf(
			LightSourceConstRef light,
			const luxrays::Point &p, const luxrays::Normal &n,
			const bool isVolume) const;

	virtual LightStrategyType GetType() const { return GetObjectType(); }
	virtual std::string GetTag() const { return GetObjectTag(); }

	// Device upload path (CompiledScene::CompileLightStrategy)
	const std::vector<ocl::LightBVHNode> &GetNodes() const { return nodes; }
	const std::vector<u_int> &GetLightToLeaf() const { return lightToLeaf; }
	float GetMinDist2() const { return minDist2; }

	//--------------------------------------------------------------------------
	// Static methods used by LightStrategyRegistry
	//--------------------------------------------------------------------------

	static LightStrategyType GetObjectType() { return TYPE_LIGHT_BVH; }
	static std::string GetObjectTag() { return "LIGHT_BVH"; }
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static LightStrategyUPtr FromProperties(const luxrays::Properties &cfg);

protected:
	static luxrays::PropertiesUPtr GetDefaultProps();

private:
	// GPU parity: LightBVH_NodeImportance in lightbvh_funcs.cl mirrors
	// this bound - keep the two implementations identical.
	float NodeImportance(const ocl::LightBVHNode &node,
			const luxrays::Point &x, const luxrays::Normal &n,
			const bool isVolume) const;

	LightStrategyTask taskType;
	float minDist2;
	std::vector<ocl::LightBVHNode> nodes;
	std::vector<u_int> lightToLeaf;
};

}

#endif	/* _SLG_LIGHTSTRATEGY_LIGHTBVH_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
