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

#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>

#include "luxrays/utils/mc.h"
#include "slg/engines/pathguiding.h"
#include "slg/slg.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Tunables (env; cold path only)
//------------------------------------------------------------------------------

// Env fallbacks for the cache Settings fields the engine leaves unset
// (path.guiding.* property wins; these keep the LUX_PG_* debug knobs
// working when no property is defined).
static unsigned long long GuideSwapRecordsDefault() {
	static const unsigned long long v = []() {
		const char *e = getenv("LUX_PG_SWAP");
		return e ? Max(1000ULL, (unsigned long long)atoll(e)) :
				PathGuidingCache::SWAP_RECORDS;
	}();
	return v;
}

// Warmup default when path.guiding.warmup is not set (env fallback).
// The value is FLOORED at WARMUP_RECORDS: the GPU kernel gate
// (GUIDE_WARMUP_RECORDS) is hardcoded, so any leaf the builder marks
// usable must carry a count that also passes the device-side check -
// a lower host warmup would emit nComp>0 leaves the kernel rejects.
static float GuideWarmupDefault() {
	static const float v = []() {
		const char *e = getenv("LUX_PG_WARMUP");
		return e ? (float)Max(atof(e), 1.0) : (float)PathGuidingCache::WARMUP_RECORDS;
	}();
	return Max(v, (float)PathGuidingCache::WARMUP_RECORDS);
}

// Split threshold: a leaf refines when its incident-flux share exceeds
// this fraction of the round's total (path.guiding.split; env
// LUX_PG_SPLIT fallback; default SPLIT_FLUX_FRAC).
static float GuideSplitFracDefault() {
	static const float v = []() {
		const char *e = getenv("LUX_PG_SPLIT");
		return e ? (float)Clamp(atof(e), 1e-6, 0.5) :
				PathGuidingCache::SPLIT_FLUX_FRAC;
	}();
	return v;
}

static u_int GuideMaxDepthDefault() {
	static const u_int v = []() {
		const char *e = getenv("LUX_PG_MAXDEPTH");
		return e ? (u_int)Clamp(atoi(e), 1, 12) : PathGuidingCache::TREE_MAX_DEPTH;
	}();
	return v;
}

static u_int GuideMaxLeavesDefault() {
	static const u_int v = []() {
		const char *e = getenv("LUX_PG_MAXLEAVES");
		return e ? (u_int)Clamp(atoi(e), 16, 65536) : PathGuidingCache::TREE_MAX_LEAVES;
	}();
	return v;
}

// Variance-aware target (Rath et al. 2020): fit the leaf mixture to
// sqrt(E[x^2]) per direction instead of E[x]. LUX_PG_NOVA reverts.
static bool GuideNoVA() {
	static const bool v = (getenv("LUX_PG_NOVA") != nullptr);
	return v;
}

// vMF concentration ceiling during EM. The 128-bin directional
// histogram resolves ~10-15 deg features, so lobes sharper than ~32
// overfit the quantization and leave near-zero-density holes inside
// the empirical support (huge importance weights at sampling time).
// LUX_PG_KCAP overrides for experiments.
static float GuideKappaCap() {
	static const float v = []() {
		const char *e = getenv("LUX_PG_KCAP");
		return e ? (float)Clamp(atof(e), 1.0, 512.0) : 32.f;
	}();
	return v;
}

// Fixed uniform component inside the EM mixture (PAVMM-style): absorbs
// diffuse mass and bounds the fitted density over the sphere. Its weight
// is fit by EM with a small floor. LUX_PG_NOUNI disables for experiments.
static bool GuideNoUni() {
	static const bool v = (getenv("LUX_PG_NOUNI") != nullptr);
	return v;
}

//------------------------------------------------------------------------------
// Small helpers
//------------------------------------------------------------------------------

void PathGuidingCache::AggStats::Add(const LeafStats &ls) {
	for (u_int i = 0; i < DIR_BINS; ++i) {
		bins[i] += ls.bins[i].load(std::memory_order_relaxed);
		binsSq[i] += ls.binsSq[i].load(std::memory_order_relaxed);
	}
	count += ls.count.load(std::memory_order_relaxed);
	nz += ls.nz.load(std::memory_order_relaxed);
	total += ls.total.load(std::memory_order_relaxed);
}

void PathGuidingCache::AggStats::Add(const AggStats &o) {
	for (u_int i = 0; i < DIR_BINS; ++i) {
		bins[i] += o.bins[i];
		binsSq[i] += o.binsSq[i];
	}
	count += o.count;
	nz += o.nz;
	total += o.total;
}

u_int PathGuidingCache::DirBin(const Vector &dir) {
	// Equal-area: phi uniform, cosTheta uniform in [-1, 1]
	const float phi = atan2f(dir.y, dir.x);  // [-pi, pi]
	const float c = Clamp(dir.z, -1.f, 1.f);
	u_int pi = Min(static_cast<u_int>((phi + M_PI) / (2.f * M_PI) * DIR_PHI), DIR_PHI - 1);
	u_int ti = Min(static_cast<u_int>((c * .5f + .5f) * DIR_THETA), DIR_THETA - 1);
	return ti * DIR_PHI + pi;
}

Vector PathGuidingCache::BinDir(u_int bin, float u0, float u1) {
	const u_int pi = bin % DIR_PHI;
	const u_int ti = bin / DIR_PHI;
	const float phi = ((pi + u0) / DIR_PHI) * 2.f * M_PI - M_PI;
	const float c = ((ti + u1) / DIR_THETA) * 2.f - 1.f;
	const float s = sqrtf(Max(0.f, 1.f - c * c));
	return Vector(s * cosf(phi), s * sinf(phi), c);
}

float PathGuidingCache::VmfPdf(const float cosMuW, const float kappa) {
	if (kappa < 1e-3f)
		return .25f * INV_PI;  // uniform sphere
	return kappa * expf(kappa * (cosMuW - 1.f)) /
			(2.f * M_PI * (1.f - expf(-2.f * kappa)));
}

Vector PathGuidingCache::VmfSample(const Vector &mu, const float kappa,
		const float u0, const float u1) {
	if (kappa < 1e-3f)
		return UniformSampleSphere(u0, u1);
	// cos(theta) ~ k e^{k(z-1)} on [-1,1]: closed-form inversion
	const float z = 1.f + logf(Max(u1 + (1.f - u1) * expf(-2.f * kappa),
			1e-30f)) / kappa;
	const float s = sqrtf(Max(0.f, 1.f - z * z));
	const float phi = 2.f * M_PI * u0;
	const Vector helper = (fabsf(mu.z) < .999f) ? Vector(0.f, 0.f, 1.f) :
			Vector(0.f, 1.f, 0.f);
	const Vector u = Normalize(Cross(helper, mu));
	const Vector v = Cross(mu, u);
	return u * (s * cosf(phi)) + v * (s * sinf(phi)) + mu * z;
}

float PathGuidingCache::VmfMeanCos(const float kappa) {
	// A(k) = coth(k) - 1/k, the vMF mean resultant length; ~k/3 near 0.
	if (kappa < 1e-3f)
		return kappa / 3.f;
	const float e = expf(-2.f * kappa);
	return (1.f + e) / (1.f - e) - 1.f / kappa;
}

bool PathGuidingCache::CosineSample(const Vector &n, float u0, float u1,
		Vector *sampledDir, float *pdfW) {
	const float cosTheta = sqrtf(Max(0.f, 1.f - u0));
	if (!(cosTheta > 0.f))
		return false;
	const float sinTheta = sqrtf(Max(0.f, 1.f - cosTheta * cosTheta));
	const float phi = 2.f * M_PI * u1;
	const Vector helper = (fabsf(n.z) < .999f) ? Vector(0.f, 0.f, 1.f) :
			Vector(0.f, 1.f, 0.f);
	const Vector u = Normalize(Cross(helper, n));
	const Vector v = Cross(n, u);
	*sampledDir = u * (sinTheta * cosf(phi)) + v * (sinTheta * sinf(phi)) + n * cosTheta;
	*pdfW = cosTheta * INV_PI;
	return true;
}

u_int PathGuidingCache::Descend(const vector<TreeNode> &nodes,
		u_int root, const Point &p) {
	u_int ni = root;
	for (;;) {
		const TreeNode &nd = nodes[ni];
		if (nd.axis == ~0u)
			return nd.leaf;
		const float pc = (nd.axis == 0) ? p.x : ((nd.axis == 1) ? p.y : p.z);
		ni = nd.child[(pc < nd.split) ? 0 : 1];
	}
}

const PathGuidingCache::ReadLeaf *PathGuidingCache::ReadLeafAt(const Point &p) const {
	const ReadTree *rt = readTree.load(std::memory_order_acquire);
	if (!rt || rt->nodes.empty())
		return nullptr;
	return &rt->leaves[Descend(rt->nodes, rt->root, p)];
}

// Component selection weights for a query. Surfaces approximate the
// f*Li product at lobe granularity: each component's weight is scaled by
// its expected cosine under the lobe, A(kappa)*(mu.n), clamped positive.
// Identical math runs in Sample() and Pdf() so the drawn density and its
// evaluation always agree (one-sample MIS stays exact).
float PathGuidingCache::CompWeights(const ReadLeaf &leaf,
		const Vector &n, const bool isotropic, float *s) {
	float sSum = 0.f;
	for (u_int k = 0; k < leaf.nComp; ++k) {
		float sk = leaf.w[k];
		if (!isotropic) {
			// kappa ~ 0 doubles as the cosine lobe on surfaces
			// (E[cos] = 2/3); on volumes it is the uniform sphere.
			if (leaf.kappa[k] < 1e-3f) {
				sk *= 2.f / 3.f;
			} else {
				const float d = leaf.mu[k][0] * n.x + leaf.mu[k][1] * n.y +
						leaf.mu[k][2] * n.z;
				sk *= Max(0.f, VmfMeanCos(leaf.kappa[k]) * d);
			}
		}
		s[k] = sk;
		sSum += sk;
	}
	return sSum;
}

//------------------------------------------------------------------------------
// Construction / teardown
//------------------------------------------------------------------------------

PathGuidingCache::PathGuidingCache(const Point &min, float size,
		const Settings &s) :
		cubeMin(min), cubeSize(size), invCubeSize(1.f / size),
		warmup((u_int)Max((float)(s.warmupRecords ? s.warmupRecords :
				(u_int)GuideWarmupDefault()), (float)WARMUP_RECORDS)),
		swapRecords(s.swapRecords ? s.swapRecords : GuideSwapRecordsDefault()),
		splitFrac(s.splitFrac > 0.f ? s.splitFrac : GuideSplitFracDefault()),
		maxDepth(s.maxDepth ? Min(s.maxDepth, TREE_MAX_DEPTH) :
				GuideMaxDepthDefault()),
		maxLeaves(s.maxLeaves ? Min(s.maxLeaves, 65536u) :
				GuideMaxLeavesDefault()),
		maxComponents(Clamp(s.maxComponents ? s.maxComponents : VMF_K,
				1u, VMF_K)),
		debug(s.debug) {
	// Both trees start as a single cold root leaf.
	WriteTree *wt = new WriteTree();
	TreeNode root;
	root.child[0] = root.child[1] = ~0u;
	root.axis = ~0u;
	root.split = 0.f;
	root.leaf = 0;
	wt->nodes.push_back(root);
	wt->leaves.reset(new LeafStats[1]);
	wt->leaves[0].Clear();
	wt->leafCount = 1;
	writeTree.store(wt, std::memory_order_relaxed);

	ReadTree *rt = new ReadTree();
	rt->nodes.push_back(root);
	rt->leaves.resize(1);
	readTree.store(rt, std::memory_order_relaxed);
}

PathGuidingCache::Settings PathGuidingCache::SettingsFromProperties(
		const Properties &cfg) {
	// Property wins when defined; an unset field stays 0/empty so the
	// ctor applies the LUX_PG_* env fallback and then the default.
	Settings s;
	s.warmupRecords = cfg.IsDefined("path.guiding.warmup") ?
			(u_int)Max(0, cfg.Get("path.guiding.warmup").Get<int>()) : 0;
	s.swapRecords = cfg.IsDefined("path.guiding.swaprecords") ?
			(u_int)Max(1000, cfg.Get("path.guiding.swaprecords").Get<int>()) : 0;
	s.splitFrac = cfg.IsDefined("path.guiding.split") ?
			Clamp(cfg.Get("path.guiding.split").Get<float>(), 1e-6f, .5f) : 0.f;
	s.maxDepth = cfg.IsDefined("path.guiding.maxdepth") ?
			(u_int)Clamp(cfg.Get("path.guiding.maxdepth").Get<int>(), 1, 12) : 0;
	s.maxLeaves = cfg.IsDefined("path.guiding.maxleaves") ?
			(u_int)Clamp(cfg.Get("path.guiding.maxleaves").Get<int>(), 16, 65536) : 0;
	s.maxComponents = cfg.IsDefined("path.guiding.components") ?
			(u_int)Clamp(cfg.Get("path.guiding.components").Get<int>(), 1,
				(int)VMF_K) : 0;
	s.debug = cfg.IsDefined("path.guiding.debug") &&
			cfg.Get("path.guiding.debug").Get<bool>();
	return s;
}

PathGuidingCache::~PathGuidingCache() {
	delete readTree.load(std::memory_order_relaxed);
	delete writeTree.load(std::memory_order_relaxed);
	for (auto *t : retiredRead)
		delete t;
	for (auto *t : retiredWrite)
		delete t;
}

//------------------------------------------------------------------------------
// Recording (hot path, lock-free)
//------------------------------------------------------------------------------

void PathGuidingCache::Record(const Point &p, const Vector &wi, float flux) const {
	// Swap cadence counts record *attempts*, not kept records: in dim
	// scenes most arrivals carry ~0 local value and are dropped below, so
	// counting only keeps would stall rounds forever.
	writeRecords.fetch_add(1uLL, std::memory_order_relaxed);
	if (writeRecords.load(std::memory_order_relaxed) > swapRecords)
		SwapTrees();
	// Rebuild window: drop the record rather than descend a mutating
	// tree. Bounded and rare (~ms per ~1M records); training tolerates it.
	if (swapFlag.load(std::memory_order_relaxed))
		return;
	if (!isfinite(flux))
		return;
	const WriteTree *t = writeTree.load(std::memory_order_acquire);
	if (!t || t->nodes.empty())
		return;
	LeafStats &ls = t->leaves[Descend(t->nodes, t->root, p)];
	// count/centroid track *visitation* (every arrival, even dark ones):
	// a leaf visited often IS well-sampled, and splits must see where
	// paths actually go. Signal-bearing records additionally bump nz,
	// the directional moments and the flux.
	AtomicAdd(ls.count, 1.f);
	AtomicAdd(ls.posX, p.x);
	AtomicAdd(ls.posY, p.y);
	AtomicAdd(ls.posZ, p.z);
	if (!(flux > 0.f))
		return;
	flux = Min(flux, RECORD_CLAMP);
	const u_int bin = DirBin(wi);
	static const char *kDumpRec = getenv("LUX_PG_DUMPREC");
	if (kDumpRec && flux > 0.f) {
		static std::atomic<u_int> dumped(0);
		if (dumped.fetch_add(1, std::memory_order_relaxed) < 20000) {
			static FILE *rf = fopen(kDumpRec, "w");
			if (rf) {
				fprintf(rf, "%.3f %.3f %.3f %.3f %.3f %.3f %.3f\n",
						p.x, p.y, p.z, wi.x, wi.y, wi.z, flux);
			}
		}
	}
	AtomicAdd(ls.bins[bin], flux);
	AtomicAdd(ls.binsSq[bin], flux * flux);
	AtomicAdd(ls.binCnt[bin], 1.f);
	AtomicAdd(ls.nz, 1.f);
	AtomicAdd(ls.total, flux);
	AtomicAdd(ls.dirX, wi.x * flux);
	AtomicAdd(ls.dirY, wi.y * flux);
	AtomicAdd(ls.dirZ, wi.z * flux);
}

//------------------------------------------------------------------------------
// Round swap: fit the read tree, refine the write tree, publish both
//------------------------------------------------------------------------------

void PathGuidingCache::ForceSwap() const {
	SwapTrees(true);
}

void PathGuidingCache::SwapTrees(const bool forced) const {
	if (frozen)
		return;
	bool expected = false;
	if (!swapFlag.compare_exchange_strong(expected, true,
			std::memory_order_relaxed, std::memory_order_relaxed))
		return;
	// Spurious wake-up: another thread may win the flag right after a
	// completed swap reset the counter; building from a near-empty round
	// would publish an empty field for a whole round. Explicit ForceSwap
	// calls (drain cadence, tests) bypass the check.
	if (!forced &&
			writeRecords.load(std::memory_order_relaxed) < swapRecords / 2) {
		swapFlag.store(false, std::memory_order_relaxed);
		return;
	}

	WriteTree *oldW = writeTree.load(std::memory_order_relaxed);
	ReadTree *oldR = readTree.load(std::memory_order_relaxed);

	ReadTree *newR = BuildReadTree(*oldW);
	WriteTree *newW = BuildWriteTree(*oldW);

	const bool kDebug = debug || (getenv("LUX_PG_DEBUG") != nullptr);
	if (kDebug) {
		double cSum = 0.0, tSum = 0.0;
		u_int warm = 0;
		for (u_int i = 0; i < newR->leaves.size(); ++i) {
			cSum += newR->leaves[i].count;
			tSum += newR->leaves[i].total;
			if (newR->leaves[i].count >= warmup)
				++warm;
		}
		// stderr: pysuperluxcore installs no SLG log handler
		fprintf(stderr, "[PG] swap: leaves=%u kept=%llu attempts=%llu flux=%g warm=%u borrowed=%u\n",
				newW->leafCount, (unsigned long long)cSum,
				writeRecords.load(std::memory_order_relaxed), tSum, warm,
				newR->borrowedLeaves);
	}
	static const char *kDumpBins = getenv("LUX_PG_DUMPBINS");
	if (kDumpBins) {
		FILE *bf = fopen(kDumpBins, "a");
		if (bf) {
			// Per-leaf raw histogram dump: centroid position, counts and
			// the top bins (bin -> direction, mass). For diagnosing what
			// the write tree actually learned at a location.
			for (u_int i = 0; i < oldW->leafCount; ++i) {
				const LeafStats &ls = oldW->leaves[i];
				const float cnt = ls.count.load(std::memory_order_relaxed);
				const float nz = ls.nz.load(std::memory_order_relaxed);
				if (cnt <= 0.f)
					continue;
				const Point c(ls.posX.load(std::memory_order_relaxed) / cnt,
						ls.posY.load(std::memory_order_relaxed) / cnt,
						ls.posZ.load(std::memory_order_relaxed) / cnt);
				u_int top[6] = {0, 0, 0, 0, 0, 0};
				float topV[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
				for (u_int b = 0; b < DIR_BINS; ++b) {
					const float v = ls.bins[b].load(std::memory_order_relaxed);
					for (u_int s = 0; s < 6; ++s) {
						if (v > topV[s]) {
							for (u_int t = 5; t > s; --t) {
								topV[t] = topV[t - 1];
								top[t] = top[t - 1];
							}
							topV[s] = v;
							top[s] = b;
							break;
						}
					}
				}
				fprintf(bf, "leaf=%u c=(%.2f,%.2f,%.2f) cnt=%.0f nz=%.0f |",
						i, c.x, c.y, c.z, cnt, nz);
				for (u_int s = 0; s < 6 && topV[s] > 0.f; ++s) {
					const Vector d = BinDir(top[s], .5f, .5f);
					fprintf(bf, " b%u(%.2f,%.2f,%.2f)=%.1f",
							top[s], d.x, d.y, d.z, topV[s]);
				}
				// Full histogram appended after " ALL:" when wanted for
				// offline EM reproduction (128 floats).
				fprintf(bf, " ALL:");
				for (u_int b = 0; b < DIR_BINS; ++b)
					fprintf(bf, " %.3g",
							ls.bins[b].load(std::memory_order_relaxed));
				fprintf(bf, "\n");
			}
			fclose(bf);
		}
	}

	readTree.store(newR, std::memory_order_release);
	writeTree.store(newW, std::memory_order_release);

	// Epoch-style reclamation: keep the newest retired generations so
	// in-flight lookups/records keep valid memory; anything older cannot
	// be reached (a query only uses the pointer it loaded at entry).
	retiredRead.push_back(oldR);
	retiredWrite.push_back(oldW);
	while (retiredRead.size() > 4) {
		delete retiredRead.front();
		retiredRead.erase(retiredRead.begin());
	}
	while (retiredWrite.size() > 4) {
		delete retiredWrite.front();
		retiredWrite.erase(retiredWrite.begin());
	}

	writeRecords.store(0uLL, std::memory_order_relaxed);
	swapFlag.store(false, std::memory_order_relaxed);
}

// Fit one leaf's vMF mixture from a directional histogram by weighted
// EM over the 128 bin centroids. The per-bin target is the
// variance-aware sqrt(E[x^2]) (Rath 2020), not the raw mean.
//
// P5 adaptive complexity: the directional-lobe count is chosen per leaf
// by BIC (weighted log-likelihood - 1/2 * params * log N over the
// signal-record count) across K_dir in {0..VMF_K-1}. A broad unimodal
// field no longer pays for phantom extra lobes (overfit noise modes
// were a measurable regression), while genuinely multimodal leaves
// still earn the full K.
void PathGuidingCache::FitLeaf(const AggStats &a, ReadLeaf &out) const {
	out.nComp = 0;
	out.peak = 0.f;
	const bool noVA = GuideNoVA();

	// Per-bin target weights q_i over the sphere. The variance-aware
	// target (Rath 2020) is the UNCONDITIONAL second moment sqrt(E[x^2])
	// = sqrt(sumSq / N); conditioning on a hit (sumSq/hitCnt) erases the
	// hit-frequency information: a cluster bin with 300 medium hits then
	// ranks below a bin with one lucky hit, which flattened real fields
	// into scattered lobes. N is uniform across bins so it factors out.
	float q[DIR_BINS];
	float qSum = 0.f;
	Vector centroid[DIR_BINS];
	for (u_int i = 0; i < DIR_BINS; ++i) {
		float t;
		if (noVA) {
			t = a.bins[i];
		} else {
			const float m2 = a.binsSq[i];
			t = (m2 > 0.f) ? sqrtf(m2) : 0.f;
		}
		q[i] = t;
		qSum += t;
		centroid[i] = BinDir(i, .5f, .5f);
	}
	if (!(qSum > 0.f))
		return;
	const float invQ = 1.f / qSum;
	for (u_int i = 0; i < DIR_BINS; ++i)
		q[i] *= invQ;

	const bool hasUni = !GuideNoUni();

	// --- Seed order for the directional lobes: argmax first, then the
	// weighted bins least covered by the already-seeded lobes. The same
	// greedy order applies to every candidate K, so it is computed once.
	Vector seedMu[VMF_K];
	// path.guiding.components caps the TOTAL lobe count (uni included):
	// the BIC search only runs up to that budget.
	const u_int maxDir = Min(hasUni ? VMF_K - 1 : VMF_K,
			maxComponents - (hasUni ? Min(maxComponents, 1u) : 0u));
	u_int nSeed = 0;
	for (u_int k = 0; k < maxDir; ++k) {
		u_int best = 0;
		float bestScore = -1.f;
		for (u_int i = 0; i < DIR_BINS; ++i) {
			if (q[i] <= 0.f)
				continue;
			float cov = 0.f;
			for (u_int j = 0; j < nSeed; ++j)
				cov = Max(cov, VmfPdf(Dot(centroid[i], seedMu[j]), 4.f));
			const float score = q[i] * Max(0.f, 1.f - cov / VmfPdf(1.f, 4.f));
			if (score > bestScore) {
				bestScore = score;
				best = i;
			}
		}
		if (!(bestScore > 0.f))
			break;
		seedMu[nSeed++] = centroid[best];
	}
	if (nSeed == 0 && !hasUni)
		return;

	// --- Candidate model: uniform lobe (if enabled) + kDir seeded
	// directional lobes, EM-refined, pruned, evaluated.
	struct Model {
		u_int nComp;
		float w[VMF_K], kappa[VMF_K];
		Vector mu[VMF_K];
		float logLik;
	};
	auto fitK = [&](const u_int kDir, Model &m) {
		float w[VMF_K], kappa[VMF_K];
		Vector mu[VMF_K];
		u_int kUsed = 0;
		if (hasUni) {
			mu[0] = Vector(0.f, 0.f, 1.f);
			w[0] = .05f;
			kappa[0] = 0.f;
			kUsed = 1;
		}
		for (u_int k = 0; k < kDir; ++k) {
			mu[kUsed] = seedMu[k];
			w[kUsed] = .9f;
			kappa[kUsed] = 4.f;
			++kUsed;
		}

		float resp[VMF_K];
		for (u_int it = 0; it < 16u && kUsed; ++it) {
			float newW[VMF_K] = {};
			Vector newS[VMF_K];
			for (u_int i = 0; i < DIR_BINS; ++i) {
				if (q[i] <= 0.f)
					continue;
				float rSum = 0.f;
				for (u_int k = 0; k < kUsed; ++k) {
					resp[k] = w[k] * VmfPdf(Dot(centroid[i], mu[k]), kappa[k]);
					rSum += resp[k];
				}
				if (!(rSum > 0.f))
					continue;
				const float invR = q[i] / rSum;
				for (u_int k = 0; k < kUsed; ++k) {
					const float r = resp[k] * invR;
					newW[k] += r;
					newS[k] += centroid[i] * r;
				}
			}
			bool alive = false;
			for (u_int k = 0; k < kUsed; ++k) {
				w[k] = newW[k];
				if (hasUni && k == 0)
					continue;  // uniform lobe: weight only, mu/kappa fixed
				const float len = newS[k].Length();
				if (len > 0.f && w[k] > 0.f) {
					mu[k] = newS[k] * (1.f / len);
					const float r = Min(len / w[k], .9999f);
					// 128 bins cannot justify arbitrarily sharp lobes:
					// over-tight kappa leaves near-zero density holes
					// inside the empirical support -> huge sample weights.
					kappa[k] = Min(r * (3.f - r * r) / (1.f - r * r),
							GuideKappaCap());
					alive = true;
				}
			}
			if (!alive)
				break;
		}

		// Prune negligible lobes (the uniform comp is kept at a floor
		// weight: it is the coverage guarantee), renormalize.
		if (hasUni)
			w[0] = Max(w[0], .02f);
		float wSum = 0.f;
		for (u_int k = 0; k < kUsed; ++k)
			wSum += ((hasUni && k == 0) || (w[k] > .02f)) ? w[k] : 0.f;
		if (!(wSum > 0.f)) {
			m.nComp = 0;
			m.logLik = -1e30f;
			return;
		}
		const float invW = 1.f / wSum;
		u_int dst = 0;
		for (u_int k = 0; k < kUsed; ++k) {
			if (!(hasUni && k == 0) && !(w[k] > .02f))
				continue;
			m.w[dst] = w[k] * invW;
			m.mu[dst] = mu[k];
			m.kappa[dst] = kappa[k];
			++dst;
		}
		m.nComp = dst;

		// Weighted log-likelihood of the fitted model over the raw bin
		// masses (the EM fit worked on the normalized target; BIC needs
		// the evidence scale back for a fair penalty comparison).
		m.logLik = 0.f;
		for (u_int i = 0; i < DIR_BINS; ++i) {
			if (q[i] <= 0.f)
				continue;
			float mix = 0.f;
			for (u_int k = 0; k < dst; ++k)
				mix += m.w[k] * VmfPdf(Dot(centroid[i], m.mu[k]), m.kappa[k]);
			m.logLik += q[i] * qSum * logf(Max(mix, 1e-30f));
		}
	};

	// BIC model selection: p counts free parameters (uni weight + per
	// directional lobe w, mu 2-dof, kappa); N is the signal-record count
	// - more evidence justifies more lobes, sparse leaves stay simple.
	const float logN = logf(Max(a.nz, 1.f));
	Model best;
	best.nComp = 0;
	best.logLik = -1e30f;
	float bestBic = -1e30f;
	for (u_int kDir = 0; kDir <= nSeed; ++kDir) {
		Model m;
		fitK(kDir, m);
		const u_int dirComps = m.nComp - (hasUni ? Min(m.nComp, 1u) : 0u);
		const float p = dirComps * 4.f + (hasUni ? 1.f : 0.f);
		const float bic = m.logLik - .5f * p * logN;
		if (bic > bestBic) {
			bestBic = bic;
			best = m;
		}
	}
	if (best.nComp == 0) {
		// Degenerate fit: a single uniform component keeps the leaf warm
		// (kappa ~ 0 IS the uniform sphere distribution).
		out.nComp = 1;
		out.w[0] = 1.f;
		out.mu[0][0] = 0.f; out.mu[0][1] = 0.f; out.mu[0][2] = 1.f;
		out.kappa[0] = 0.f;
		return;
	}
	out.nComp = best.nComp;
	for (u_int k = 0; k < best.nComp; ++k) {
		out.w[k] = best.w[k];
		out.mu[k][0] = best.mu[k].x;
		out.mu[k][1] = best.mu[k].y;
		out.mu[k][2] = best.mu[k].z;
		out.kappa[k] = best.kappa[k];
	}

	// Informativeness of the fit: 4pi * (integral of p^2 over the sphere)
	// - 1, evaluated on the equal-area bins. ~0 for a uniform field,
	// ~kappa for a tight lobe. MixWeight gates on it: a leaf whose field
	// is no more concentrated than the fallback gains nothing from
	// guiding - proposal noise would only hurt.
	float m2 = 0.f;
	for (u_int i = 0; i < DIR_BINS; ++i) {
		const Vector d = BinDir(i, .5f, .5f);
		float pd = 0.f;
		for (u_int k = 0; k < out.nComp; ++k)
			pd += out.w[k] * VmfPdf(d.x * out.mu[k][0] +
					d.y * out.mu[k][1] + d.z * out.mu[k][2],
					out.kappa[k]);
		m2 += pd * pd;
	}
	out.peak = m2 * BinSolidAngle() * 4.f * M_PI - 1.f;
}

PathGuidingCache::ReadTree *PathGuidingCache::BuildReadTree(const WriteTree &wt) const {
	ReadTree *rt = new ReadTree();
	rt->nodes = wt.nodes;  // the read field mirrors the topology that
	                       // collected the stats
	rt->root = wt.root;
	rt->leaves.resize(wt.leafCount);

	// Hierarchical fallback (P5): a leaf without WARMUP of its own
	// records borrows the fit of its nearest ancestor whose SUBTREE is
	// warm. The ancestor's histogram covers a larger region - a coarser
	// field, still a valid positive pdf, and strictly better than the
	// previous all-or-nothing cold gate (OpenPGL-style fallback).
	//
	// Nodes are emitted post-order (children index below their parent),
	// so a single forward pass aggregates every subtree and records
	// parent links.
	vector<AggStats> agg(wt.nodes.size());
	vector<u_int> parent(wt.nodes.size(), ~0u);
	for (u_int i = 0; i < wt.nodes.size(); ++i) {
		const TreeNode &nd = wt.nodes[i];
		if (nd.axis == ~0u)
			agg[i].Add(wt.leaves[nd.leaf]);
		else {
			agg[i].Add(agg[nd.child[0]]);
			agg[i].Add(agg[nd.child[1]]);
			parent[nd.child[0]] = parent[nd.child[1]] = i;
		}
	}

	for (u_int i = 0; i < wt.nodes.size(); ++i) {
		const TreeNode &nd = wt.nodes[i];
		if (nd.axis != ~0u)
			continue;
		const AggStats &a = agg[i];
		ReadLeaf &rl = rt->leaves[nd.leaf];
		rl.total = a.total;
		rl.count = a.count;
		rl.nz = a.nz;
		// Eligibility keys on visitation, not signal count: a leaf visited
		// by many paths but lit through a narrow bottleneck still learns
		// a usable lobe from its few nonzero records (the zeros just feed
		// the uniform component). Signal-only gating starved exactly the
		// dim receiver surfaces guiding exists for.
		if (a.count >= warmup) {
			FitLeaf(a, rl);
			continue;
		}
		// Cold leaf: ascend to the nearest warm ancestor and fit its
		// aggregate. The borrowed model is emitted with a DAMPED
		// synthetic count - the consumers' count-driven warmup gate and
		// MixWeight then apply a proportionally lower trust with no
		// contract change (still >= warmup so both backends accept it).
		for (u_int p = parent[i]; p != ~0u; p = parent[p]) {
			if (agg[p].count < warmup)
				continue;
			FitLeaf(agg[p], rl);
			if (rl.nComp > 0) {
				rl.count = Clamp(a.count + .25f * agg[p].count,
						(float)warmup, 4.f * warmup);
				rl.nz = a.nz;
				++rt->borrowedLeaves;
			}
			break;
		}
	}
	return rt;
}

PathGuidingCache::WriteTree *PathGuidingCache::BuildWriteTree(const WriteTree &wt) const {
	const double globalFlux = [&]() {
		double f = 0.0;
		for (u_int i = 0; i < wt.leafCount; ++i)
			f += wt.leaves[i].total.load(std::memory_order_relaxed);
		return f;
	}();
	const double globalCount = [&]() {
		double c = 0.0;
		for (u_int i = 0; i < wt.leafCount; ++i)
			c += wt.leaves[i].count.load(std::memory_order_relaxed);
		return c;
	}();
	WriteTree *nw = new WriteTree();
	// Leaf count is known only after the topology is emitted; leaves
	// allocate at the end. Track emissions against the budget directly.
	vector<TreeNode> &nodes = nw->nodes;

	auto longestAxis = [](const Point &rmin, const Point &rmax) {
		const float ex = rmax.x - rmin.x, ey = rmax.y - rmin.y,
				ez = rmax.z - rmin.z;
		return (ex >= ey && ex >= ez) ? 0u : ((ey >= ez) ? 1u : 2u);
	};
	auto axisVal = [](const Point &p, u_int axis) {
		return (axis == 0) ? p.x : ((axis == 1) ? p.y : p.z);
	};
	auto clampSplit = [](float s, float lo, float hi) {
		const float len = hi - lo;
		return Min(Max(s, lo + .1f * len), hi - .1f * len);
	};

	u_int leafCount = 0;
	// Persistent statistics (Muller-style lifetime accumulation): each
	// emitted leaf records the source leaf it inherits from and the
	// share. Unsplit leaves carry their stats over unchanged; children
	// of a split leaf inherit the parent's histogram diluted by their
	// share, so a fresh split never regresses the field to cold.
	vector<u_int> leafSrc;
	vector<float> leafShare;
	auto emitLeaf = [&](u_int src, float share) -> u_int {
		TreeNode nd;
		nd.child[0] = nd.child[1] = ~0u;
		nd.axis = ~0u;
		nd.split = 0.f;
		nd.leaf = leafCount++;
		leafSrc.push_back(src);
		leafShare.push_back(share);
		nodes.push_back(nd);
		return (u_int)nodes.size() - 1;
	};

	// Speculative deeper chain for the child that contains the centroid:
	// a hot leaf reaches depth in one round instead of one level/round.
	function<u_int(const Point &, const Point &, u_int, double, double, const Point &,
			u_int, float)>
			specChain = [&](const Point &rmin, const Point &rmax, u_int depth,
			double cntEst, double fluxEst, const Point &centroid,
			u_int src, float share) -> u_int {
		const bool hot = (cntEst > splitFrac * globalCount) ||
				(fluxEst > splitFrac * globalFlux);
		if (!hot || depth >= maxDepth || leafCount + 3u > maxLeaves)
			return emitLeaf(src, share);
		const u_int axis = longestAxis(rmin, rmax);
		const float lo = axisVal(rmin, axis), hi = axisVal(rmax, axis);
		const float split = clampSplit(axisVal(centroid, axis), lo, hi);

		Point cmin = rmin, cmax = rmax;
		const bool inLow = axisVal(centroid, axis) < split;
		if (inLow)
			cmax = Point(axis == 0 ? split : rmax.x,
					axis == 1 ? split : rmax.y,
					axis == 2 ? split : rmax.z);
		else
			cmin = Point(axis == 0 ? split : rmin.x,
					axis == 1 ? split : rmin.y,
					axis == 2 ? split : rmin.z);

		const u_int deeper = specChain(cmin, cmax, depth + 1,
				cntEst * .5, fluxEst * .5, centroid, src, share * .5f);
		const u_int sib = emitLeaf(src, share * .5f);
		TreeNode nd;
		nd.axis = axis;
		nd.split = split;
		nd.leaf = ~0u;
		if (inLow) {
			nd.child[0] = deeper;
			nd.child[1] = sib;
		} else {
			nd.child[0] = sib;
			nd.child[1] = deeper;
		}
		nodes.push_back(nd);
		return (u_int)nodes.size() - 1;
	};

	function<u_int(u_int, const Point &, const Point &, u_int)> build =
			[&](u_int oldNi, const Point &rmin, const Point &rmax,
			u_int depth) -> u_int {
		const TreeNode &ond = wt.nodes[oldNi];
		if (ond.axis != ~0u) {
			// Inner node: keep, recurse into children with split bounds
			Point lmax = rmax, rmin2 = rmin;
			if (ond.axis == 0)      { lmax.x = ond.split; rmin2.x = ond.split; }
			else if (ond.axis == 1) { lmax.y = ond.split; rmin2.y = ond.split; }
			else                    { lmax.z = ond.split; rmin2.z = ond.split; }
			const u_int c0 = build(ond.child[0], rmin, lmax, depth + 1);
			const u_int c1 = build(ond.child[1], rmin2, rmax, depth + 1);
			TreeNode nd;
			nd.axis = ond.axis;
			nd.split = ond.split;
			nd.leaf = ~0u;
			nd.child[0] = c0;
			nd.child[1] = c1;
			nodes.push_back(nd);
			return (u_int)nodes.size() - 1;
		}

		// Leaf node: refine when it holds a large share of the
		// accumulated records (Muller-style sample-density refinement:
		// resolution follows where paths actually go, including dim
		// surfaces that a flux-only criterion would starve) or of the
		// flux. Stats persist across rounds now, so shares are lifetime.
		const LeafStats &ls = wt.leaves[ond.leaf];
		const float cnt = ls.count.load(std::memory_order_relaxed);
		const float tot = ls.total.load(std::memory_order_relaxed);
		const bool doSplit = (cnt > splitFrac * globalCount) ||
				(tot > splitFrac * globalFlux);
		if (!(cnt >= warmup) || !doSplit ||
				depth >= maxDepth || leafCount + 3u > maxLeaves)
			return emitLeaf(ond.leaf, 1.f);

		// Split at the arrival centroid on the longest axis (clamped to
		// the middle 80% so both children keep a usable region).
		const Point centroid = (cnt > 0.f) ?
				Point(ls.posX.load(std::memory_order_relaxed) / cnt,
						ls.posY.load(std::memory_order_relaxed) / cnt,
						ls.posZ.load(std::memory_order_relaxed) / cnt) :
				Point((rmin.x + rmax.x) * .5f, (rmin.y + rmax.y) * .5f,
						(rmin.z + rmax.z) * .5f);
		const u_int axis = longestAxis(rmin, rmax);
		const float lo = axisVal(rmin, axis), hi = axisVal(rmax, axis);
		const float split = clampSplit(axisVal(centroid, axis), lo, hi);

		Point cmin = rmin, cmax = rmax;
		const bool inLow = axisVal(centroid, axis) < split;
		if (inLow)
			cmax = Point(axis == 0 ? split : rmax.x,
					axis == 1 ? split : rmax.y,
					axis == 2 ? split : rmax.z);
		else
			cmin = Point(axis == 0 ? split : rmin.x,
					axis == 1 ? split : rmin.y,
					axis == 2 ? split : rmin.z);
		const u_int deeper = specChain(cmin, cmax, depth + 1,
				(double)cnt * .5, (double)tot * .5, centroid, ond.leaf, .5f);
		const u_int sib = emitLeaf(ond.leaf, .5f);
		TreeNode nd;
		nd.axis = axis;
		nd.split = split;
		nd.leaf = ~0u;
		if (inLow) {
			nd.child[0] = deeper;
			nd.child[1] = sib;
		} else {
			nd.child[0] = sib;
			nd.child[1] = deeper;
		}
		nodes.push_back(nd);
		return (u_int)nodes.size() - 1;
	};

	const Point rmax(cubeMin.x + cubeSize, cubeMin.y + cubeSize,
			cubeMin.z + cubeSize);
	nw->root = build(wt.root, cubeMin, rmax, 0u);

	nw->leafCount = leafCount;
	nw->leaves.reset(new LeafStats[leafCount]);
	// Directional inheritance cap: a split child gets at most this many
	// signal-records worth of the parent histogram. Enough to seed EM
	// (argmax init finds the parent's lobe directions) but the child's
	// own data dominates within a round or two - the parent field was
	// integrated over the whole parent region and is spatially wrong
	// for the child, so inheriting it at full share smears peaked
	// children toward isotropic forever.
	static const float kInheritCap = []() {
		const char *e = getenv("LUX_PG_INHERIT");
		return e ? (float)atof(e) : 48.f;
	}();
	for (u_int i = 0; i < leafCount; ++i) {
		const u_int src = leafSrc[i];
		if (src == ~0u) {
			nw->leaves[i].Clear();
			continue;
		}
		const float fSpat = leafShare[i];
		float fDir = fSpat;
		if (fSpat < 1.f) {
			const float srcNz = wt.leaves[src].nz.load(
					std::memory_order_relaxed);
			if (srcNz > 0.f)
				fDir = fSpat * Min(1.f, kInheritCap / (fSpat * srcNz));
			else
				fDir = 0.f;
		}
		nw->leaves[i].CopyFrom(wt.leaves[src], fSpat, fDir);
	}
	return nw;
}

//------------------------------------------------------------------------------
// Queries
//------------------------------------------------------------------------------

bool PathGuidingCache::CanGuide(const Point &p) const {
	const ReadLeaf *leaf = ReadLeafAt(p);
	return leaf && leaf->nComp > 0 && leaf->count >= warmup;
}

float PathGuidingCache::ReadTotal(const Point &p) const {
	const ReadLeaf *leaf = ReadLeafAt(p);
	return leaf ? leaf->total : 0.f;
}

float PathGuidingCache::ReadCount(const Point &p) const {
	const ReadLeaf *leaf = ReadLeafAt(p);
	return leaf ? leaf->count : 0.f;
}

float PathGuidingCache::ReadPeak(const Point &p) const {
	const ReadLeaf *leaf = ReadLeafAt(p);
	return leaf ? leaf->peak : 0.f;
}

float PathGuidingCache::IncidentEstimate(const Point &p,
		const Vector &dir, const float floorFrac) const {
	const ReadLeaf *leaf = ReadLeafAt(p);
	if (!leaf || leaf->nComp == 0 || leaf->total <= 0.f)
		return 0.f;
	// Raw fitted mixture (the EM uniform component stays a sphere
	// uniform here - unlike LobePdf's surface cosine reading - so the
	// estimate keeps positive support wherever the BSDF is nonzero,
	// which RIS requires of its target function).
	float mix = 0.f;
	for (u_int k = 0; k < leaf->nComp; ++k)
		mix += leaf->w[k] * VmfPdf(dir.x * leaf->mu[k][0] +
				dir.y * leaf->mu[k][1] + dir.z * leaf->mu[k][2],
				leaf->kappa[k]);
	// floorFrac of the leaf's uniform level keeps positive support for
	// resampling targets (an unfitted/zero-mix direction must still be
	// reachable or the RIS estimator loses coverage).
	if (floorFrac > 0.f)
		mix = (1.f - floorFrac) * mix + floorFrac * .25f * INV_PI;
	return leaf->total * mix;
}

bool PathGuidingCache::Sample(const Point &p, const Normal &n,
		float uBin, float uDir0, float uDir1,
		Vector *sampledDir, float *pdfW, const bool isotropic) const {
	const float floorW = isotropic ? FLOOR_W_VOLUME : FLOOR_W_SURFACE;
	const Vector nn(n.x, n.y, n.z);
	const ReadLeaf *leaf = ReadLeafAt(p);
	if (!leaf || leaf->nComp == 0 || leaf->count < warmup) {
		// Cold leaf: pure-floor fallback (same density reported by Pdf).
		if (isotropic) {
			*sampledDir = UniformSampleSphere(uDir0, uDir1);
			*pdfW = .25f * INV_PI;
			return true;
		}
		return CosineSample(nn, uDir0, uDir1, sampledDir, pdfW);
	}

	float s[VMF_K];
	const float sSum = CompWeights(*leaf, nn, isotropic, s);
	if (!(sSum > 0.f)) {
		// All lobes below the horizon (surface) or degenerate: floor only.
		if (isotropic) {
			*sampledDir = UniformSampleSphere(uDir0, uDir1);
			*pdfW = .25f * INV_PI;
			return true;
		}
		return CosineSample(nn, uDir0, uDir1, sampledDir, pdfW);
	}

	// Usable-field fraction for this normal: sSum/wSum is the share of the
	// mixture mass that is not grazing/dead. Whatever is unusable folds back
	// into the cosine floor so a horizon-hugging field degrades to plain
	// cosine sampling instead of wasting proposal mass on f*cos ~ 0 lobes.
	float wSum = 0.f;
	for (u_int k = 0; k < leaf->nComp; ++k)
		wSum += leaf->w[k];
	const float usable = (wSum > 0.f) ? Clamp(sSum / wSum, 0.f, 1.f) : 0.f;
	const float effFloorW = floorW + (1.f - floorW) * (1.f - usable);

	if (uBin < effFloorW) {
		// Floor component: cosine lobe for surfaces, uniform for volumes.
		if (isotropic) {
			*sampledDir = UniformSampleSphere(uDir0, uDir1);
		} else {
			float fp;
			CosineSample(nn, uDir0, uDir1, sampledDir, &fp);
		}
	} else {
		// Pick a mixture component proportional to s_k
		const float u = (uBin - effFloorW) / (1.f - effFloorW);
		float pick = u * sSum;
		u_int k = 0;
		for (; k + 1 < leaf->nComp; ++k) {
			pick -= s[k];
			if (pick <= 0.f)
				break;
		}
		if (!(s[k] > 0.f))
			k = 0;
		if (leaf->kappa[k] < 1e-3f && !isotropic) {
			float fp;
			CosineSample(nn, uDir0, uDir1, sampledDir, &fp);
		} else {
			const Vector mu(leaf->mu[k][0], leaf->mu[k][1], leaf->mu[k][2]);
			*sampledDir = VmfSample(mu, leaf->kappa[k], uDir0, uDir1);
		}
	}

	// Exact pdf of the compound distribution we just drew from
	const float d = Dot(*sampledDir, nn);
	float mix = 0.f;
	for (u_int k = 0; k < leaf->nComp; ++k)
		mix += (s[k] / sSum) * LobePdf(leaf->kappa[k],
				sampledDir->x * leaf->mu[k][0] +
				sampledDir->y * leaf->mu[k][1] +
				sampledDir->z * leaf->mu[k][2],
				d, isotropic);
	const float floorPdf = isotropic ? .25f * INV_PI :
			((d > 0.f) ? d * INV_PI : 0.f);
	*pdfW = effFloorW * floorPdf + (1.f - effFloorW) * mix;
	return true;
}

float PathGuidingCache::Pdf(const Point &p, const Normal &n,
		const Vector &dir, const bool isotropic) const {
	const float floorW = isotropic ? FLOOR_W_VOLUME : FLOOR_W_SURFACE;
	const Vector nn(n.x, n.y, n.z);
	const ReadLeaf *leaf = ReadLeafAt(p);
	const float d = Dot(dir, nn);
	const float floorPdf = isotropic ? .25f * INV_PI :
			((d > 0.f) ? d * INV_PI : 0.f);
	if (!leaf || leaf->nComp == 0 || leaf->count < warmup)
		return floorPdf;

	float s[VMF_K];
	const float sSum = CompWeights(*leaf, nn, isotropic, s);
	if (!(sSum > 0.f))
		return floorPdf;

	// Same usable-fraction folding as Sample(): the drawn density and its
	// evaluation must agree for one-sample MIS to stay exact.
	float wSum = 0.f;
	for (u_int k = 0; k < leaf->nComp; ++k)
		wSum += leaf->w[k];
	const float usable = (wSum > 0.f) ? Clamp(sSum / wSum, 0.f, 1.f) : 0.f;
	const float effFloorW = floorW + (1.f - floorW) * (1.f - usable);

	float mix = 0.f;
	for (u_int k = 0; k < leaf->nComp; ++k)
		mix += (s[k] / sSum) * LobePdf(leaf->kappa[k],
				dir.x * leaf->mu[k][0] + dir.y * leaf->mu[k][1] +
				dir.z * leaf->mu[k][2],
				d, isotropic);
	return effFloorW * floorPdf + (1.f - effFloorW) * mix;
}

//------------------------------------------------------------------------------
// GPU flattened-tree snapshot (M4e): the kernel descends the same tree
// the CPU queries and evaluates the fitted vMF mixture itself
//------------------------------------------------------------------------------

void PathGuidingCache::SnapshotTree(vector<u_int> *nodesOut,
		vector<float> *leavesOut) const {
	nodesOut->clear();
	leavesOut->clear();
	const ReadTree *rt = readTree.load(std::memory_order_acquire);

	if (!rt || rt->nodes.empty()) {
		// Canonical empty field: one leaf node pointing at a cold leaf.
		nodesOut->assign(4, ~0u);
		(*nodesOut)[2] = 0u;
		leavesOut->assign(24, 0.f);
		return;
	}

	// Re-emit nodes in DFS order so the root lands at index 0 (the
	// kernel has no root parameter to keep the arg list stable).
	vector<u_int> remap(rt->nodes.size(), ~0u);
	vector<u_int> stack(1, rt->root);
	u_int emit = 0;
	while (!stack.empty()) {
		const u_int src = stack.back();
		stack.pop_back();
		remap[src] = emit++;
		const TreeNode &nd = rt->nodes[src];
		if (nd.axis != ~0u) {
			stack.push_back(nd.child[0]);
			stack.push_back(nd.child[1]);
		}
	}

	nodesOut->resize(rt->nodes.size() * 4);
	for (u_int src = 0; src < rt->nodes.size(); ++src) {
		if (remap[src] == ~0u)
			continue;
		const TreeNode &nd = rt->nodes[src];
		u_int *dst = &(*nodesOut)[remap[src] * 4];
		if (nd.axis == ~0u) {
			dst[0] = dst[1] = ~0u;
			dst[2] = nd.leaf;
			dst[3] = 0u;
		} else {
			dst[0] = remap[nd.child[0]];
			dst[1] = remap[nd.child[1]];
			dst[2] = nd.axis;
			u_int splitBits;
			memcpy(&splitBits, &nd.split, sizeof(u_int));
			dst[3] = splitBits;
		}
	}

	leavesOut->resize(rt->leaves.size() * 24);
	for (u_int i = 0; i < rt->leaves.size(); ++i) {
		const ReadLeaf &rl = rt->leaves[i];
		float *dst = &(*leavesOut)[i * 24];
		for (u_int k = 0; k < VMF_K; ++k) {
			dst[k] = rl.w[k];
			dst[4 + k * 4 + 0] = rl.mu[k][0];
			dst[4 + k * 4 + 1] = rl.mu[k][1];
			dst[4 + k * 4 + 2] = rl.mu[k][2];
			dst[4 + k * 4 + 3] = rl.kappa[k];
		}
		dst[20] = rl.count;
		dst[21] = rl.peak;
		dst[22] = (float)rl.nComp;
		dst[23] = rl.total;
	}
}

//------------------------------------------------------------------------------
// Persistence (v2: tree + leaf mixtures; v1 fine-table files rejected)
//------------------------------------------------------------------------------

bool PathGuidingCache::Save(const std::string &path) const {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const ReadTree *rt = readTree.load(std::memory_order_acquire);
	const u_int magic = 0x47554944u; // GUID
	const u_int version = 3u;
	fwrite(&magic, sizeof(magic), 1, f);
	fwrite(&version, sizeof(version), 1, f);
	fwrite(&cubeMin.x, sizeof(float), 1, f);
	fwrite(&cubeMin.y, sizeof(float), 1, f);
	fwrite(&cubeMin.z, sizeof(float), 1, f);
	fwrite(&cubeSize, sizeof(float), 1, f);
	u_int nodeCount = rt ? (u_int)rt->nodes.size() : 0u;
	u_int leafCount = rt ? (u_int)rt->leaves.size() : 0u;
	u_int root = rt ? rt->root : 0u;
	fwrite(&nodeCount, sizeof(nodeCount), 1, f);
	fwrite(&leafCount, sizeof(leafCount), 1, f);
	fwrite(&root, sizeof(root), 1, f);
	bool ok = true;
	for (u_int i = 0; ok && i < nodeCount; ++i) {
		const TreeNode &nd = rt->nodes[i];
		ok = fwrite(&nd.axis, sizeof(u_int), 1, f) == 1 &&
				fwrite(&nd.split, sizeof(float), 1, f) == 1 &&
				fwrite(&nd.child[0], sizeof(u_int), 1, f) == 1 &&
				fwrite(&nd.child[1], sizeof(u_int), 1, f) == 1 &&
				fwrite(&nd.leaf, sizeof(u_int), 1, f) == 1;
	}
	for (u_int i = 0; ok && i < leafCount; ++i) {
		const ReadLeaf &rl = rt->leaves[i];
		ok = fwrite(&rl.total, sizeof(float), 1, f) == 1 &&
				fwrite(&rl.count, sizeof(float), 1, f) == 1 &&
				fwrite(&rl.nz, sizeof(float), 1, f) == 1 &&
				fwrite(&rl.peak, sizeof(float), 1, f) == 1 &&
				fwrite(&rl.nComp, sizeof(u_int), 1, f) == 1 &&
				fwrite(rl.w, sizeof(float), VMF_K, f) == VMF_K &&
				fwrite(rl.mu, sizeof(float), VMF_K * 3, f) == VMF_K * 3 &&
				fwrite(rl.kappa, sizeof(float), VMF_K, f) == VMF_K;
	}
	fclose(f);
	return ok;
}

PathGuidingCache *PathGuidingCache::Load(const std::string &path,
		const bool freeze, const Settings &s) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return nullptr;
	u_int magic, version;
	float minx, miny, minz, size;
	bool ok = fread(&magic, sizeof(magic), 1, f) == 1 &&
			fread(&version, sizeof(version), 1, f) == 1 &&
			fread(&minx, sizeof(minx), 1, f) == 1 &&
			fread(&miny, sizeof(miny), 1, f) == 1 &&
			fread(&minz, sizeof(minz), 1, f) == 1 &&
			fread(&size, sizeof(size), 1, f) == 1;
	ok = ok && (magic == 0x47554944u) && (version == 3u) && (size > 0.f);
	PathGuidingCache *cache = nullptr;
	if (ok) {
		cache = new PathGuidingCache(Point(minx, miny, minz), size, s);
		u_int nodeCount = 0, leafCount = 0, root = 0;
		ok = fread(&nodeCount, sizeof(nodeCount), 1, f) == 1 &&
				fread(&leafCount, sizeof(leafCount), 1, f) == 1 &&
				fread(&root, sizeof(root), 1, f) == 1 &&
				nodeCount > 0u && leafCount > 0u && root < nodeCount;
		if (ok) {
			ReadTree *rt = new ReadTree();
			rt->nodes.resize(nodeCount);
			rt->leaves.resize(leafCount);
			rt->root = root;
			for (u_int i = 0; ok && i < nodeCount; ++i) {
				TreeNode &nd = rt->nodes[i];
				ok = fread(&nd.axis, sizeof(u_int), 1, f) == 1 &&
						fread(&nd.split, sizeof(float), 1, f) == 1 &&
						fread(&nd.child[0], sizeof(u_int), 1, f) == 1 &&
						fread(&nd.child[1], sizeof(u_int), 1, f) == 1 &&
						fread(&nd.leaf, sizeof(u_int), 1, f) == 1;
			}
			for (u_int i = 0; ok && i < leafCount; ++i) {
				ReadLeaf &rl = rt->leaves[i];
				ok = fread(&rl.total, sizeof(float), 1, f) == 1 &&
						fread(&rl.count, sizeof(float), 1, f) == 1 &&
						fread(&rl.nz, sizeof(float), 1, f) == 1 &&
						fread(&rl.peak, sizeof(float), 1, f) == 1 &&
						fread(&rl.nComp, sizeof(u_int), 1, f) == 1 &&
						fread(rl.w, sizeof(float), VMF_K, f) == VMF_K &&
						fread(rl.mu, sizeof(float), VMF_K * 3, f) == VMF_K * 3 &&
						fread(rl.kappa, sizeof(float), VMF_K, f) == VMF_K;
				rl.nComp = Min(rl.nComp, VMF_K);
			}
			if (ok) {
				delete cache->readTree.load(std::memory_order_relaxed);
				cache->readTree.store(rt, std::memory_order_relaxed);
				// A loaded field replaces online training: the write tree
				// stays a fresh single leaf and swaps never overwrite it.
				// freeze comes resolved from the caller
				// (path.guiding.freeze / LUX_PG_FREEZE=0 opt back into
				// continued training).
				cache->frozen = freeze;
			} else {
				delete rt;
			}
		}
		if (!ok) {
			delete cache;
			cache = nullptr;
		}
	}
	fclose(f);
	if (!ok)
		SLG_LOG("WARNING: unable to load path guiding table v2: " << path);
	return cache;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
