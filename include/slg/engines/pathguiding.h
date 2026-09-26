/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * http://www.apache.org/licenses/LICENSE-2.0                              *
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
#include <memory>
#include <string>
#include <vector>

#include "luxrays/core/geometry/point.h"
#include "luxrays/core/geometry/vector.h"
#include "luxrays/core/geometry/normal.h"
#include "luxrays/utils/utils.h"
#include "luxrays/usings.h"

namespace slg {

//------------------------------------------------------------------------------
// PathGuidingCache (M4a: SD-tree + per-leaf vMF mixture)
//
// Practical path guiding in the spirit of Muller et al. 2017 with the
// production-grade upgrades described in doc/features/path-guiding-m4-design.md:
//
//  - Spatial: adaptive binary tree (SD-tree) over the scene bounding cube.
//    Leaves subdivide across training rounds when their share of the
//    incident flux exceeds a fraction of the total (flux-fraction split,
//    deepest where the field carries the most energy). Split position is
//    the recorded-arrival centroid on the node's longest axis.
//  - Directional: each warm leaf holds a small von Mises-Fisher mixture
//    (K <= VMF_K) fitted by weighted EM over the leaf's 128-bin directional
//    histogram at round swap. kappa ~ 0 degenerates to uniform sphere, so
//    no separate bin/uniform fallback representation is needed.
//  - Target: bins accumulate first AND second moment; the fit uses the
//    variance-aware target sqrt(E[x^2]) per direction (Rath et al. 2020)
//    instead of the raw mean radiance. LUX_PG_NOVA reverts to the mean.
//  - Product: surface queries cosine-weight the component selection
//    (approximate product at lobe level) and add a cosine-lobe floor;
//    volume queries use the raw mixture plus a uniform floor.
//    Sample()/Pdf() evaluate the identical compound distribution, so the
//    one-sample MIS in the path tracer stays exact.
//
// Concurrency: render threads share one cache. Records descend a frozen
// write tree (atomic pointer) and CAS-accumulate leaf statistics. Every
// SWAP_RECORDS record attempts, one thread wins the swap flag, builds a
// fitted read tree from the completed write stats plus a refined empty
// write tree, swaps both atomically, and defers deletion of the retired
// trees (in-flight lookups stay valid). Reads snapshot the read tree
// pointer once per query: sampling and MIS weights always agree with the
// round's field.
//
// GPU: unchanged contract - training records arrive through RecordBin()
// (16^3 cell + bin -> Record at the cell center), sampling reads
// SnapshotCoarseTable() which evaluates the fitted leaf model at coarse
// cell/bin centers. M4e will upload the flattened tree + vMF table.
//------------------------------------------------------------------------------

class PathGuidingCache {
public:
	// Record-side directional histogram resolution (write-side leaf stats)
	static const u_int DIR_PHI = 16;
	static const u_int DIR_THETA = 8;
	static const u_int DIR_BINS = DIR_PHI * DIR_THETA;
	// Legacy fine spatial grid: only used to map RecordBin() cell indices
	// (GPU M2b-2 drain) to record positions - NOT the field structure.
	static const u_int GRID_RES = 16;
	// Training round length: sides swap every this many record attempts
	static const unsigned long long SWAP_RECORDS = 1000000ULL;
	// Coarse GPU field layout (M2b): shared with the OpenCL port, which
	// must use the same numbers (see pathoclbase_funcs.cl)
	static const u_int COARSE_GRID = 8u;
	static const u_int COARSE_PHI = 8u;
	static const u_int COARSE_THETA = 4u;
	static const u_int COARSE_BINS = 32u;
	static const u_int COARSE_CHUNKS = 16u;
	static const u_int COARSE_CHUNK_CELLS = 32u;
	// A leaf guides only after this many recorded arrivals (before that the
	// bounce is BSDF-only; the records still accumulate)
	static const u_int WARMUP_RECORDS = 256;
	// Record clamp: raw throughput outliers (firefly paths) would spike
	// the field and make the guide chase them.
	static constexpr float RECORD_CLAMP = 25.f;
	// SD-tree: max subdivision depth (finest leaf edge = cubeSize / 256)
	static const u_int TREE_MAX_DEPTH = 12;
	// Leaf budget across the whole tree (hard cap; ~1.6KB stats per leaf)
	static const u_int TREE_MAX_LEAVES = 8192;
	// A leaf splits when its incident-flux share exceeds this fraction of
	// the round's total recorded flux (and it holds >= WARMUP records).
	static constexpr float SPLIT_FLUX_FRAC = .004f;
	// vMF mixture size per leaf
	static const u_int VMF_K = 4;
	// Defensive sampling floors: cosine lobe (surfaces) / uniform sphere
	// (volumes) admixture so the guided pdf never fully collapses where
	// the fitted mixture misses mass.
	static constexpr float FLOOR_W_SURFACE = .10f;
	static constexpr float FLOOR_W_VOLUME = .15f;

	PathGuidingCache(const luxrays::Point &cubeMin, float cubeSize);
	~PathGuidingCache();

	// Record arrival value at position p from direction wi (pointing back
	// toward the previous vertex): caller-provided incident-radiance
	// estimate (local direct light + emission, throughput-normalized).
	void Record(const luxrays::Point &p, const luxrays::Vector &wi, float flux) const;

	// Direct bin record for GPU-drained training data (M2b-2): fine cell
	// and directional bin in the legacy 16^3 x 128 layout are mapped to
	// the cell-center position + bin-center direction and re-recorded.
	// Returns false for out-of-range input (dropped).
	bool RecordBin(u_int cell, u_int bin, float flux) const;
	// Force a training-round swap now (M2b-2 drain cadence)
	void ForceSwap() const;

	// Is there enough training data to guide at p?
	bool CanGuide(const luxrays::Point &p) const;
	// Read-side leaf flux total at p (frozen within a round)
	float ReadTotal(const luxrays::Point &p) const;
	// Read-side leaf record count at p: the statistical-confidence input
	// for MixWeight (flux conflates brightness with sample support; a dim
	// but well-sampled leaf must still earn guide weight).
	float ReadCount(const luxrays::Point &p) const;
	// Read-side leaf informativeness at p (0 for uniform fields).
	float ReadPeak(const luxrays::Point &p) const;
	// Incident-radiance field estimate at (p, dir): leaf.total times the
	// raw fitted vMF mixture density (kappa~0 reads as the EM uniform
	// 1/4pi, NOT the cosine-lobe convention LobePdf uses on surfaces -
	// this is the pure learned field, with no sampling floor and no
	// cosine weighting, for RIS product-guiding target evaluation).
	// floorFrac > 0 mixes in that fraction of the leaf's uniform level
	// (total/4pi): callers that resample against this estimate need the
	// positive support it guarantees wherever the true field is nonzero.
	float IncidentEstimate(const luxrays::Point &p,
			const luxrays::Vector &dir, const float floorFrac = 0.f) const;
	// Informativeness gate: a leaf whose fitted mixture is no more
	// concentrated than the cosine fallback gains nothing from guiding -
	// the extra proposal noise only hurts. A cosine lobe scores
	// 4pi*E[p^2]-1 = 5/3 ~ 1.67. Measured on the regression scenes, the
	// ramp must open well above parity to stay neutral on broad fields:
	// peak <= ~3.5 -> 0, peak >= ~9 (kappa ~ 15+ lobe) -> 1.
	static float PeakGate(float peak) {
		static const bool kNoPeak = (getenv("LUX_PG_NOPEAK") != nullptr);
		if (kNoPeak)
			return 1.f;
		return luxrays::Clamp((peak - 3.5f) / 5.5f, 0.f, 1.f);
	}
	// M2c adaptive mixture: guide-side selection probability from the
	// read-side record count times the informativeness gate. Thin or
	// diffuse leaves sample mostly BSDF (the field is noise or flat
	// there); rich peaked leaves trust the guide. Any w in (0,1) keeps
	// the one-sample MIS exact (same w must weight the pdf at bounce + DL).
	static float MixWeight(float count, float peak) {
		static const float kW0 = []() {
			const char *e = getenv("LUX_PG_W0");
			return e ? (float)luxrays::Max(atof(e), 1.0) : 1024.f;
		}();
		static const bool kNoMix = (getenv("LUX_PG_NOMIX") != nullptr);
		if (kNoMix)
			return .5f;
		return luxrays::Min(count / (count + kW0), .75f) * PeakGate(peak);
	}

	// Sample a world-space outgoing direction at (p, n). Surface queries
	// cosine-weight the component selection (approximate f*Li product at
	// lobe granularity) plus a cosine-lobe floor. pdfW is the guide pdf
	// (per solid angle) of the returned direction, including the floor.
	// isotropic (volume scattering vertices): no cosine weighting, uniform
	// sphere floor - media scatter into the full sphere.
	// Returns false only for degenerate input.
	bool Sample(const luxrays::Point &p, const luxrays::Normal &n,
			float uBin, float uDir0, float uDir1,
			luxrays::Vector *sampledDir, float *pdfW,
			bool isotropic = false) const;

	// Guide pdf (per solid angle) of dir at (p, n), the exact same
	// compound distribution Sample() draws from.
	float Pdf(const luxrays::Point &p, const luxrays::Normal &n,
			const luxrays::Vector &dir, bool isotropic = false) const;

	// Persistent cache: dump/load the frozen read side (tree + leaf
	// mixtures). Used to train on CPU once and sample on GPU.
	bool Save(const std::string &path) const;
	// Returns nullptr (and logs) when the file is missing/incompatible
	static PathGuidingCache *Load(const std::string &path);

	// Read-side snapshot evaluated into the coarse GPU field layout
	// (16 chunks of 32 cells x (32 bins + total)): each coarse cell center
	// descends the read tree and the leaf mixture is evaluated at the 32
	// bin centers. Transitional until M4e uploads the flattened tree.
	void SnapshotCoarseTable(std::vector<float> *out) const;

	// Field bounds of the snapshot (for the GPU upload)
	luxrays::Point GetCubeMin() const { return cubeMin; }
	float GetCubeSize() const { return cubeSize; }

private:
	// Shared node layout for both trees (indices into the tree's own
	// nodes/leaves arrays).
	struct TreeNode {
		u_int child[2];   // inner nodes; ~0u for leaves
		u_int axis;       // split axis (0..2), ~0u for leaves
		float split;      // absolute split coordinate on axis
		u_int leaf;       // leaf index, leaf nodes only
	};

	// Write-side leaf statistics: directional first/second moments per bin
	// (variance-aware target), visitation vs signal counts, arrival-position
	// and direction moments (split centroid + EM seed).
	struct LeafStats {
		void Clear() const {
			for (u_int i = 0; i < DIR_BINS; ++i) {
				bins[i].store(0.f, std::memory_order_relaxed);
				binsSq[i].store(0.f, std::memory_order_relaxed);
				binCnt[i].store(0.f, std::memory_order_relaxed);
			}
			total.store(0.f, std::memory_order_relaxed);
			count.store(0.f, std::memory_order_relaxed);
			nz.store(0.f, std::memory_order_relaxed);
			posX.store(0.f, std::memory_order_relaxed);
			posY.store(0.f, std::memory_order_relaxed);
			posZ.store(0.f, std::memory_order_relaxed);
			dirX.store(0.f, std::memory_order_relaxed);
			dirY.store(0.f, std::memory_order_relaxed);
			dirZ.store(0.f, std::memory_order_relaxed);
		}
		// Seed this leaf's stats from another leaf's accumulated stats.
		// Spatial stats (count, position moments) scale by fSpat - a
		// centroid split really does halve the record population.
		// Directional stats scale by fDir, capped separately: the parent
		// histogram is only a prior (it was integrated over the parent's
		// whole region, so it is spatially smeared for the child); left
		// uncapped it swamps the child's own sparse-but-peaked data.
		void CopyFrom(const LeafStats &o, const float fSpat,
				const float fDir) const {
			for (u_int i = 0; i < DIR_BINS; ++i) {
				bins[i].store(o.bins[i].load(std::memory_order_relaxed) * fDir,
						std::memory_order_relaxed);
				binsSq[i].store(o.binsSq[i].load(std::memory_order_relaxed) * fDir,
						std::memory_order_relaxed);
				binCnt[i].store(o.binCnt[i].load(std::memory_order_relaxed) * fDir,
						std::memory_order_relaxed);
			}
			total.store(o.total.load(std::memory_order_relaxed) * fDir, std::memory_order_relaxed);
			count.store(o.count.load(std::memory_order_relaxed) * fSpat, std::memory_order_relaxed);
			nz.store(o.nz.load(std::memory_order_relaxed) * fDir, std::memory_order_relaxed);
			posX.store(o.posX.load(std::memory_order_relaxed) * fSpat, std::memory_order_relaxed);
			posY.store(o.posY.load(std::memory_order_relaxed) * fSpat, std::memory_order_relaxed);
			posZ.store(o.posZ.load(std::memory_order_relaxed) * fSpat, std::memory_order_relaxed);
			dirX.store(o.dirX.load(std::memory_order_relaxed) * fDir, std::memory_order_relaxed);
			dirY.store(o.dirY.load(std::memory_order_relaxed) * fDir, std::memory_order_relaxed);
			dirZ.store(o.dirZ.load(std::memory_order_relaxed) * fDir, std::memory_order_relaxed);
		}
		mutable std::atomic<float> bins[DIR_BINS];
		mutable std::atomic<float> binsSq[DIR_BINS];
		mutable std::atomic<float> binCnt[DIR_BINS];
		mutable std::atomic<float> total;
		// count = visitation (all arrivals); nz = records that carried
		// signal. Warmup/eligibility key on count: a dim but well-visited
		// leaf still has a usable directional estimate, and sparse-signal
		// leaves are exactly the surfaces guiding exists for.
		mutable std::atomic<float> count;
		mutable std::atomic<float> nz;
		mutable std::atomic<float> posX, posY, posZ;
		mutable std::atomic<float> dirX, dirY, dirZ;
	};

	struct WriteTree {
		std::vector<TreeNode> nodes;
		// Fixed-size per round: non-copyable atomics forbid vector growth
		std::unique_ptr<LeafStats[]> leaves;
		u_int leafCount = 0;
		// Nodes are emitted post-order, so the root is NOT index 0.
		u_int root = 0;
	};

	struct ReadLeaf {
		float total = 0.f;   // round flux sum
		float count = 0.f;   // visitation count (all arrivals)
		float nz = 0.f;      // signal-bearing record count (fit support)
		float peak = 0.f;    // informativeness: 4pi*E[p^2]-1 (0 = uniform)
		u_int nComp = 0;     // 0 = cold leaf (no usable model)
		float w[VMF_K];
		float mu[VMF_K][3];
		float kappa[VMF_K];
	};

	struct ReadTree {
		std::vector<TreeNode> nodes;
		std::vector<ReadLeaf> leaves;
		u_int root = 0;
	};

	// Descend a frozen tree to the leaf index for p (root must exist).
	static u_int Descend(const std::vector<TreeNode> &nodes,
			u_int root, const luxrays::Point &p);
	// Read-side leaf at p, or nullptr when the tree is empty.
	const ReadLeaf *ReadLeafAt(const luxrays::Point &p) const;

	// Round swap: build the fitted read tree + a refined write tree from
	// the completed write stats, publish both, retire the old pair.
	void SwapTrees(bool forced = false) const;
	// Build helpers (run by the single swap winner; inputs are the
	// completed write stats of the retiring write tree).
	ReadTree *BuildReadTree(const WriteTree &wt) const;
	WriteTree *BuildWriteTree(const WriteTree &wt) const;
	// Fit one leaf's vMF mixture from its directional histogram.
	static void FitLeaf(const LeafStats &ls, ReadLeaf &out);
	// EM iterations over the 128 weighted bin centroids.
	// Per-query component selection weights (cosine-weighted for
	// surfaces, raw weights for volumes); returns their sum.
	static float CompWeights(const ReadLeaf &leaf, const luxrays::Vector &n,
			bool isotropic, float *s);
	static u_int DirBin(const luxrays::Vector &dir);
	static luxrays::Vector BinDir(u_int bin, float u0, float u1);
	static float BinSolidAngle() {
		return (2.f * M_PI / DIR_PHI) * (2.f / DIR_THETA);
	}
	// vMF pdf on the sphere: k e^{k(c-1)} / (2 pi (1-e^{-2k})); kappa ~ 0
	// is treated as the uniform distribution 1/(4pi).
	static float VmfPdf(float cosMuW, float kappa);
	// Exact vMF sample via closed-form z inversion + uniform phi
	// (kappa ~ 0 degenerates to uniform sphere).
	static luxrays::Vector VmfSample(const luxrays::Vector &mu, float kappa,
			float u0, float u1);
	// Mean resultant length A(kappa) = coth(k) - 1/k (component cosine
	// weight for surfaces); ~k/3 for small kappa.
	static float VmfMeanCos(float kappa);
	// Per-component density at a direction: vMF for kappa > 0; for
	// kappa ~ 0 the lobe reads as the cosine lobe on surfaces
	// (cosDirN/pi) and the uniform sphere in volumes.
	static float LobePdf(float kappa, float cosMuW, float cosDirN,
			bool isotropic) {
		if (kappa < 1e-3f)
			return isotropic ? (.25f * INV_PI) :
					((cosDirN > 0.f) ? cosDirN * INV_PI : 0.f);
		return VmfPdf(cosMuW, kappa);
	}
	static bool CosineSample(const luxrays::Vector &n, float u0, float u1,
			luxrays::Vector *sampledDir, float *pdfW);
	static void AtomicAdd(std::atomic<float> &v, float x) {
		float old = v.load(std::memory_order_relaxed);
		while (!v.compare_exchange_weak(old, old + x,
				std::memory_order_relaxed, std::memory_order_relaxed)) {
		}
	}

	luxrays::Point cubeMin;
	float cubeSize, invCubeSize;

	// Frozen read tree (queries) + active write tree (records); both
	// swapped atomically each round. Retired trees stay allocated a few
	// more rounds so in-flight lookups keep valid memory (epoch-style
	// reclamation; a query only touches the tree it loaded).
	mutable std::atomic<ReadTree *> readTree;
	mutable std::atomic<WriteTree *> writeTree;
	mutable std::vector<ReadTree *> retiredRead;
	mutable std::vector<WriteTree *> retiredWrite;
	mutable std::atomic<unsigned long long> writeRecords{0};
	mutable std::atomic<bool> swapFlag{false};
	// Frozen caches never swap: the read tree stays whatever was loaded
	// (tablefile warm start) or fitted at freeze time (LUX_PG_FREEZE).
	bool frozen = false;
};

}

#endif	/* _SLG_PATHGUIDING_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
