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

#ifndef _LUXRAYS_BVHACCEL_H
#define	_LUXRAYS_BVHACCEL_H

#include <memory>
#include <vector>

#include "luxrays/luxrays.h"
#include "luxrays/core/accelerator.h"
#include "luxrays/core/bvh/bvhbuild.h"

namespace luxrays {

class HardwareIntersectionDevice;

// BVHAccel Declarations
class BVHAccel : public Accelerator {
public:
	// BVHAccel Public Methods
	BVHAccel(const Context & context);
	virtual ~BVHAccel() = default;

	virtual AcceleratorType GetType() const { return ACCEL_BVH; }

	virtual bool HasNativeSupport(const IntersectionDevice &device) const;
	virtual bool HasHWSupport(const IntersectionDevice &device) const;

	virtual HardwareIntersectionKernelUPtr NewHardwareIntersectionKernel(HardwareIntersectionDevice &device) const override;

	virtual void Init(
		const std::deque<const Mesh *> &meshes,
		const u_longlong totalVertexCount,
		const u_longlong totalTriangleCount
	);

	virtual bool Intersect(const Ray *ray, RayHit *hit) const;

	// Spills the node array to a file-backed copy-on-write mapping under
	// `dir` when it exceeds `minBytes`. Used for pure device renders where
	// the host tree is dead weight after upload; reads stay transparent
	// through the mapping (re-uploads, edits, native Intersect).
	virtual size_t SpillBVHNodes(const std::string &dir,
			const std::string &prefix, size_t minBytes) const override;

	// Read-only access to the built GPU-layout tree: node array
	// (leaf vertex indices are MESH-LOCAL - apply per-mesh offsets)
	// and the mesh list order used at Init(). Metal/GPU backends and
	// dumpers need these to upload the exact same data the OpenCL
	// kernel receives (see BVHKernel in bvhaccelhw.cpp).
	const luxrays::ocl::BVHArrayNode *GetNodes(u_int *count = nullptr) const {
		if (count)
			*count = nNodes;
		return bvhTree.get();
	}
	const std::deque<const Mesh *> &GetMeshes() const { return meshes; }
	u_longlong GetTotalVertexCount() const { return totalVertexCount; }
	u_longlong GetTotalTriangleCount() const { return totalTriangleCount; }

	static BVHParams ToBVHParams(const Properties &props);

	friend class BVHKernel;
	friend class MBVHKernel;
	friend class MBVHAccel;

private:
	BVHParams params;

	u_int nNodes;
	// shared_ptr so the array can be swapped for a file mapping (aliasing
	// keeper); mutable so SpillBVHNodes() stays logically const
	mutable std::shared_ptr<luxrays::ocl::BVHArrayNode[]> bvhTree;

	const Context & ctx;
	std::deque<const Mesh *> meshes;
	u_longlong totalVertexCount, totalTriangleCount;

	bool initialized;
};

}

#endif	/* _LUXRAYS_BVHACCEL_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
