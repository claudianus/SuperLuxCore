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

#ifndef _SLG_MNEESEEDCACHE_H
#define	_SLG_MNEESEEDCACHE_H

// MNEE manifold seed cache (path.mnee.seedcache), shared header.
//
// Every converged single-vertex solve stores its vertex in a fixed-size
// world-space hash grid keyed by (endpoint id, occluder mesh, quantized
// blocker-hit position). A later attempt blocked by the same occluder
// region warm-starts Newton from the cached vertex instead of the line
// seed, which is what makes solves on a curved caster converge at all
// (the straight-line seed sits far outside the Newton basin there).
//
// Producers:
//   - pathtracer_mnee.cpp: stores on every converged solve (CPU) /
//     the mneeSeeds table (GPU kernel twin in pathoclbase_funcs.cl).
//   - tracephotonsthread.cpp (MPG-lite Phase B): caustic photon deposits
//     inject their last delta-specular vertex as a deferred MneeSeedRecord;
//     the same light sub-path crossed exactly the manifold an eye-side
//     MNEE connection needs to solve, so its specular vertex is a
//     zero-cost warm start. Records merge into this table after it is
//     allocated (PhotonGI preprocess can run before the table exists).
//
// The seed only selects the basin: the solver still verifies the
// half-vector constraint on re-projected surface vertices, so a stale,
// colliding or torn entry costs iterations but never biases the
// estimator.
//
// Entries also carry failure evidence (failCount): the solve outcome is
// near-deterministic per (cell, endpoint, occluder), so cells that
// repeatedly failed get a capped probe budget instead of a full solve.
// This bounds the cost of impossible manifold connections (silhouette
// cells, wrong-mode roots) without removing them - a capped probe that
// makes progress still converges inside its budget.

#include <atomic>

#include "luxrays/utils/utils.h"
#include "luxrays/core/geometry/point.h"
#include "luxrays/core/geometry/normal.h"

namespace slg {

#define MNEE_SEED_CACHE_SIZE_CPU (1u << 14)
#define MNEE_SEED_CELL_FRAC_CPU 64.f

struct MneeSeedEntry {
	std::atomic<float> vx{0.f}, vy{0.f}, vz{0.f};
	std::atomic<float> nx{0.f}, ny{0.f}, nz{0.f};
	std::atomic<u_int> lightIndex{0}, meshIndex{0};
	std::atomic<u_int> mirrorMode{0}, valid{0};
	// Energy-aware retention (manifold path guiding): the throughput
	// luminance of the solve that produced the entry. Colliding
	// stores keep the historically brighter basin.
	std::atomic<float> fluxWeight{0.f};
	// Failure evidence ("poisoned" slot): solves are deterministic in
	// (x0-cell, endpoint, occluder), so a failed solve almost always
	// fails again for the same key. Entries with valid == 0 and
	// failCount > 0 carry no seed vertex - they only tell the caller
	// this cell is a repeated Newton failure so it can run a cheap
	// capped probe instead of a full solve. A later converged solve
	// overwrites the slot and resets the counter (self-correcting).
	std::atomic<u_int> failCount{0};
};

// Deferred store record: a plain POD so photon-tracing threads (and any
// producer running before the shared table exists) can queue seeds for a
// later merge into the MneeSeedEntry grid.
struct MneeSeedRecord {
	luxrays::Point p;
	luxrays::Normal n;
	u_int key, lightIndex, meshIndex;
	u_int mirrorMode;
	float fluxWeight;
};

// The table is shared by all render threads: seeds are last-writer-wins
// hints (a stale entry only wastes Newton iterations, the constraint is
// still verified), so relaxed atomics are sufficient.
inline u_int MneeSeedKey(const u_int lightIndex, const u_int meshIndex,
		const luxrays::Point &p, const float cellSize) {
	const int cx = luxrays::Floor2Int(p.x / cellSize);
	const int cy = luxrays::Floor2Int(p.y / cellSize);
	const int cz = luxrays::Floor2Int(p.z / cellSize);
	u_int h = (u_int)cx * 73856093u ^ (u_int)cy * 19349663u ^
			(u_int)cz * 83492791u;
	h ^= lightIndex * 2654435761u;
	h ^= meshIndex * 40503u;
	h ^= h >> 16;
	h *= 2246822519u;
	h ^= h >> 13;
	return h & (MNEE_SEED_CACHE_SIZE_CPU - 1u);
}

// Two-way linear probing: every key probes slots {key, key+1}. The table
// is far smaller than the number of distinct (cell, endpoint, mesh)
// contexts, so colliding contexts evicted each other at a measurable
// rate; the second slot halves that churn. Probing is deterministic so
// lookups see the same pair stores used.
inline bool MneeSeedMatch(const MneeSeedEntry &e, const u_int lightIndex,
		const u_int meshIndex, const bool mirrorMode) {
	return (e.lightIndex.load(std::memory_order_relaxed) == lightIndex) &&
			(e.meshIndex.load(std::memory_order_relaxed) == meshIndex) &&
			(e.mirrorMode.load(std::memory_order_relaxed) ==
				(mirrorMode ? 1u : 0u));
}

inline u_int MneeSeedSlot2(const u_int key) {
	return (key + 1u) & (MNEE_SEED_CACHE_SIZE_CPU - 1u);
}

inline void MneeSeedStoreEntry(MneeSeedEntry &e,
		const luxrays::Point &p, const luxrays::Normal &n,
		const u_int lightIndex, const u_int meshIndex, const bool mirrorMode,
		const float fluxWeight) {
	e.vx.store(p.x, std::memory_order_relaxed);
	e.vy.store(p.y, std::memory_order_relaxed);
	e.vz.store(p.z, std::memory_order_relaxed);
	e.nx.store(n.x, std::memory_order_relaxed);
	e.ny.store(n.y, std::memory_order_relaxed);
	e.nz.store(n.z, std::memory_order_relaxed);
	e.lightIndex.store(lightIndex, std::memory_order_relaxed);
	e.meshIndex.store(meshIndex, std::memory_order_relaxed);
	e.mirrorMode.store(mirrorMode ? 1u : 0u, std::memory_order_relaxed);
	e.fluxWeight.store(fluxWeight, std::memory_order_relaxed);
	e.failCount.store(0u, std::memory_order_relaxed);
	e.valid.store(1u, std::memory_order_relaxed);
}

inline void MneeSeedStore(MneeSeedEntry *cache,
		const u_int key, const luxrays::Point &p, const luxrays::Normal &n,
		const u_int lightIndex, const u_int meshIndex, const bool mirrorMode,
		const float fluxWeight) {
	// Energy-aware retention (manifold path guiding, Fan et al. 2023): a
	// converged basin that historically carried more flux keeps the slot;
	// a fresher or brighter solve displaces it. Seeds only select the
	// Newton basin — retention cannot bias the estimator.
	for (u_int k = key, i = 0; i < 2; ++i, k = MneeSeedSlot2(k)) {
		MneeSeedEntry &e = cache[k];
		const bool match = MneeSeedMatch(e, lightIndex, meshIndex,
				mirrorMode);
		const bool valid = e.valid.load(std::memory_order_relaxed);
		if (valid && (e.fluxWeight.load(std::memory_order_relaxed) >
				fluxWeight)) {
			if (match)
				return;      // a brighter basin for our context stays
			continue;        // brighter foreign context: probe the next slot
		}
		MneeSeedStoreEntry(e, p, n, lightIndex, meshIndex, mirrorMode,
				fluxWeight);
		return;
	}
}

// Record a failed solve against this cell. Free slots are claimed as
// poison (namespaced fields set, valid stays 0); slots holding a seed
// for a different context are left alone; a seed for the same context
// accumulates evidence that the cached basin keeps failing.
inline void MneeSeedRecordFail(MneeSeedEntry *cache,
		const u_int key, const u_int lightIndex, const u_int meshIndex,
		const bool mirrorMode) {
	for (u_int k = key, i = 0; i < 2; ++i, k = MneeSeedSlot2(k)) {
		MneeSeedEntry &e = cache[k];
		const bool match = MneeSeedMatch(e, lightIndex, meshIndex,
				mirrorMode);
		if (e.valid.load(std::memory_order_relaxed)) {
			if (match &&
					(e.failCount.load(std::memory_order_relaxed) < 255u))
				e.failCount.fetch_add(1u, std::memory_order_relaxed);
			if (match)
				return;
			continue;
		}
		// Invalid slot: matching context accumulates evidence, foreign
		// context claims the slot as fresh poison.
		if (match) {
			if (e.failCount.load(std::memory_order_relaxed) < 255u)
				e.failCount.fetch_add(1u, std::memory_order_relaxed);
		} else {
			e.lightIndex.store(lightIndex, std::memory_order_relaxed);
			e.meshIndex.store(meshIndex, std::memory_order_relaxed);
			e.mirrorMode.store(mirrorMode ? 1u : 0u,
					std::memory_order_relaxed);
			e.failCount.store(1u, std::memory_order_relaxed);
		}
		return;
	}
}

inline void MneeSeedStore(MneeSeedEntry *cache, const MneeSeedRecord &r) {
	MneeSeedStore(cache, r.key, r.p, r.n, r.lightIndex, r.meshIndex,
			r.mirrorMode != 0u, r.fluxWeight);
}

}

#endif	/* _SLG_MNEESEEDCACHE_H */
