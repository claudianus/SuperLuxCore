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

#ifndef _SLG_PATHGUIDING_H
#define	_SLG_PATHGUIDING_H

#include <atomic>
#include <string>
#include <vector>

#include "luxrays/core/geometry/point.h"
#include "luxrays/core/geometry/vector.h"
#include "luxrays/core/geometry/normal.h"
#include "luxrays/utils/utils.h"
#include "luxrays/usings.h"

namespace slg {

//------------------------------------------------------------------------------
// PathGuidingCache (P1-3, M1)
//
// Practical path guiding in the spirit of Muller et al. 2017, scoped to a
// CPU-only milestone 1: a uniform spatial grid over the scene bounding cube,
// each cell holding incident-radiance bins over world-space directions
// (azimuth x elevation, equal-area). Paths record the local value
// (direct light + emission, throughput-normalized) at every non-delta
// bounce; glossy bounces sample the cache with a one-sample MIS against
// BSDF sampling, so any learned distribution (even a bad/empty one) stays
// unbiased -- the cache only affects variance.
//
// Concurrency: render threads share one cache. Records use lock-free float
// CAS accumulation into the write side; queries snapshot the read side.
// Every SWAP_RECORDS records the sides swap (the write side is cleared):
// frozen training rounds, so sampling and MIS weights always agree with
// the round's field and concurrent training only affects the next round.
// (This is also the GPU-ready split: train on CPU, sample a frozen field.)
//
// M2 (next): adaptive spatial/directional subdivision (SD-tree), product
// guiding.
//------------------------------------------------------------------------------

class PathGuidingCache {
public:
	// Fixed M1 resolution (uniform grid + uniform directional bins)
	static const u_int GRID_RES = 16;
	static const u_int DIR_PHI = 16;
	static const u_int DIR_THETA = 8;
	static const u_int DIR_BINS = DIR_PHI * DIR_THETA;
	// Training round length: sides swap every this many records
	static const unsigned long long SWAP_RECORDS = 1000000ULL;
	// Coarse GPU field layout (M2b): shared with the OpenCL port, which
	// must use the same numbers (see pathoclbase_funcs.cl)
	static const u_int COARSE_GRID = 8u;
	static const u_int COARSE_PHI = 8u;
	static const u_int COARSE_THETA = 4u;
	static const u_int COARSE_BINS = 32u;
	static const u_int COARSE_CHUNKS = 16u;
	static const u_int COARSE_CHUNK_CELLS = 32u;
	// A cell guides only after this many recorded arrivals (before that the
	// bounce is BSDF-only; the uniform records still accumulate)
	static const u_int WARMUP_RECORDS = 256;
	// Record clamp: raw throughput outliers (firefly paths) would spike
	// bins and make the guide chase them. M1 fixed heuristic.
	static constexpr float RECORD_CLAMP = 25.f;

	PathGuidingCache(const luxrays::Point &cubeMin, float cubeSize);

	// Record arrival value at position p from direction wi (pointing back
	// toward the previous vertex): caller-provided incident-radiance
	// estimate (local direct light + emission, throughput-normalized).
	void Record(const luxrays::Point &p, const luxrays::Vector &wi, float flux) const;

	// Direct bin record for GPU-drained training data (M2b-2): cell and
	// directional bin in the fine 16^3 x 128 layout, bounds-checked.
	// Returns false for out-of-range input (dropped).
	bool RecordBin(u_int cell, u_int bin, float flux) const;
	// Force a training-round swap now (M2b-2 drain cadence)
	void ForceSwap() const;

	// Is there enough training data to guide at p?
	bool CanGuide(const luxrays::Point &p) const;
	// Read-side total at p (frozen within a round; for mixture weights)
	float ReadTotal(const luxrays::Point &p) const {
		return cells[CellIndex(p)].total.load(std::memory_order_relaxed);
	}
	// M2c adaptive mixture: guide-side selection probability from the
	// read-side total. Thin cells sample mostly BSDF (the field is noise
		// there); rich cells trust the guide. Any w in (0,1) keeps the
	// one-sample MIS exact (same w must weight the pdf at bounce + DL).
	static float MixWeight(float total) {
		static const float kW0 = []() {
			const char *e = getenv("LUX_PG_W0");
			return e ? (float)luxrays::Max(atof(e), 1.0) : 1024.f;
		}();
		static const bool kNoMix = (getenv("LUX_PG_NOMIX") != nullptr);
		if (kNoMix)
			return .5f;
		return luxrays::Min(total / (total + kW0), .75f);
	}

	// Sample a world-space outgoing direction at (p, n). Cosine-weights the
	// bins by their centroid against n (bins facing away are never picked).
	// pdfW is the guide pdf (per solid angle) of the returned direction.
	// Returns false when the cell is cold (caller falls back to BSDF-only).
	// isotropic (volume scattering vertices): no cosine weighting and no
	// hemisphere cutoff - media scatter into the full sphere.
	bool Sample(const luxrays::Point &p, const luxrays::Normal &n,
			float uBin, float uDir0, float uDir1,
			luxrays::Vector *sampledDir, float *pdfW,
			bool isotropic = false) const;

	// Guide pdf (per solid angle) of dir at (p, n), using the same
	// cosine-weighted (resp. isotropic) snapshot convention as Sample.
	float Pdf(const luxrays::Point &p, const luxrays::Normal &n,
			const luxrays::Vector &dir, bool isotropic = false) const;

	// Persistent cache (M2b): dump/load the frozen read side
	// (4096 cells x (128 bins + total)). Used to train on CPU once and
	// sample on GPU.
	bool Save(const std::string &path) const;
	// Returns nullptr (and logs) when the file is missing/incompatible
	static PathGuidingCache *Load(const std::string &path);

	// Read-side snapshot for upload (cell-major: bins[128], total)
	void SnapshotTable(std::vector<float> *out) const;
	// Read-side snapshot downsampled to the coarse GPU field (M2b):
	// 16 chunks of 32 cells x (32 bins + total), mass-preserving
	// average pooling of the 16^3 x 128 field.
	void SnapshotCoarseTable(std::vector<float> *out) const;
	// Field bounds of the snapshot (for the GPU upload)
	luxrays::Point GetCubeMin() const { return cubeMin; }
	float GetCubeSize() const { return cubeSize; }

private:
	struct Cell {
		Cell() {
			Clear();
		}
		void Clear() const {
			for (u_int i = 0; i < DIR_BINS; ++i)
				bins[i].store(0.f, std::memory_order_relaxed);
			total.store(0.f, std::memory_order_relaxed);
			dirX.store(0.f, std::memory_order_relaxed);
			dirY.store(0.f, std::memory_order_relaxed);
			dirZ.store(0.f, std::memory_order_relaxed);
		}
		// Mutable so queries/training work through a const cache shared
		// by all render threads (lock-free; see the class comment).
		mutable std::atomic<float> bins[DIR_BINS];
		mutable std::atomic<float> total;
		// Directional first moment S1 = sum(wi * flux) for the vMF fit
		// (volume path guiding only; not serialized to disk/GPU tables).
		mutable std::atomic<float> dirX, dirY, dirZ;
	};

	u_int CellIndex(const luxrays::Point &p) const;
	// Bin of a world direction + its solid angle (equal-area bins)
	static u_int DirBin(const luxrays::Vector &dir);
	static luxrays::Vector BinDir(u_int bin, float u0, float u1);
	// Cosine-hemisphere fallback around n (valid distribution with exact
	// pdf = cos/PI); keeps Sample() total when the table has no usable
	// mass for this orientation. Returns false only for degenerate input.
	static bool CosineSample(const luxrays::Vector &n, float u0, float u1,
			luxrays::Vector *sampledDir, float *pdfW);
	static float BinSolidAngle() {
		return (2.f * M_PI / DIR_PHI) * (2.f / DIR_THETA);
	}
	// Snapshot a cell (bins + total) for self-consistent sample/evaluate
	void SnapshotCell(u_int cell, float *bins, float *total) const;
	// vMF fit of the cell's directional moment (volume isotropic mode).
	// A sharp incident field (god-ray beam, single dominant source) fits a
	// von Mises-Fisher lobe far better than 128 flat bins: exact closed-form
	// sampling + pdf, no rejection. Returns false when the field is too
	// cold or too isotropic (r = |S1|/S0 below threshold -> keep bins).
	bool CellVmf(u_int cell, luxrays::Vector &mu, float &kappa) const;
	// vMF pdf on the sphere: k e^{k(c-1)} / (2 pi (1-e^{-2k})), stable for
	// all k > 0.
	static float VmfPdf(float cosMuW, float kappa);
	// Exact vMF sample via closed-form z inversion + uniform phi.
	static luxrays::Vector VmfSample(const luxrays::Vector &mu, float kappa,
			float u0, float u1);
	// Trilinear blend of the 2x2x2 neighborhood masses (M2c-T experiment)
	void SnapshotBlend(const luxrays::Point &p, float *bins, float *total) const;
	// Swap training rounds when due (lock-free winner-takes-all; records
	// landing mid-swap may be dropped, which training tolerates)
	void MaybeSwap() const;

	static void AtomicAdd(std::atomic<float> &v, float x) {
		float old = v.load(std::memory_order_relaxed);
		while (!v.compare_exchange_weak(old, old + x,
				std::memory_order_relaxed, std::memory_order_relaxed)) {
		}
	}

	luxrays::Point cubeMin;
	float cubeSize, invCubeSize;
	// Read side (queries) + write side (records); swapped every round
	// (mutable: training works through a const shared cache)
	mutable std::vector<Cell> cells;
	mutable std::vector<Cell> cellsWrite;
	mutable std::atomic<unsigned long long> writeRecords{0};
	mutable std::atomic<bool> swapFlag{false};
};

}

#endif	/* _SLG_PATHGUIDING_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
