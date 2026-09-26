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

#include <algorithm>
#include <cmath>

#include "slg/lights/strategies/lightbvh.h"
#include "luxrays/core/geometry/vector_normal.h"
#include "luxrays/utils/properties.h"
#include "slg/lights/laserlight.h"
#include "slg/lights/mappointlight.h"
#include "slg/lights/mapspherelight.h"
#include "slg/lights/pointlight.h"
#include "slg/lights/projectionlight.h"
#include "slg/lights/spherelight.h"
#include "slg/lights/spotlight.h"
#include "slg/lights/trianglelight.h"
#include "slg/scene/scene.h"

using namespace std;
using namespace luxrays;
using namespace slg;

namespace {
	// Per-light build record. Flat lights (infinite/directional) carry
	// no spatial data: their energy is accumulated in the nodes'
	// energyFlat term, which bypasses every spatial bound.
	struct LightBVHEntry {
		u_int lightIndex;
		float energy;
		bool flat;
		// Local lights only
		Point bboxMin, bboxMax, centroid;
		Vector axis;
		float thetaO;
	};

	constexpr u_int LIGHTBVH_BINS = 16;

	inline Point PointMin(const Point &a, const Point &b) {
		return Point(Min(a.x, b.x), Min(a.y, b.y), Min(a.z, b.z));
	}
	inline Point PointMax(const Point &a, const Point &b) {
		return Point(Max(a.x, b.x), Max(a.y, b.y), Max(a.z, b.z));
	}
	inline Vector VectorMax(const Vector &a, const Vector &b) {
		return Vector(Max(a.x, b.x), Max(a.y, b.y), Max(a.z, b.z));
	}

	// Bounding-cone solid angle sr: omni (thetaO = PI) costs 4*PI, a
	// perfectly collimated cluster 0.
	inline float ConeMeasure(const float thetaO) {
		return 2.f * M_PI * (1.f - cosf(thetaO));
	}

	inline float BBoxArea(const Point &bmin, const Point &bmax) {
		const Vector d = bmax - bmin;
		return 2.f * (d.x * d.y + d.y * d.z + d.z * d.x);
	}

	// GPU node fields are ocl PODs ({x,y,z} only): convert at the
	// boundary, all math stays on the CPU-side types.
	inline slg::ocl::Point ToOCL(const Point &p) {
		slg::ocl::Point r; r.x = p.x; r.y = p.y; r.z = p.z; return r;
	}
	inline slg::ocl::Vector ToOCL(const Vector &v) {
		slg::ocl::Vector r; r.x = v.x; r.y = v.y; r.z = v.z; return r;
	}
	inline Point FromOCL(const slg::ocl::Point &p) { return Point(p.x, p.y, p.z); }
	inline Vector FromOCL(const slg::ocl::Vector &v) { return Vector(v.x, v.y, v.z); }

	// Max scale factor of a transform's linear part (for radius
	// expansion of sphere/laser lights under non-uniform transforms)
	float TransformMaxScale(const Transform &t) {
		const float c0 = t.m.m[0][0] * t.m.m[0][0] + t.m.m[1][0] * t.m.m[1][0] +
				t.m.m[2][0] * t.m.m[2][0];
		const float c1 = t.m.m[0][1] * t.m.m[0][1] + t.m.m[1][1] * t.m.m[1][1] +
				t.m.m[2][1] * t.m.m[2][1];
		const float c2 = t.m.m[0][2] * t.m.m[0][2] + t.m.m[1][2] * t.m.m[1][2] +
				t.m.m[2][2] * t.m.m[2][2];
		return sqrtf(Max(Max(c0, c1), c2));
	}

	// Recursive binned-SAO build (E&K'18 Sec. 4.3). Fills nodes[]
	// implicit-layout: internal node i has the left subtree in
	// [i+1, rightChildIndex). Returns the index just past this subtree
	// (= nodeIndex + 2*count - 1).
	u_int LightBVHBuild(vector<LightBVHEntry> &entries,
			vector<slg::ocl::LightBVHNode> &nodes, vector<u_int> &lightToLeaf,
			const u_int begin, const u_int end, const u_int nodeIndex) {
		slg::ocl::LightBVHNode &node = nodes[nodeIndex];
		node.flags = 0;
		node.axis = ToOCL(Vector(0.f, 0.f, 1.f));
		node.thetaO = 0.f;

		if (end - begin == 1) {
			// Leaf
			const LightBVHEntry &e = entries[begin];
			node.flags = 1u;
			node.u.lightIndex = e.lightIndex;
			node.energyFlat = e.flat ? e.energy : 0.f;
			node.energyLocal = e.flat ? 0.f : e.energy;
			if (e.flat) {
				node.bboxMin = node.bboxMax = ToOCL(Point(0.f));
			} else {
				node.bboxMin = ToOCL(e.bboxMin);
				node.bboxMax = ToOCL(e.bboxMax);
				node.axis = ToOCL(e.axis);
				node.thetaO = e.thetaO;
			}
			lightToLeaf[e.lightIndex] = nodeIndex;
			return nodeIndex + 1;
		}

		// Aggregate the range: local bbox, centroid bbox, axis sum,
		// energies (flat lights contribute only their energy)
		Point bMin(FLT_MAX, FLT_MAX, FLT_MAX), bMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		Point cMin(FLT_MAX, FLT_MAX, FLT_MAX), cMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		Vector axisSum(0.f);
		float eFlat = 0.f, eLocal = 0.f;
		u_int localCount = 0;
		for (u_int i = begin; i < end; ++i) {
			const LightBVHEntry &e = entries[i];
			if (e.flat)
				eFlat += e.energy;
			else {
				eLocal += e.energy;
				++localCount;
				bMin = PointMin(bMin, e.bboxMin);
				bMax = PointMax(bMax, e.bboxMax);
				cMin = PointMin(cMin, e.centroid);
				cMax = PointMax(cMax, e.centroid);
				axisSum += e.axis * e.energy;
			}
		}
		node.energyFlat = eFlat;
		node.energyLocal = eLocal;
		if (localCount == 0) {
			// Uniform flat subtree: the bounds are never evaluated
			node.bboxMin = node.bboxMax = ToOCL(Point(0.f));
		} else {
			node.bboxMin = ToOCL(bMin);
			node.bboxMax = ToOCL(bMax);
			node.axis = ToOCL((axisSum.LengthSquared() > 1e-12f) ?
					Normalize(axisSum) : Vector(0.f, 0.f, 1.f));
			node.thetaO = 0.f;
			const Vector nodeAxis = FromOCL(node.axis);
			for (u_int i = begin; i < end; ++i) {
				const LightBVHEntry &e = entries[i];
				if (!e.flat) {
					const float a = acosf(Clamp(Dot(nodeAxis, e.axis), -1.f, 1.f));
					node.thetaO = Max(node.thetaO, a + e.thetaO);
				}
			}
			node.thetaO = Min(node.thetaO, (float)M_PI);
		}

		//------------------------------------------------------------------
		// Binned SAO split over the local lights' centroids. Flat
		// entries always land in the last bin (they have no meaningful
		// position) so the tree peels them into uniform subtrees.
		//------------------------------------------------------------------

		u_int splitAt = begin + (end - begin) / 2; // midpoint fallback
		if ((localCount >= 2) && (cMin != cMax)) {
			struct Bin {
				u_int count = 0;
				float eFlat = 0.f, eLocal = 0.f;
				Point bMin = Point(FLT_MAX, FLT_MAX, FLT_MAX);
				Point bMax = Point(-FLT_MAX, -FLT_MAX, -FLT_MAX);
				Vector axisSum = Vector(0.f);
			};
			float bestCost = FLT_MAX;
			u_int bestAxis = 0, bestBin = 0;
			for (u_int axis = 0; axis < 3; ++axis) {
				const float c0 = cMin[axis], c1 = cMax[axis];
				const float inv = LIGHTBVH_BINS / Max(c1 - c0, 1e-30f);
				const auto binOf = [&](const LightBVHEntry &e) {
					return e.flat ? (LIGHTBVH_BINS - 1) :
							Min((u_int)(Max(0.f, (e.centroid[axis] - c0)) * inv),
									LIGHTBVH_BINS - 1);
				};
				Bin bins[LIGHTBVH_BINS];
				for (u_int i = begin; i < end; ++i) {
					const LightBVHEntry &e = entries[i];
					const u_int b = binOf(e);
					Bin &bin = bins[b];
					++bin.count;
					if (e.flat)
						bin.eFlat += e.energy;
					else {
						bin.eLocal += e.energy;
						bin.bMin = PointMin(bin.bMin, e.bboxMin);
						bin.bMax = PointMax(bin.bMax, e.bboxMax);
						bin.axisSum += e.axis * e.energy;
					}
				}
				for (u_int f = 1; f < LIGHTBVH_BINS; ++f) {
					Bin L, R;
					for (u_int b = 0; b < f; ++b) {
						L.count += bins[b].count;
						L.eFlat += bins[b].eFlat;
						L.eLocal += bins[b].eLocal;
						L.bMin = PointMin(L.bMin, bins[b].bMin);
						L.bMax = PointMax(L.bMax, bins[b].bMax);
						L.axisSum += bins[b].axisSum;
					}
					for (u_int b = f; b < LIGHTBVH_BINS; ++b) {
						R.count += bins[b].count;
						R.eFlat += bins[b].eFlat;
						R.eLocal += bins[b].eLocal;
						R.bMin = PointMin(R.bMin, bins[b].bMin);
						R.bMax = PointMax(R.bMax, bins[b].bMax);
						R.axisSum += bins[b].axisSum;
					}
					if ((L.count == 0) || (R.count == 0))
						continue;
					float thL = 0.f, thR = 0.f;
					const Vector aL = (L.axisSum.LengthSquared() > 1e-12f) ?
							Normalize(L.axisSum) : Vector(0.f, 0.f, 1.f);
					const Vector aR = (R.axisSum.LengthSquared() > 1e-12f) ?
							Normalize(R.axisSum) : Vector(0.f, 0.f, 1.f);
					for (u_int i = begin; i < end; ++i) {
						const LightBVHEntry &e = entries[i];
						if (e.flat)
							continue;
						const u_int b = binOf(e);
						const float a = acosf(Clamp(
								Dot((b < f) ? aL : aR, e.axis), -1.f, 1.f));
						if (b < f)
							thL = Max(thL, a + e.thetaO);
						else
							thR = Max(thR, a + e.thetaO);
					}
					thL = Min(thL, (float)M_PI);
					thR = Min(thR, (float)M_PI);
					// E&K'18 energy measure: energy * bboxArea *
					// coneSolidAngle; an all-flat side has no spatial
					// bound -> measure 1
					const float mL = (L.eLocal > 0.f) ?
							BBoxArea(L.bMin, L.bMax) * ConeMeasure(thL) : 1.f;
					const float mR = (R.eLocal > 0.f) ?
							BBoxArea(R.bMin, R.bMax) * ConeMeasure(thR) : 1.f;
					const float cost = (L.eFlat + L.eLocal) * mL +
							(R.eFlat + R.eLocal) * mR;
					if (cost < bestCost) {
						bestCost = cost;
						bestAxis = axis;
						bestBin = f;
					}
				}
			}
			if (bestCost < FLT_MAX) {
				// Partition entries into [begin, splitAt) | [splitAt, end)
				// by bin index along the winning axis
				const float c0 = cMin[bestAxis], c1 = cMax[bestAxis];
				const float inv = LIGHTBVH_BINS / Max(c1 - c0, 1e-30f);
				const u_int mid = u_int(std::stable_partition(
						entries.begin() + begin, entries.begin() + end,
						[&](const LightBVHEntry &e) {
							const u_int b = e.flat ? (LIGHTBVH_BINS - 1) :
									Min((u_int)(Max(0.f, (e.centroid[bestAxis] - c0)) * inv),
											LIGHTBVH_BINS - 1);
							return b < bestBin;
						}) - entries.begin());
				if ((mid > begin) && (mid < end))
					splitAt = mid;
			}
		}

		const u_int rightIndex = LightBVHBuild(entries, nodes, lightToLeaf,
				begin, splitAt, nodeIndex + 1);
		node.u.rightChildIndex = rightIndex;
		LightBVHBuild(entries, nodes, lightToLeaf, splitAt, end, rightIndex);
		return nodeIndex + 2 * (end - begin) - 1;
	}
}

//------------------------------------------------------------------------------
// LightStrategyLightBVH
//------------------------------------------------------------------------------

void LightStrategyLightBVH::Preprocess(SceneConstRef scene,
		const LightStrategyTask taskType_, const bool useRTMode) {
	taskType = taskType_;
	nodes.clear();
	lightToLeaf.clear();
	// The flat log-power distribution stays alive for every task: it
	// serves emit/infinite-only picks directly and doubles as the
	// device-side fallback when the BVH buffer is not uploaded
	LightStrategyLogPower::Preprocess(scene, taskType_, useRTMode);

	if (taskType_ != TASK_ILLUMINATE)
		return;

	const u_int lightCount = scene.GetLightSources().GetSize();
	if (lightCount == 0)
		return;

	const float sceneRad = scene.GetSceneBSphere().rad;
	// Geometric attenuation bound floor (see NodeImportance)
	minDist2 = sceneRad * sceneRad * 1e-6f;

	//----------------------------------------------------------------------
	// Per-light records
	//----------------------------------------------------------------------

	vector<LightBVHEntry> entries;
	entries.reserve(lightCount);
	lightToLeaf.assign(lightCount, LIGHTBVH_NULL_INDEX);

	for (u_int i = 0; i < lightCount; ++i) {
		auto& l = scene.GetLightSources().GetLightSource(i);
		if (!l.IsDirectLightSamplingEnabled())
			continue;
		const float rawPower = l.GetPower(scene) * l.GetImportance();
		const float energy = isfinite(rawPower) ?
				logf(1.f + Max(0.f, rawPower)) :
				((rawPower > 0.f) ? logf(FLT_MAX) : 0.f);

		LightBVHEntry e;
		e.lightIndex = i;
		e.energy = energy;
		e.axis = Vector(0.f, 0.f, 1.f);
		e.thetaO = 0.f;
		e.bboxMin = Point(FLT_MAX, FLT_MAX, FLT_MAX);
		e.bboxMax = Point(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		e.centroid = Point(0.f);

		if (l.IsInfinite()) {
			// Infinite/env/directional lights have no position to
			// cluster - they enter the tree as flat leaves
			e.flat = true;
		} else {
			e.flat = false;
			switch (l.GetType()) {
				case TYPE_TRIANGLE: {
					const TriangleLight &tl =
							static_cast<const TriangleLight&>(l);
					auto& mesh = tl.sceneObject->GetExtMesh();
					Transform l2w;
					mesh.GetLocal2World(0.f, l2w);
					const Triangle &tri = mesh.GetTriangles()[tl.triangleIndex];
					for (u_int k = 0; k < 3; ++k) {
						const Point v = mesh.GetVertex(l2w, tri.v[k]);
						e.bboxMin = PointMin(e.bboxMin, v);
						e.bboxMax = PointMax(e.bboxMax, v);
					}
					e.centroid = tl.worldCentroid;
					e.axis = Vector(tl.worldGeometryNormal);
					const float et = tl.lightMaterial->GetEmittedTheta();
					e.thetaO = (et == 0.f) ? 0.f :
							((et < 90.f) ? Radians(et) : (float)M_PI_2);
					break;
				}
				case TYPE_POINT:
				case TYPE_MAPPOINT: {
					const PointLight &pl = static_cast<const PointLight&>(l);
					e.bboxMin = e.bboxMax = e.centroid =
							pl.lightToWorld * pl.localPos;
					e.thetaO = M_PI; // omni
					break;
				}
				case TYPE_SPHERE:
				case TYPE_MAPSPHERE: {
					const SphereLight &sl = static_cast<const SphereLight&>(l);
					const Point c = sl.lightToWorld * sl.localPos;
					const float r = sl.radius * TransformMaxScale(sl.lightToWorld);
					e.bboxMin = c - Vector(r, r, r);
					e.bboxMax = c + Vector(r, r, r);
					e.centroid = c;
					e.thetaO = M_PI;
					break;
				}
				case TYPE_SPOT: {
					const SpotLight &sl = static_cast<const SpotLight&>(l);
					e.bboxMin = e.bboxMax = e.centroid =
							sl.lightToWorld * sl.localPos;
					e.axis = Normalize(sl.lightToWorld *
							Vector(sl.localTarget - sl.localPos));
					e.thetaO = Radians(sl.coneAngle + sl.coneDeltaAngle);
					break;
				}
				case TYPE_PROJECTION: {
					const ProjectionLight &pl =
							static_cast<const ProjectionLight&>(l);
					e.bboxMin = e.bboxMax = e.centroid =
							pl.lightToWorld * pl.localPos;
					e.axis = Normalize(pl.lightToWorld *
							Vector(pl.localTarget - pl.localPos));
					// Frustum corner rays reach ~fov*sqrt(2)/2 off
					// axis - bound with slack
					e.thetaO = Radians(pl.fov);
					break;
				}
				case TYPE_LASER: {
					const LaserLight &ll = static_cast<const LaserLight&>(l);
					const Point c = ll.lightToWorld * ll.localPos;
					const float r = ll.radius * TransformMaxScale(ll.lightToWorld);
					e.bboxMin = c - Vector(r, r, r);
					e.bboxMax = c + Vector(r, r, r);
					e.centroid = c;
					e.axis = Normalize(ll.lightToWorld *
							Vector(ll.localTarget - ll.localPos));
					e.thetaO = 0.f; // collimated
					break;
				}
				default:
					// Unknown local type: a position-less flat leaf is
					// always safe (uniform share of the picks)
					e.flat = true;
					break;
			}
		}
		entries.push_back(e);
	}

	if (entries.empty())
		return;

	// Flat leaves sort to the tail: at every binned split they land in
	// the last bin, so the tree peels them off into uniform subtrees
	// early instead of diluting the spatial clusters
	stable_sort(entries.begin(), entries.end(),
			[](const LightBVHEntry &a, const LightBVHEntry &b) {
				return (int)a.flat < (int)b.flat;
			});

	nodes.resize(2 * entries.size() - 1);
	LightBVHBuild(entries, nodes, lightToLeaf, 0, entries.size(), 0);
}

//------------------------------------------------------------------------------
// Importance bound at the receiver - GPU parity: keep identical to
// LightBVH_NodeImportance in lightbvh_funcs.cl
//------------------------------------------------------------------------------

float LightStrategyLightBVH::NodeImportance(const slg::ocl::LightBVHNode &node,
		const Point &x, const Normal &n, const bool isVolume) const {
	float imp = node.energyFlat;
	if (node.energyLocal > 0.f) {
		const Point bmin = FromOCL(node.bboxMin);
		const Point bmax = FromOCL(node.bboxMax);
		const Point c = (bmin + bmax) * 0.5f;
		const Vector c2x = x - c;
		const Vector q = VectorMax(VectorMax(bmin - x, x - bmax), Vector(0.f));
		const float d2 = q.LengthSquared();
		const float geo = 1.f / Max(d2, minDist2);

		const Vector diag = bmax - bmin;
		const float r2 = 0.25f * diag.LengthSquared();
		const float dc2 = c2x.LengthSquared();
		float cosSurf = 1.f, cosOrient = 1.f;
		if ((d2 > 0.f) && (dc2 > r2)) {
			const float thetaB = asinf(Min(sqrtf(r2 / dc2), 1.f));
			const Vector toC = -c2x / sqrtf(dc2);
			if (!isVolume) {
				const float aN = acosf(Clamp(Dot(n, toC), -1.f, 1.f));
				cosSurf = Max(0.f, cosf(Max(0.f, aN - thetaB)));
			}
			const float aO = acosf(Clamp(Dot(FromOCL(node.axis), -toC), -1.f, 1.f));
			cosOrient = Max(0.f, cosf(Max(0.f, aO - node.thetaO - thetaB)));
		}
		imp += node.energyLocal * geo * cosSurf * cosOrient;
	}
	return imp;
}

//------------------------------------------------------------------------------
// Sampling / pdf - GPU parity: identical control flow to
// LightBVH_SampleLights / LightBVH_SampleLightPdf (lightbvh_funcs.cl)
//------------------------------------------------------------------------------

LightSourcePtr LightStrategyLightBVH::SampleLights(SceneConstRef scene,
		const float u0, const Point &p, const Normal &n,
		const bool isVolume, float *pdf) const {
	if (taskType != TASK_ILLUMINATE)
		return DistributionLightStrategy::SampleLights(scene, u0, p, n,
				isVolume, pdf);
	if (nodes.empty()) {
		*pdf = 0.f;
		return nullptr;
	}
	float u = u0;
	float pickPdf = 1.f;
	u_int i = 0;
	for (;;) {
		const slg::ocl::LightBVHNode &node = nodes[i];
		if (node.flags & 1u) {
			*pdf = pickPdf;
			return LightSourcePtr(&scene.GetLightSources().
					GetLightSource(node.u.lightIndex));
		}
		const float impL = NodeImportance(nodes[i + 1], p, n, isVolume);
		const float impR = NodeImportance(nodes[node.u.rightChildIndex],
				p, n, isVolume);
		const float sum = impL + impR;
		const float pL = (sum > 0.f) ? (impL / sum) : 0.5f;
		if (u < pL) {
			pickPdf *= pL;
			u = (pL > 0.f) ? (u / pL) : 0.f;
			i = i + 1;
		} else {
			const float pR = 1.f - pL;
			pickPdf *= pR;
			u = (pR > 0.f) ? ((u - pL) / pR) : 0.f;
			i = node.u.rightChildIndex;
		}
	}
}

float LightStrategyLightBVH::SampleLightPdf(LightSourceConstRef light,
		const Point &p, const Normal &n, const bool isVolume) const {
	if (taskType != TASK_ILLUMINATE)
		return DistributionLightStrategy::SampleLightPdf(light, p, n, isVolume);
	if (nodes.empty())
		return 0.f;
	const u_int leaf = lightToLeaf[light.lightSceneIndex];
	if (leaf == LIGHTBVH_NULL_INDEX)
		return 0.f;
	float pickPdf = 1.f;
	u_int i = 0;
	for (;;) {
		const slg::ocl::LightBVHNode &node = nodes[i];
		if (node.flags & 1u)
			return pickPdf;
		const float impL = NodeImportance(nodes[i + 1], p, n, isVolume);
		const float impR = NodeImportance(nodes[node.u.rightChildIndex],
				p, n, isVolume);
		const float sum = impL + impR;
		const float pL = (sum > 0.f) ? (impL / sum) : 0.5f;
		if (leaf < node.u.rightChildIndex) {
			pickPdf *= pL;
			i = i + 1;
		} else {
			pickPdf *= (1.f - pL);
			i = node.u.rightChildIndex;
		}
	}
}

//------------------------------------------------------------------------------
// Static methods used by LightStrategyRegistry
//------------------------------------------------------------------------------

PropertiesUPtr LightStrategyLightBVH::ToProperties(const Properties &cfg) {
	PropertiesUPtr props = std::make_unique<Properties>();

	*props <<
			cfg.Get(GetDefaultProps()->Get("lightstrategy.type"));

	return props;
}

LightStrategyUPtr LightStrategyLightBVH::FromProperties(const Properties &cfg) {
	return std::make_unique<LightStrategyLightBVH>();
}

PropertiesUPtr LightStrategyLightBVH::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			LightStrategy::GetDefaultProps() <<
			Property("lightstrategy.type")(GetObjectTag());

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
