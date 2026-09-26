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

#ifndef _LUXRAYS_GEOMSTREAM_H
#define	_LUXRAYS_GEOMSTREAM_H

// Skeleton for >VRAM device-side geometry streaming. The .lxm cluster
// index already partitions meshes into bounded triangle runs; this
// class is the device-independent part of a cluster residency cache:
// a fixed-capacity slot pool with LRU eviction and a miss-request
// queue. A device backend (Metal sparse buffer, CUDA VMM, or a plain
// staging-buffer uploader) subscribes to fill/evict callbacks — the
// actual upload mechanics are backend-specific.
//
// NOTE: unverified on unified-memory hardware — no discrete VRAM
// pressure exists there. See AGENTS.md ">VRAM device streaming" for
// the full design.

#include <algorithm>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "luxrays/utils/utils.h"

namespace luxrays {

// Identifies one cluster: (meshIndex, clusterIdx) inside the mesh's
// cluster table.
struct StreamClusterRef {
	u_int meshIndex;
	u_int clusterIndex;

	bool operator==(const StreamClusterRef &o) const {
		return meshIndex == o.meshIndex && clusterIndex == o.clusterIndex;
	}
};

struct StreamClusterRefHash {
	size_t operator()(const StreamClusterRef &r) const {
		return ((size_t)r.meshIndex << 32) | r.clusterIndex;
	}
};

// Fixed-capacity LRU slot pool. Each slot holds one cluster's device
// data. `fill(slot, ref)` must upload the cluster's triangles/vertices
// (read from the .lxm mapping via the mesh's cluster table); `evict`
// releases the slot's device storage. Eviction is cheap because
// cluster data is read-only.
class ClusterResidencyPool {
public:
	// slotCount = device budget / worst-case cluster payload.
	ClusterResidencyPool(u_int slotCount) :
		slotCount(slotCount) {
		freeSlots.reserve(slotCount);
		for (u_int i = 0; i < slotCount; ++i)
			freeSlots.push_back(i);
	}

	// Callbacks installed by the device backend.
	std::function<void(u_int slot, const StreamClusterRef &ref)> onFill;
	std::function<void(u_int slot, const StreamClusterRef &ref)> onEvict;

	// Returns the slot for `ref`, filling/evicting as needed. The slot
	// is moved to the LRU tail (most recently used).
	u_int Acquire(const StreamClusterRef &ref) {
		auto it = slotByRef.find(ref);
		if (it != slotByRef.end()) {
			lru.erase(std::find(lru.begin(), lru.end(), ref));
			lru.push_back(ref);
			return it->second;
		}

		u_int slot;
		if (!freeSlots.empty()) {
			slot = freeSlots.back();
			freeSlots.pop_back();
		} else {
			// Evict the least recently used slot.
			const StreamClusterRef &victim = lru.front();
			slot = slotByRef[victim];
			if (onEvict)
				onEvict(slot, victim);
			slotByRef.erase(victim);
			lru.pop_front();
		}

		slotByRef[ref] = slot;
		lru.push_back(ref);
		if (onFill)
			onFill(slot, ref);
		return slot;
	}

	bool IsResident(const StreamClusterRef &ref) const {
		return slotByRef.count(ref) != 0;
	}

	u_int ResidentCount() const { return lru.size(); }

private:
	const u_int slotCount;
	std::vector<u_int> freeSlots;
	// LRU order: front = coldest.
	std::deque<StreamClusterRef> lru;
	std::unordered_map<StreamClusterRef, u_int, StreamClusterRefHash> slotByRef;
};

}

#endif /* _LUXRAYS_GEOMSTREAM_H */
